/**
  ******************************************************************************
  * @file    softuart_decode.h
  * @brief   Edge-timestamp -> byte decoder for the software UART (TOP port).
  *
  * Pure arithmetic over a ring of capture timestamps, with no hardware access,
  * so it can be compiled and tested on the host. softuart.c feeds it the
  * timer's DMA ring; tools/test_decode.c feeds it synthetic edge lists.
  *
  * Wire model: the line idles high. A byte is a falling start edge at t0,
  * eight data bits LSB first and a high stop bit, one bit = `tpb` ticks. The
  * ring holds the time of *every* edge (both polarities) but not the level.
  * Two parity facts recover the level anyway:
  *
  *   - the level right after edge e is the current pin level flipped once per
  *     edge captured after e, so whether e was falling is known exactly;
  *   - the level at any instant after t0 is the parity of the number of edges
  *     in (t0, instant]: even means still low, odd means high.
  *
  * The first is what makes hunting safe. Without it a rising edge followed by
  * idle decodes as a plausible byte and eats the real frame behind it.
  ******************************************************************************
  */

#ifndef __SOFTUART_DECODE_H
#define __SOFTUART_DECODE_H

#include <stdint.h>

/* Result of su_decode_next(). */
#define SU_DECODE_BYTE     1   /* *out is valid, *rd advanced past the byte     */
#define SU_DECODE_WAIT     0   /* the byte at *rd has not finished yet          */
#define SU_DECODE_FRAMING -1   /* bad start or stop bit; *rd advanced by one    */
#define SU_DECODE_SKIP     2   /* edge at *rd was rising, not a start; skipped  */

/**
  * Try to decode the byte whose start edge is at edges[*rd].
  *
  * @param edges     ring of edge timestamps (timer ticks, free-running 16-bit)
  * @param mask      ring size - 1 (size is a power of two)
  * @param rd        read index, updated on BYTE, FRAMING and SKIP
  * @param wr        write index: the slot the DMA will fill next
  * @param now       current timer count
  * @param level_now current pin level, read consistently with `wr` and `now`
  * @param tpb       ticks per bit
  * @param out       decoded byte on SU_DECODE_BYTE
  *
  * All time arithmetic is modulo 2^16, so it is only meaningful while every
  * pending edge is younger than one timer wrap. softuart.c guarantees that by
  * running the decoder at least every few ms, well inside the wrap.
  */
static inline int su_decode_next(const volatile uint16_t *edges, uint16_t mask,
                                 uint16_t *rd, uint16_t wr, uint16_t now,
                                 uint8_t level_now, uint16_t tpb, uint8_t *out)
{
  if (*rd == wr) return SU_DECODE_WAIT;

  /* Polarity: the line is at level_now after `after` further edges, so it
   * was level_now ^ (after & 1) right after this one. A start edge leaves
   * it low. */
  uint16_t after = (uint16_t)((wr - *rd - 1u) & mask);
  if ((level_now ^ after) & 1u) {
    *rd = (uint16_t)((*rd + 1u) & mask);
    return SU_DECODE_SKIP;
  }

  uint16_t t0 = edges[*rd];

  /* The stop bit is sampled at 9.5 bits; wait until that instant has passed
   * so every edge that can affect this byte has been captured. */
  if ((uint16_t)(now - t0) < (uint16_t)(10u * tpb)) return SU_DECODE_WAIT;

  uint16_t j = (uint16_t)((*rd + 1u) & mask);
  uint16_t nedges = 0;
  uint16_t byte = 0;

  for (uint16_t k = 0; k <= 9u; k++) {
    /* Sample mid-bit: start bit at 0.5, data k at k+0.5, stop at 9.5. */
    uint16_t s = (uint16_t)(k * tpb + tpb / 2u);
    while (j != wr && (uint16_t)(edges[j] - t0) <= s) {
      nedges++;
      j = (uint16_t)((j + 1u) & mask);
    }
    uint16_t level = nedges & 1u;

    if (k == 0) {
      /* A start bit that is already high again half a bit in was a glitch
       * shorter than a bit, not a start bit. */
      if (level) goto framing;
    } else if (k <= 8u) {
      byte |= (uint16_t)(level << (k - 1u));
    } else if (!level) {
      goto framing;   /* stop bit low */
    }
  }

  /* j is now the first edge later than the stop-bit sample (9.5 bits). A
   * back-to-back byte's start edge lands at 10.0 bits, so it is exactly the
   * next thing to decode. */
  *out = (uint8_t)byte;
  *rd = j;
  return SU_DECODE_BYTE;

framing:
  /* Step one edge and let the polarity check above find the next real start.
   * A byte lost to a DMA overrun leaves the parity of everything captured
   * *before* the gap off by one, so those are misjudged until *rd passes the
   * gap; the frame checksum in link.c rejects whatever that produces. */
  *rd = (uint16_t)((*rd + 1u) & mask);
  return SU_DECODE_FRAMING;
}

#endif /* __SOFTUART_DECODE_H */
