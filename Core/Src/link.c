/**
  ******************************************************************************
  * @file    link.c
  * @brief   Hot-plug-safe UART link to the four adjacent boards.
  *
  * Electrical discipline (the reason this module exists at all):
  *
  *  - RX is always an AF input *with the internal pull-up on*, receiver always
  *    enabled. The pull-up is what makes the dangerous case detectable:
  *      high -> nothing connected, or a powered neighbour idling
  *      low  -> an unpowered board is clamping the line to ~0.7V through its
  *              RX ESD diode. Driving TX into that back-feeds its 3V3 rail and
  *              stops it booting, so we must not drive.
  *    The pull-up sources ~65 uA into a dead board, which is harmless, and it
  *    holds the line at UART idle so a probe burst never looks like a start bit.
  *
  *  - TX is high-Z (GPIO input, no pull) unless the port is UP or mid-probe.
  *    Entering drive with TE already set puts the pin straight to idle-high,
  *    which is the correct entry state; leaving drive waits for TC first.
  *
  * A purely passive "wait until RX proves someone is there" rule deadlocks when
  * both ends run it, so a port with nothing confirmed emits one HELLO every
  * LINK_PROBE_MS and returns to high-Z. At 460800 baud that is ~370 us of drive
  * in 200 ms (~0.2%), far too little to hold a neighbour's rail up: even 1 mA of
  * load collapses ~10 uF within ~26 ms, well inside the gap.
  *
  * Receiving a valid frame is stronger evidence than any level check -- it can
  * only come from a powered, running board -- so it overrides LINK_BLOCKED.
  ******************************************************************************
  */

#include "link.h"
#include "softuart.h"

#include "main.h"
#include "usbd_composite.h"

#include <string.h>

/* --- Tuning ---------------------------------------------------------------- */

#define LINK_PROTO_VERSION      1u

#define LINK_CLEAR_MS           50u    /* RX must be high this long to leave BLOCKED */
#define LINK_PROBE_MS           200u   /* gap between HELLO bursts on an idle port */
#define LINK_PROBE_TIMEOUT_MS   20u    /* give up on a burst that will not drain */
#define LINK_HANDSHAKE_MS       100u   /* peer must ACK within this */
#define LINK_BEAT_MS            50u    /* PING interval once UP */
#define LINK_DEAD_MS            250u   /* silence this long tears the link down */

#define LINK_RX_RING            256u   /* must be a power of two */
#define LINK_TX_RING            256u   /* must be a power of two; holds a 98-byte colour frame */

/* --- Port hardware map ------------------------------------------------------
 * Sides as wired on the PCB. TOP has no USART bonded out for it; uart == NULL
 * routes it to the timer-driven software UART in softuart.c, which feeds the
 * same rings, so everything above the driver seam below is shared. */

typedef struct {
  USART_TypeDef *uart;      /* NULL = software UART (softuart.c) */
  IRQn_Type      irqn;
  GPIO_TypeDef  *tx_port;
  uint16_t       tx_pin;
  uint8_t        tx_af;
  GPIO_TypeDef  *rx_port;
  uint16_t       rx_pin;
} link_hw_t;

static const link_hw_t link_hw[LINK_PORT_COUNT] = {
  /* TOP    */ { NULL,    0,             GPIOB, GPIO_PIN_4,    GPIO_AF1_TIM16,    GPIOB, GPIO_PIN_5    },
  /* BOTTOM */ { LPUART1, LPUART1_IRQn,  GPIOA, GPIO_PIN_2,    GPIO_AF12_LPUART1, GPIOA, GPIO_PIN_3    },
  /* LEFT   */ { USART2,  USART2_IRQn,   GPIOB, GPIO_PIN_3,    GPIO_AF7_USART2,   GPIOA, GPIO_PIN_15   },
  /* RIGHT  */ { USART1,  USART1_IRQn,   GPIOA, GPIO_PIN_9,    GPIO_AF7_USART1,   GPIOA, GPIO_PIN_10   },
};

static const char *const link_port_names[LINK_PORT_COUNT] = {
  "top", "bottom", "left", "right"
};

