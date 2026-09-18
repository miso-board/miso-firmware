/**
  ******************************************************************************
  * @file    softuart.c
  * @brief   Software UART for the TOP link port (TX PB4, RX PB5).
  *
  * See softuart.h for the shape of it. Two details that are load-bearing:
  *
  *  - TX bursts end with two idle words. A CCR1 value transferred at update k
  *    drives the pin during period k+1, so without them the DMA would report
  *    complete while the stop bit was still on the wire and link.c would put
  *    the pin back to high-Z half a bit early.
  *
  *  - RX decoding needs the current line level to know which edges are
  *    falling (softuart_decode.h). Reading the pin races the DMA, which lags
  *    each capture by a few bus cycles, so the level is taken from the
  *    timer instead: CH1 captures rising edges only, CH2 captures both, and
  *    the last edge was rising exactly when the two hold the same timestamp.
  ******************************************************************************
  */

#include "softuart.h"
#include "softuart_decode.h"

#include "main.h"

/* --- Hardware map ------------------------------------------------------------ */

#define SU_TX_TIM       TIM16               /* CH1 -> PB4, AF1 */
#define SU_RX_TIM       TIM3                /* TI2 <- PB5, AF2 */
#define SU_TX_DMA_CH    DMA1_Channel2
#define SU_RX_DMA_CH    DMA1_Channel3
#define SU_TX_DMA_IRQ   DMA1_Channel2_IRQn
#define SU_RX_DMA_IRQ   DMA1_Channel3_IRQn
#define SU_RX_TIM_IRQ   TIM3_IRQn

/* --- Tuning ------------------------------------------------------------------ */

#define SU_RX_TPB       8u     /* capture ticks per bit: 1/16-bit sampling error, 17.7 ms wrap */
#define SU_RX_EDGES     256u   /* power of two. Half of it is the latency budget for the
                                * backstop interrupt at full line rate: 128 edges >= 277 us. */
#define SU_TX_CHUNK     32u    /* bytes per DMA burst; one interrupt per chunk while sending */
#define SU_TX_WORDS     (SU_TX_CHUNK * 10u + 2u)
#define SU_GUARD_TICKS  8192u  /* decoder backstop period: 2.2 ms, well inside the wrap */

/* --- State ------------------------------------------------------------------- */

static DMA_HandleTypeDef su_tx_dma;
static DMA_HandleTypeDef su_rx_dma;

static uint16_t          su_tx_words[SU_TX_WORDS];
static volatile uint16_t su_edges[SU_RX_EDGES];
static uint16_t          su_rd;        /* decoder's read index into su_edges */
static uint16_t          su_high;      /* CCR1 value that holds the pin high for a whole bit */
static volatile uint8_t  su_tx_busy;
static uint8_t           su_ready;

/* --- TX ---------------------------------------------------------------------- */

/* Encode up to a chunk of bytes from link's ring and start the DMA. Runs only
 * from the TX DMA interrupt, so it never races itself. */
static void su_tx_start_burst(void)
{
  uint16_t n = 0;
  uint8_t c;

  while (n + 10u <= SU_TX_CHUNK * 10u && link_soft_tx_pop(&c)) {
    su_tx_words[n++] = 0;                                              /* start */
    for (uint8_t b = 0; b < 8u; b++) {
      su_tx_words[n++] = (c & (1u << b)) ? su_high : 0;                /* LSB first */
    }
    su_tx_words[n++] = su_high;                                        /* stop */
  }
  if (n == 0) {
    su_tx_busy = 0;
    return;
  }

  /* Pads: complete fires when these have been transferred, i.e. after the
   * stop bit's period has ended, not when it has merely been queued. */
  su_tx_words[n++] = su_high;
  su_tx_words[n++] = su_high;

  su_tx_busy = 1;
  HAL_DMA_Start_IT(&su_tx_dma, (uint32_t)su_tx_words, (uint32_t)&SU_TX_TIM->CCR1, n);
  SU_TX_TIM->DIER |= TIM_DIER_UDE;
}

static void su_tx_burst_done(DMA_HandleTypeDef *h)
{
  (void)h;
  SU_TX_TIM->DIER &= ~TIM_DIER_UDE;
  su_tx_busy = 0;
}

void softuart_irq_tx_dma(void)
{
  HAL_DMA_IRQHandler(&su_tx_dma);          /* transfer complete -> su_tx_burst_done */
  if (!su_tx_busy) su_tx_start_burst();    /* next chunk, or a kick from link_send */
}

void softuart_tx_kick(void)
{
  /* Start (or continue) from the ISR rather than here: the ring is consumed
   * only in that one context, so there is nothing to fence. */
  if (su_ready) NVIC_SetPendingIRQ(SU_TX_DMA_IRQ);
}

uint8_t softuart_tx_idle(void)
{
  return !su_tx_busy;
}

/* --- RX ---------------------------------------------------------------------- */

static void su_rx_dma_event(DMA_HandleTypeDef *h)
{
  (void)h;   /* nothing to do: the ISR below decodes whatever the reason it ran */
}

