/**
  ******************************************************************************
  * @file    softuart.h
  * @brief   Software UART for the TOP link port (TX PB4, RX PB5).
  *
  * The G431KBU6 bonds out three full-duplex UART pairs and the other three
  * sides have them. This one is built from two timers and two DMA channels so
  * that no interrupt fires per bit in either direction; the Hall scan loop
  * never sees it.
  *
  *   TX  TIM16 runs in PWM mode 1 with one period per bit. CCR1 = 0 holds the
  *       pin low for the whole period and CCR1 > ARR holds it high, so a byte
  *       is ten CCR1 values. A DMA transfer on every update event writes the
  *       next one; the CPU only encodes the next chunk when a burst finishes.
  *
  *   RX  TIM3 free-runs and captures the timestamp of every edge on PB5 into
  *       a circular DMA ring. softuart_decode.h turns timestamps into bytes;
  *       it runs from the link tick and, as a backstop, from the DMA half /
  *       complete interrupts and a periodic compare on TIM3.
  *
  * Bytes come from and go to link.c's per-port rings through the three
  * link_soft_* hooks below, so the state machine, frame parser and hot-plug
  * gating in link.c are identical for all four ports.
  *
  * Configured at register level rather than through CubeMX so regenerating
  * from the .ioc cannot revert it. The .ioc therefore still shows PB4, PB5,
  * TIM3, TIM16 and DMA1 channels 2-3 as free: do not assign them elsewhere.
  ******************************************************************************
  */

#ifndef __SOFTUART_H
#define __SOFTUART_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Same rate as the hardware ports, so a vertical hop costs the same as a
 * horizontal one. Both ends are crystal-timed; the timers realise it to
 * within 0.2%, as the USART baud generators do. */
#define SOFTUART_BAUD   460800u

void    softuart_init(void);

/* TX. link_send() appends to the ring and kicks; the burst engine drains it
 * from interrupt context via link_soft_tx_pop(). */
void    softuart_tx_kick(void);
uint8_t softuart_tx_idle(void);     /* no burst in flight: the stop bit has left the pin */

/* RX. Pends the decoder; bytes arrive through link_soft_rx_push(). */
void    softuart_rx_poll(void);

/* Teardown. Mask, reset, flush link's rings, unmask -- the same fence the
 * hardware ports put around their ring resets. */
void    softuart_irq_mask(uint8_t masked);
void    softuart_reset(void);       /* call only between mask(1) and mask(0) */

/* ISR entry points, called from stm32g4xx_it.c. */
void    softuart_irq_tx_dma(void);     /* DMA1_Channel2 */
void    softuart_irq_rx_dma(void);     /* DMA1_Channel3 */
void    softuart_irq_rx_timer(void);   /* TIM3 */

/* --- Provided by link.c ---------------------------------------------------
 * All three run in interrupt context, on the TOP port's rings. */
uint8_t link_soft_tx_pop(uint8_t *c);   /* 1 and *c set if a byte was waiting */
void    link_soft_rx_push(uint8_t c);
void    link_soft_rx_error(void);       /* framing error */

#ifdef __cplusplus
}
#endif

#endif /* __SOFTUART_H */