static const char *const link_state_names[] = {
  "BLOCKED", "DOWN", "PROBE", "HANDSHAKE", "UP"
};

/* --- Per-port state --------------------------------------------------------- */

typedef struct {
  link_state_t state;
  uint32_t t_state;       /* tick at state entry */
  uint32_t t_rx_low;      /* tick RX was last seen low */
  uint32_t t_last_rx;     /* tick of the last valid frame */
  uint32_t t_next_probe;
  uint32_t t_last_beat;
  uint8_t  tx_driven;

  volatile uint8_t  rx_buf[LINK_RX_RING];
  volatile uint16_t rx_head, rx_tail;
  volatile uint8_t  tx_buf[LINK_TX_RING];
  volatile uint16_t tx_head, tx_tail;

  /* frame parser */
  uint8_t  ps;            /* 0 magic0, 1 magic1, 2 type, 3 len, 4 payload, 5 sum */
  uint8_t  p_type, p_len, p_count, p_sum;
  uint8_t  p_buf[LINK_MAX_PAYLOAD];

  /* peer, valid while UP */
  uint32_t peer_uid[3];
  uint8_t  peer_fw, peer_port, peer_has_usb;

  uint32_t n_rx, n_tx, n_err;
  /* n_err is the total; these say WHICH, because the causes want different fixes
   * and a single number cannot tell them apart. n_rxfull means the main loop
   * fell behind draining the ring, n_txfull means we offered frames faster than
   * the port could send them, and n_sum is a corrupt or resynced frame.
   *
   * The line itself is split in two, because ORE and FE/NE/PE were previously
   * one `n_uart` and they point in opposite directions. n_frame is the WIRE:
   * framing, noise or parity, i.e. the signal arrived wrong -- a connector,
   * crosstalk, the hardware note's series resistors. n_ore is US: the receive
   * ISR did not read RDR before the next byte landed on top of it, which at
   * 460800 baud is a 17.4 us budget, so it indicts interrupt latency rather
   * than the cable. Anything at preempt priority 0 that runs longer than a byte
   * time -- the LED DMA completion handler's dma_sum() over 747 words is the
   * one to suspect -- produces ORE on a perfectly good link. */
  uint32_t n_ore, n_frame, n_rxfull, n_txfull, n_sum;
} link_t;

static link_t link[LINK_PORT_COUNT];
static uint8_t self_has_usb;
static link_rx_handler_t   rx_handler;
static link_down_handler_t down_handler;

/* --- TX pin gating ---------------------------------------------------------- */

static void link_tx_highz(const link_hw_t *hw)
{
  GPIO_InitTypeDef g = {0};
  g.Pin  = hw->tx_pin;
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(hw->tx_port, &g);
}

static void link_tx_drive(const link_hw_t *hw)
{
  GPIO_InitTypeDef g = {0};
  g.Pin       = hw->tx_pin;
  g.Mode      = GPIO_MODE_AF_PP;
  g.Pull      = GPIO_NOPULL;
  g.Speed     = GPIO_SPEED_FREQ_HIGH;
  g.Alternate = hw->tx_af;
  HAL_GPIO_Init(hw->tx_port, &g);
}

static void link_drive_on(link_port_t p)
{
  link_t *L = &link[p];
  if (L->tx_driven) return;
  if (link_hw[p].uart) link_hw[p].uart->ICR = USART_ICR_TCCF;
  link_tx_drive(&link_hw[p]);
  L->tx_driven = 1;
}

static void link_drive_off(link_port_t p)
{
  link_t *L = &link[p];
  if (!L->tx_driven) return;
  link_tx_highz(&link_hw[p]);
  L->tx_driven = 0;
}

/* --- Driver seam --------------------------------------------------------------
 * The three places the state machine touches the transmitter differ between a
 * USART and the software UART. Everything else -- rings, parser, gating -- is
 * shared, and the ISRs on both sides follow the same ring discipline. */

static void link_hw_tx_kick(link_port_t p)
{
  USART_TypeDef *u = link_hw[p].uart;
  if (u) u->CR1 |= USART_CR1_TXEIE;
  else   softuart_tx_kick();
}