static void su_rx_decode(void)
{
  uint16_t wr, last, last_rise;

  /* Snapshot the ring's write index together with the level after the last
   * captured edge. CCR2 holds that edge's time regardless of the DMA, so
   * waiting for its slot to hold the same value means the DMA has caught up
   * and the count of edges after any candidate is exact. Bounded, so a DMA
   * that has stopped for any reason degrades to a stale level, not a hang. */
  for (uint8_t tries = 0; ; tries++) {
    last      = (uint16_t)SU_RX_TIM->CCR2;
    last_rise = (uint16_t)SU_RX_TIM->CCR1;
    wr = (uint16_t)(SU_RX_EDGES - __HAL_DMA_GET_COUNTER(&su_rx_dma));
    if (su_edges[(wr - 1u) & (SU_RX_EDGES - 1u)] == last || tries >= 8u) break;
  }
  uint8_t level = (last == last_rise);   /* also true before any edge: both 0, line idle-high */
  uint16_t now  = (uint16_t)SU_RX_TIM->CNT;

  uint8_t c;
  int r;
  while ((r = su_decode_next(su_edges, SU_RX_EDGES - 1u, &su_rd, wr, now, level,
                             SU_RX_TPB, &c)) != SU_DECODE_WAIT) {
    if (r == SU_DECODE_BYTE)         link_soft_rx_push(c);
    else if (r == SU_DECODE_FRAMING) link_soft_rx_error();
  }
}

void softuart_irq_rx_dma(void)
{
  HAL_DMA_IRQHandler(&su_rx_dma);   /* clears half/complete; also entered by softuart_rx_poll */
  su_rx_decode();
}

void softuart_irq_rx_timer(void)
{
  if (SU_RX_TIM->SR & TIM_SR_CC3IF) {
    SU_RX_TIM->SR = (uint16_t)~TIM_SR_CC3IF;
    SU_RX_TIM->CCR3 = (uint16_t)(SU_RX_TIM->CCR3 + SU_GUARD_TICKS);
  }
  su_rx_decode();
}

void softuart_rx_poll(void)
{
  /* Decode in the DMA ISR's context, at the same priority as TIM3's, so the
   * three triggers serialise without masking anything. */
  if (su_ready) NVIC_SetPendingIRQ(SU_RX_DMA_IRQ);
}

/* --- Teardown ------------------------------------------------------------------ */

void softuart_irq_mask(uint8_t masked)
{
  if (!su_ready) return;
  if (masked) {
    HAL_NVIC_DisableIRQ(SU_TX_DMA_IRQ);
    HAL_NVIC_DisableIRQ(SU_RX_DMA_IRQ);
    HAL_NVIC_DisableIRQ(SU_RX_TIM_IRQ);
  } else {
    HAL_NVIC_EnableIRQ(SU_TX_DMA_IRQ);
    HAL_NVIC_EnableIRQ(SU_RX_DMA_IRQ);
    HAL_NVIC_EnableIRQ(SU_RX_TIM_IRQ);
  }
}

void softuart_reset(void)
{
  if (!su_ready) return;

  SU_TX_TIM->DIER &= ~TIM_DIER_UDE;
  if (su_tx_dma.State == HAL_DMA_STATE_BUSY) HAL_DMA_Abort(&su_tx_dma);
  SU_TX_TIM->CCR1 = su_high;
  SU_TX_TIM->EGR  = TIM_EGR_UG;     /* through the preload now, not at the next update */
  su_tx_busy = 0;

  /* Pending edges belong to the link being torn down; drop them. */
  su_rd = (uint16_t)(SU_RX_EDGES - __HAL_DMA_GET_COUNTER(&su_rx_dma));
}

/* --- Init ---------------------------------------------------------------------- */

static uint32_t su_timer_clock(uint32_t pclk, uint32_t apb_div)
{
  /* Timer kernel clock is PCLK, or twice it when the APB prescaler is not 1. */
  return (apb_div == RCC_HCLK_DIV1) ? pclk : pclk * 2u;
}

