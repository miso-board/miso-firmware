/* Host test for Core/Inc/softuart_decode.h, the software UART's RX decoder.
 *
 *   cc -I Core/Inc -o /tmp/test_decode tools/test_decode.c -lm && /tmp/test_decode
 *
 * Synthesises capture-ring edge lists from byte sequences and checks the
 * decoder reproduces them: back-to-back bytes, +/-2% baud mismatch, ring and
 * counter wrap, glitches before and between frames, and a lost edge (which
 * corrupts what was captured before it and must re-sync after). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "softuart_decode.h"

#define RING 256
#define MASK (RING - 1)
#define TPB  8

static volatile uint16_t ring[RING];
static uint16_t wr;
static double   t_now;      /* synthetic wall clock in ticks (fractional) */
static int      level = 1;  /* line level, idles high */

static void edge(double t) { ring[wr] = (uint16_t)llround(t); wr = (wr + 1) & MASK; }
static void set_level(double t, int l) { if (l != level) { level = l; edge(t); } }

/* Emit one byte starting at t_now with the given bit length. */
static void put_byte(uint8_t b, double bit)
{
  set_level(t_now, 0);                             /* start */
  for (int i = 0; i < 8; i++) set_level(t_now + (i + 1) * bit, (b >> i) & 1);
  set_level(t_now + 9 * bit, 1);                   /* stop */
  t_now += 10 * bit;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Drain everything decodable at time `now` (>= the last edge, so the line
 * level is the synthetic `level`); returns bytes and counts framing errors. */
static int drain(uint16_t *rd, uint16_t now, uint8_t *out, int *framing)
{
  int n = 0, r;
  uint8_t c;
  while ((r = su_decode_next(ring, MASK, rd, wr, now, (uint8_t)level, TPB, &c)) != SU_DECODE_WAIT) {
    if (r == SU_DECODE_BYTE) out[n++] = c; else if (r == SU_DECODE_FRAMING) (*framing)++;
  }
  return n;
}

static void run(const char *name, const uint8_t *msg, int len, double bit,
                double gap, uint16_t t_start, uint16_t rd_start)
{
  memset((void *)ring, 0, sizeof(ring));
  wr = rd_start; t_now = t_start; level = 1;
  uint16_t rd = rd_start;
  uint8_t out[512]; int framing = 0, n = 0;

  for (int i = 0; i < len; i++) {
    put_byte(msg[i], bit);
    t_now += gap;
    /* decode as we go, as the tick loop would, with 'now' slightly behind */
    n += drain(&rd, (uint16_t)llround(t_now - 1), out + n, &framing);
  }
  n += drain(&rd, (uint16_t)llround(t_now + 12 * TPB), out + n, &framing);

  int ok = (n == len) && memcmp(out, msg, len) == 0 && framing == 0;
  CHECK(ok, "%s: got %d bytes (want %d), framing=%d", name, n, len, framing);
  if (!ok) { for (int i = 0; i < n; i++) printf("  out[%d]=%02X want %02X\n", i, out[i], i < len ? msg[i] : 0); }
  else printf("ok   %s\n", name);
}

int main(void)
{
  const uint8_t frame[] = { 0xA5, 0x5A, 0x12, 0x00, 0x12, 0x00, 0xFF, 0x55, 0xAA, 0x80, 0x01 };
  uint8_t all[256]; for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;

  run("nominal, back-to-back",         frame, sizeof frame, 8.0,  0,   100,   0);
  run("all 256 values, back-to-back",  all,   256,          8.0,  0,   100,   0);
  run("inter-byte gap 3 bits",         frame, sizeof frame, 8.0,  24,  100,   0);
  run("sender +0.4% fast (8.03 ticks)",frame, sizeof frame, 8.03, 0,   100,   0);
  run("sender -2% (7.84 ticks)",       all,   256,          7.84, 0,   100,   0);
  run("sender +2% (8.16 ticks)",       all,   256,          8.16, 0,   100,   0);
  run("timer wrap mid-frame",          frame, sizeof frame, 8.0,  0,   65500, 0);
  run("ring wrap mid-frame",           frame, sizeof frame, 8.0,  0,   100,   250);
  run("both wraps",                    all,   256,          8.0,  5,   65480, 240);

  /* Glitch before a frame: a 2-tick low pulse must be rejected as a start bit
   * (one framing error), its rising edge skipped silently, and the frame that
   * follows must come through intact. */
  {
    memset((void *)ring, 0, sizeof(ring)); wr = 0; t_now = 100; level = 1;
    uint16_t rd = 0; uint8_t out[32]; int framing = 0;
    set_level(t_now, 0); set_level(t_now + 2, 1); t_now += 40;
    for (unsigned i = 0; i < sizeof frame; i++) put_byte(frame[i], 8.0);
    int n = drain(&rd, (uint16_t)(t_now + 100), out, &framing);
    int ok = n == (int)sizeof frame && memcmp(out, frame, n) == 0 && framing == 1;
    CHECK(ok, "glitch: n=%d framing=%d", n, framing);
    if (ok) printf("ok   glitch rejected, frame intact (framing=%d)\n", framing);
  }

  /* Glitch *between* back-to-back frames, no idle around it. */
  {
    memset((void *)ring, 0, sizeof(ring)); wr = 0; t_now = 100; level = 1;
    uint16_t rd = 0; uint8_t out[32]; int framing = 0;
    put_byte(0x11, 8.0); put_byte(0x22, 8.0);
    set_level(t_now + 3, 0); set_level(t_now + 5, 1); t_now += 8;
    put_byte(0x33, 8.0); put_byte(0x44, 8.0);
    int n = drain(&rd, (uint16_t)(t_now + 100), out, &framing);
    int ok = n == 4 && out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33 && out[3] == 0x44;
    CHECK(ok, "mid-stream glitch: n=%d framing=%d", n, framing);
    if (ok) printf("ok   mid-stream glitch: 4/4 bytes, framing=%d\n", framing);
  }

  /* Lost edge (DMA overrun model): a byte already decoded, then the start
   * edge of the next byte vanishes. Everything captured before the gap is
   * suspect; everything after the next idle gap must decode correctly. */
  {
    memset((void *)ring, 0, sizeof(ring)); wr = 0; t_now = 100; level = 1;
    uint16_t rd = 0; uint8_t out[64]; int framing = 0;
    put_byte(0x31, 8.0);
    int n = drain(&rd, (uint16_t)(t_now + 4), out, &framing);   /* tick drains it once complete */
    uint16_t save = wr; put_byte(0x32, 8.0);   /* drop this byte's first edge */
    memmove((void *)&ring[save], (void *)&ring[save + 1], (wr - save - 1) * 2); wr--;
    put_byte(0x33, 8.0);
    t_now += 40 * TPB;                          /* idle gap: line is high here */
    put_byte(0x41, 8.0); put_byte(0x42, 8.0); put_byte(0x43, 8.0);
    n += drain(&rd, (uint16_t)(t_now + 100), out + n, &framing);
    int ok = n >= 4 && out[0] == 0x31 && out[n-3] == 0x41 && out[n-2] == 0x42 && out[n-1] == 0x43;
    CHECK(ok, "lost edge: n=%d framing=%d", n, framing);
    if (!ok) { for (int i = 0; i < n; i++) printf("  out[%d]=%02X\n", i, out[i]); }
    if (ok) printf("ok   lost edge: %d bytes, %d framing errors, re-synced after idle\n", n, framing);
  }

  printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
  return fails != 0;
}