/* Ring drained and the last stop bit has left the pin. */
static uint8_t link_hw_tx_done(link_port_t p)
{
  const link_t *L = &link[p];
  USART_TypeDef *u = link_hw[p].uart;
  if (L->tx_head != L->tx_tail) return 0;
  return u ? ((u->ISR & USART_ISR_TC) != 0) : softuart_tx_idle();
}

static void link_hw_fence(link_port_t p, uint8_t on)
{
  const link_hw_t *hw = &link_hw[p];
  if (hw->uart == NULL) { softuart_irq_mask(on); return; }
  if (on) HAL_NVIC_DisableIRQ(hw->irqn);
  else    HAL_NVIC_EnableIRQ(hw->irqn);
}

/* --- Interrupt handlers ------------------------------------------------------ */

static void link_rx_put(link_t *L, uint8_t c)
{
  uint16_t next = (uint16_t)((L->rx_head + 1u) & (LINK_RX_RING - 1u));
  if (next != L->rx_tail) {
    L->rx_buf[L->rx_head] = c;
    L->rx_head = next;
  } else {
    L->n_err++;   /* main loop fell behind; drop rather than stall */
    L->n_rxfull++;
  }
}

static void link_isr(link_port_t p)
{
  link_t *L = &link[p];
  USART_TypeDef *u = link_hw[p].uart;
  uint32_t isr = u->ISR;

  if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) {
    u->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
    L->n_err++;
    /* Counted apart: ORE is our latency, FE/NE/PE is the wire. Both can be set
     * in one ISR entry, so both are counted rather than one winning. */
    if (isr & USART_ISR_ORE)                                    L->n_ore++;
    if (isr & (USART_ISR_FE | USART_ISR_NE | USART_ISR_PE))      L->n_frame++;
  }

  if (isr & USART_ISR_RXNE) link_rx_put(L, (uint8_t)u->RDR);

  if ((isr & USART_ISR_TXE) && (u->CR1 & USART_CR1_TXEIE)) {
    if (L->tx_tail != L->tx_head) {
      u->TDR = L->tx_buf[L->tx_tail];
      L->tx_tail = (uint16_t)((L->tx_tail + 1u) & (LINK_TX_RING - 1u));
    } else {
      u->CR1 &= ~USART_CR1_TXEIE;   /* TC follows; the tick loop waits on it */
    }
  }
}

void link_irq_lpuart1(void) { link_isr(LINK_PORT_BOTTOM); }
void link_irq_usart2(void)  { link_isr(LINK_PORT_LEFT); }
void link_irq_usart1(void)  { link_isr(LINK_PORT_RIGHT); }

/* Software UART hooks (TOP), from the DMA interrupts in softuart.c. */

uint8_t link_soft_tx_pop(uint8_t *c)
{
  link_t *L = &link[LINK_PORT_TOP];
  if (L->tx_tail == L->tx_head) return 0;
  *c = L->tx_buf[L->tx_tail];
  L->tx_tail = (uint16_t)((L->tx_tail + 1u) & (LINK_TX_RING - 1u));
  return 1;
}

void link_soft_rx_push(uint8_t c)
{
  link_rx_put(&link[LINK_PORT_TOP], c);
}

void link_soft_rx_error(void)
{
  link[LINK_PORT_TOP].n_err++;
}

/* --- Sending ----------------------------------------------------------------- */

int link_send(link_port_t p, uint8_t type, const uint8_t *payload, uint8_t len)
{
  if (p >= LINK_PORT_COUNT || len > LINK_MAX_PAYLOAD) return 0;

  link_t *L = &link[p];

  uint16_t need = (uint16_t)(5u + len);
  uint16_t used = (uint16_t)((L->tx_head - L->tx_tail) & (LINK_TX_RING - 1u));
  if (used + need >= LINK_TX_RING) {
    L->n_err++;
    L->n_txfull++;
    return 0;   /* drop rather than block the scan loop */
  }

  uint16_t h = L->tx_head;
  uint8_t sum = (uint8_t)(type + len);

  L->tx_buf[h] = 0xA5;  h = (uint16_t)((h + 1u) & (LINK_TX_RING - 1u));
  L->tx_buf[h] = 0x5A;  h = (uint16_t)((h + 1u) & (LINK_TX_RING - 1u));
  L->tx_buf[h] = type;  h = (uint16_t)((h + 1u) & (LINK_TX_RING - 1u));
  L->tx_buf[h] = len;   h = (uint16_t)((h + 1u) & (LINK_TX_RING - 1u));
  for (uint8_t i = 0; i < len; i++) {
    sum = (uint8_t)(sum + payload[i]);
    L->tx_buf[h] = payload[i];
    h = (uint16_t)((h + 1u) & (LINK_TX_RING - 1u));
  }
  L->tx_buf[h] = sum;   h = (uint16_t)((h + 1u) & (LINK_TX_RING - 1u));

  L->tx_head = h;
  link_hw_tx_kick(p);
  L->n_tx++;
  return 1;
}