void softuart_init(void)
{
  RCC_ClkInitTypeDef clk;
  uint32_t latency;
  HAL_RCC_GetClockConfig(&clk, &latency);
  uint32_t tx_clk = su_timer_clock(HAL_RCC_GetPCLK2Freq(), clk.APB2CLKDivider);   /* TIM16 */
  uint32_t rx_clk = su_timer_clock(HAL_RCC_GetPCLK1Freq(), clk.APB1CLKDivider);   /* TIM3  */

  /* RX pin: capture input with the internal pull-up, for the same reasons the
   * hardware ports' RX pins have it (see link.c). The TX pin is link.c's:
   * high-Z until a live neighbour is heard, then AF1 = TIM16_CH1. */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef g = {0};
  g.Pin       = GPIO_PIN_5;
  g.Mode      = GPIO_MODE_AF_PP;
  g.Pull      = GPIO_PULLUP;
  g.Speed     = GPIO_SPEED_FREQ_HIGH;
  g.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(GPIOB, &g);

  /* --- TX: TIM16, PWM mode 1, one period per bit ----------------------------- */
  __HAL_RCC_TIM16_CLK_ENABLE();
  uint32_t period = (tx_clk + SOFTUART_BAUD / 2u) / SOFTUART_BAUD;   /* 313 at 144 MHz */
  su_high = (uint16_t)period;               /* CNT runs 0..period-1 and never reaches it */

  SU_TX_TIM->CR1   = 0;
  SU_TX_TIM->PSC   = 0;
  SU_TX_TIM->ARR   = period - 1u;
  SU_TX_TIM->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE;   /* PWM1, preloaded */
  SU_TX_TIM->CCER  = TIM_CCER_CC1E;
  SU_TX_TIM->CCR1  = su_high;
  SU_TX_TIM->BDTR  = TIM_BDTR_MOE;
  SU_TX_TIM->EGR   = TIM_EGR_UG;
  SU_TX_TIM->SR    = 0;
  SU_TX_TIM->CR1   = TIM_CR1_CEN | TIM_CR1_ARPE;

  su_tx_dma.Instance                 = SU_TX_DMA_CH;
  su_tx_dma.Init.Request             = DMA_REQUEST_TIM16_UP;
  su_tx_dma.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  su_tx_dma.Init.PeriphInc           = DMA_PINC_DISABLE;
  su_tx_dma.Init.MemInc              = DMA_MINC_ENABLE;
  su_tx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;       /* as the LED driver: */
  su_tx_dma.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;   /* 16-bit words into CCR1 */
  su_tx_dma.Init.Mode                = DMA_NORMAL;
  su_tx_dma.Init.Priority            = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&su_tx_dma) != HAL_OK) Error_Handler();
  su_tx_dma.XferCpltCallback = su_tx_burst_done;

  /* --- RX: TIM3 free-running, edge capture into a circular ring -------------- */
  __HAL_RCC_TIM3_CLK_ENABLE();
  uint32_t tick_hz = SOFTUART_BAUD * SU_RX_TPB;

  SU_RX_TIM->CR1   = 0;
  SU_RX_TIM->PSC   = (uint16_t)((rx_clk + tick_hz / 2u) / tick_hz - 1u);   /* 38 at 144 MHz */
  SU_RX_TIM->ARR   = 0xFFFFu;
  /* IC2 <- TI2 both edges (the DMA stream); IC1 <- TI2 rising only (level
   * reference, read directly). Both through the 8-sample (~55 ns) filter. */
  SU_RX_TIM->CCMR1 = TIM_CCMR1_CC2S_0 | (3u << TIM_CCMR1_IC2F_Pos) |
                     TIM_CCMR1_CC1S_1 | (3u << TIM_CCMR1_IC1F_Pos);
  SU_RX_TIM->CCMR2 = 0;                                          /* CH3: compare, interrupt only */
  SU_RX_TIM->CCER  = TIM_CCER_CC2E | TIM_CCER_CC2P | TIM_CCER_CC2NP | TIM_CCER_CC1E;
  SU_RX_TIM->CCR3  = SU_GUARD_TICKS;
  SU_RX_TIM->EGR   = TIM_EGR_UG;
  SU_RX_TIM->SR    = 0;

  su_rx_dma.Instance                 = SU_RX_DMA_CH;
  su_rx_dma.Init.Request             = DMA_REQUEST_TIM3_CH2;
  su_rx_dma.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  su_rx_dma.Init.PeriphInc           = DMA_PINC_DISABLE;
  su_rx_dma.Init.MemInc              = DMA_MINC_ENABLE;
  su_rx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  su_rx_dma.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;   /* low half of CCR2 */
  su_rx_dma.Init.Mode                = DMA_CIRCULAR;
  su_rx_dma.Init.Priority            = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&su_rx_dma) != HAL_OK) Error_Handler();
  su_rx_dma.XferHalfCpltCallback = su_rx_dma_event;   /* non-NULL so the HAL enables HT */
  su_rx_dma.XferCpltCallback     = su_rx_dma_event;
  su_rd = 0;
  if (HAL_DMA_Start_IT(&su_rx_dma, (uint32_t)&SU_RX_TIM->CCR2,
                       (uint32_t)su_edges, SU_RX_EDGES) != HAL_OK) Error_Handler();

  SU_RX_TIM->DIER = TIM_DIER_CC2DE | TIM_DIER_CC3IE;
  SU_RX_TIM->CR1  = TIM_CR1_CEN;

  /* Same level as the hardware link UARTs: below USB and the LED DMA, above
   * SysTick. RX DMA and TIM3 share it so the decoder never runs re-entrantly. */
  HAL_NVIC_SetPriority(SU_TX_DMA_IRQ, 1, 0);
  HAL_NVIC_SetPriority(SU_RX_DMA_IRQ, 1, 0);
  HAL_NVIC_SetPriority(SU_RX_TIM_IRQ, 1, 0);
  su_ready = 1;
  softuart_irq_mask(0);
}