static uint8_t link_build_hello(link_port_t p, uint8_t *out)
{
  uint32_t uid[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
  memcpy(out, uid, sizeof(uid));
  out[12] = LINK_PROTO_VERSION;
  out[13] = (uint8_t)p;
  out[14] = link_self_has_usb();
  return 15;
}

static void link_send_hello(link_port_t p, uint8_t type)
{
  uint8_t pay[15];
  uint8_t n = link_build_hello(p, pay);
  link_send(p, type, pay, n);
}

/* --- Link teardown ----------------------------------------------------------- */

/* Returning TX to high-Z here is load-bearing, not tidiness: leave it driven
 * after a neighbour is unplugged and the *next* hot-plug fails exactly the way
 * this module exists to prevent. */
static void link_teardown(link_port_t p, uint32_t tick)
{
  link_t *L = &link[p];
  const link_hw_t *hw = &link_hw[p];
  uint8_t was_up = (L->state == LINK_UP);
  link_drive_off(p);

  /* Fence the ISRs out before touching both ends of a ring: caught mid-update
   * they would write back a stale index and the ring would look full forever. */
  link_hw_fence(p, 1);
  if (hw->uart) hw->uart->CR1 &= ~USART_CR1_TXEIE;
  else          softuart_reset();
  L->tx_head = L->tx_tail = 0;
  L->rx_head = L->rx_tail = 0;
  link_hw_fence(p, 0);

  L->ps = 0;
  L->peer_uid[0] = L->peer_uid[1] = L->peer_uid[2] = 0;
  L->peer_fw = L->peer_port = L->peer_has_usb = 0;
  L->state = LINK_DOWN;
  L->t_state = tick;
  L->t_next_probe = tick + LINK_PROBE_MS;

  /* After the state is settled, so the handler sees a consistent view. */
  if (was_up && down_handler) down_handler(p);
}

/* --- Frame handling ---------------------------------------------------------- */

static void link_on_frame(link_port_t p, uint32_t tick)
{
  link_t *L = &link[p];
  L->t_last_rx = tick;
  L->n_rx++;

  switch (L->p_type) {
  case LINK_MSG_HELLO:
  case LINK_MSG_HELLO_ACK:
    if (L->p_len >= 15) {
      memcpy(L->peer_uid, L->p_buf, sizeof(L->peer_uid));
      L->peer_fw       = L->p_buf[12];
      L->peer_port     = L->p_buf[13];
      L->peer_has_usb  = L->p_buf[14];
    }
    /* A valid frame proves the far board is powered and running, so driving
     * TX is now unconditionally safe -- this overrides LINK_BLOCKED. */
    link_drive_on(p);
    if (L->p_type == LINK_MSG_HELLO) {
      /* Always answer a HELLO, including while UP: it means the peer rebooted
       * and is waiting for an ACK to come back up. */
      link_send_hello(p, LINK_MSG_HELLO_ACK);
      if (L->state != LINK_UP) {
        L->state = LINK_HANDSHAKE;
        L->t_state = tick;
      }
    } else if (L->state != LINK_UP) {
      /* Our probe was answered. ACK back once so the peer leaves HANDSHAKE
       * too; a peer already UP treats that as a plain refresh, so this
       * terminates rather than ping-ponging. */
      link_send_hello(p, LINK_MSG_HELLO_ACK);
      L->state = LINK_UP;
      L->t_state = tick;
      L->t_last_beat = tick;
    }
    break;

  case LINK_MSG_PING:
    /* The peer is already treating this link as up, so our ACK got through
     * even if its ACK did not. Skip the pointless handshake timeout. */
    if (L->state == LINK_HANDSHAKE) {
      L->state = LINK_UP;
      L->t_state = tick;
      L->t_last_beat = tick;
    }
    break;

  default:
    /* Not ours: hand it to the mesh layer. t_last_rx is already updated above,
     * so any application traffic also counts as a keepalive. */
    if (rx_handler) rx_handler(p, L->p_type, L->p_buf, L->p_len);
    break;
  }
}

static void link_feed(link_port_t p, uint8_t c, uint32_t tick)
{
  link_t *L = &link[p];

  switch (L->ps) {
  case 0:
    if (c == 0xA5) L->ps = 1;
    break;
  case 1:
    /* 0xA5 0xA5 is a valid resync: keep waiting for the 0x5A. */
    L->ps = (c == 0x5A) ? 2 : ((c == 0xA5) ? 1 : 0);
    break;
  case 2:
    L->p_type = c;
    L->p_sum = c;
    L->ps = 3;
    break;
  case 3:
    if (c > LINK_MAX_PAYLOAD) { L->n_err++; L->n_sum++; L->ps = 0; break; }
    L->p_len = c;
    L->p_sum = (uint8_t)(L->p_sum + c);
    L->p_count = 0;
    L->ps = (c == 0) ? 5 : 4;
    break;
  case 4:
    L->p_buf[L->p_count++] = c;
    L->p_sum = (uint8_t)(L->p_sum + c);
    if (L->p_count >= L->p_len) L->ps = 5;
    break;
  case 5:
    if (c == L->p_sum) link_on_frame(p, tick);
    else               { L->n_err++; L->n_sum++; }
    L->ps = 0;
    break;
  default:
    L->ps = 0;
    break;
  }
}

/* --- State machine ------------------------------------------------------------ */

static uint32_t link_probe_jitter(link_port_t p)
{
  /* Spread probes across boards so two idle ports never lockstep and talk
   * over each other forever. */
  return ((HAL_GetUIDw0() >> (p * 2u)) & 0x3Fu);
}

static void link_port_tick(link_port_t p, uint32_t tick)
{
  const link_hw_t *hw = &link_hw[p];
  link_t *L = &link[p];

  if (hw->uart == NULL) softuart_rx_poll();   /* TOP: captured edges -> the ring */

  while (L->rx_tail != L->rx_head) {
    uint8_t c = L->rx_buf[L->rx_tail];
    L->rx_tail = (uint16_t)((L->rx_tail + 1u) & (LINK_RX_RING - 1u));
    link_feed(p, c, tick);
  }

  /* The RX level only means anything when the line is otherwise idle; once UP
   * it spends most of its time low carrying data. */
  if (L->state == LINK_BLOCKED || L->state == LINK_DOWN) {
    if (HAL_GPIO_ReadPin(hw->rx_port, hw->rx_pin) == GPIO_PIN_RESET) {
      L->t_rx_low = tick;
      if (L->state == LINK_DOWN) {
        L->state = LINK_BLOCKED;
        L->t_state = tick;
      }
    }
  }

  switch (L->state) {
  case LINK_BLOCKED:
    if (tick - L->t_rx_low >= LINK_CLEAR_MS) {
      L->state = LINK_DOWN;
      L->t_state = tick;
      L->t_next_probe = tick + link_probe_jitter(p);
    }
    break;

  case LINK_DOWN:
    if ((int32_t)(tick - L->t_next_probe) >= 0) {
      link_drive_on(p);               /* idle-high first, then the frame */
      link_send_hello(p, LINK_MSG_HELLO);
      L->state = LINK_PROBE;
      L->t_state = tick;
    }
    break;

  case LINK_PROBE:
    if (link_hw_tx_done(p) || (tick - L->t_state > LINK_PROBE_TIMEOUT_MS)) {
      link_drive_off(p);
      L->state = LINK_DOWN;
      L->t_state = tick;
      L->t_next_probe = tick + LINK_PROBE_MS + link_probe_jitter(p);
    }
    break;

  case LINK_HANDSHAKE:
    if (tick - L->t_state > LINK_HANDSHAKE_MS) link_teardown(p, tick);
    break;

  case LINK_UP:
    if (tick - L->t_last_rx > LINK_DEAD_MS) {
      link_teardown(p, tick);
    } else if (tick - L->t_last_beat >= LINK_BEAT_MS) {
      L->t_last_beat = tick;
      link_send(p, LINK_MSG_PING, NULL, 0);
    }
    break;

  default:
    link_teardown(p, tick);
    break;
  }
}

/* --- Public API ---------------------------------------------------------------- */

void link_init(void)
{
  uint32_t tick = HAL_GetTick();

  for (link_port_t p = 0; p < LINK_PORT_COUNT; p++) {
    link_t *L = &link[p];
    memset(L, 0, sizeof(*L));
    L->state = LINK_BLOCKED;   /* never drive before the first RX sample */
    L->t_state = tick;
    L->t_rx_low = tick;
    L->t_last_rx = tick;

    const link_hw_t *hw = &link_hw[p];
    if (hw->uart == NULL) {
      softuart_init();      /* RX captures from here on; the TX pin stays ours */
      link_tx_highz(hw);
      continue;
    }

    link_tx_highz(hw);   /* re-assert; HAL_UART_MspInit already did this */
    hw->uart->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF |
                    USART_ICR_PECF | USART_ICR_TCCF;
    (void)hw->uart->RDR;
    hw->uart->CR1 |= USART_CR1_RXNEIE;
  }
}

void link_tick(uint32_t tick)
{
  if (USBD_MIDI_Ready()) self_has_usb = 1;   /* latched: a role must not flap */

  for (link_port_t p = 0; p < LINK_PORT_COUNT; p++) {
    link_port_tick(p, tick);
  }
}

link_state_t link_state(link_port_t p)
{
  return (p < LINK_PORT_COUNT) ? link[p].state : LINK_DOWN;
}

const char *link_state_name(link_port_t p)
{
  if (p >= LINK_PORT_COUNT) return "?";
  return link_state_names[link[p].state];
}

char link_state_char(link_port_t p)
{
  static const char chars[] = { 'b', '.', 'p', 'h', 'U' };
  if (p >= LINK_PORT_COUNT) return '?';
  return chars[link[p].state];
}

const char *link_port_name(link_port_t p)
{
  return (p < LINK_PORT_COUNT) ? link_port_names[p] : "?";
}

const uint32_t *link_peer_uid(link_port_t p)
{
  static const uint32_t none[3] = {0, 0, 0};
  return (p < LINK_PORT_COUNT) ? link[p].peer_uid : none;
}

uint8_t link_peer_port(link_port_t p)
{
  return (p < LINK_PORT_COUNT) ? link[p].peer_port : 0;
}

uint8_t link_peer_has_usb(link_port_t p)
{
  return (p < LINK_PORT_COUNT) ? link[p].peer_has_usb : 0;
}

void link_stats(link_port_t p, uint32_t *rx, uint32_t *tx, uint32_t *err)
{
  if (p >= LINK_PORT_COUNT) return;
  if (rx)  *rx  = link[p].n_rx;
  if (tx)  *tx  = link[p].n_tx;
  if (err) *err = link[p].n_err;
}

void link_err_breakdown(link_port_t p, uint32_t *ore, uint32_t *frame,
                        uint32_t *rxfull, uint32_t *txfull, uint32_t *sum)
{
  if (p >= LINK_PORT_COUNT) return;
  if (ore)    *ore    = link[p].n_ore;
  if (frame)  *frame  = link[p].n_frame;
  if (rxfull) *rxfull = link[p].n_rxfull;
  if (txfull) *txfull = link[p].n_txfull;
  if (sum)    *sum    = link[p].n_sum;
}

void link_set_rx_handler(link_rx_handler_t cb)
{
  rx_handler = cb;
}

void link_set_down_handler(link_down_handler_t cb)
{
  down_handler = cb;
}

uint8_t link_self_has_usb(void)
{
  return self_has_usb;
}
