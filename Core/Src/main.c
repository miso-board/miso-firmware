/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "usbd_cdc_if.h"
#include "usbd_composite.h"
#include "link.h"
#include "mesh.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define NUM_LEDS        31
#define BITS_PER_LED    24
#define LED_RESET_WORDS 750    // trailing low period; latches the frame
#define DMA_BUF_LEN     (NUM_LEDS * BITS_PER_LED + LED_RESET_WORDS)   // DMA beats, one per 1.25 us bit

// Duty cycle values (adjust these if colours look wrong)
#define LED_CODE_0      45     // ~0.35 µs high
#define LED_CODE_1      110    // ~0.8 µs high

// Green level held on every LED while the board sits in DFU. Same scale as the
// boot pattern (whose brightest channel is 25), kept well under it so it reads
// as a standby glow rather than as the board playing.
#define LED_DFU_GREEN   6

// USB DFU bootloader entry
#define BOOTLOADER_MAGIC        0x4D49534Fu  // "MISO"
#define BOOTLOADER_SYSMEM_BASE  0x1FFF0000u  // STM32G4 system memory (ROM bootloader)
#define BOOTLOADER_TAP_WINDOW   500u         // ms: second reset within this window enters DFU

// Hall sensor scanning + CDC streaming
#define NUM_SENSORS         31
_Static_assert(NUM_SENSORS == MESH_SENSORS,
               "mesh.h's MESH_SENSORS must match NUM_SENSORS");
#define CYCLES_PER_US       144u   // SYSCLK in MHz, for DWT cycle-counter timing
#define MUX_SETTLE_US       5u     // 4067 switch + ADC input settling after channel select
#define STREAM_MAGIC0       0xA5
#define STREAM_MAGIC1       0x5A
#define FRAME_TYPE_SCAN     0x01
#define STREAM_DECIM_DEFAULT 2u    // stream every Nth scan
#define FW_VERSION          "miso 0.8.0"
#define GLOW_TARGET         200u   // max channel a held key's color is lifted toward at vel 127
#define COLOR_RX_TIMEOUT_MS 200u   // abort a half-received 'C' color frame after this

// --- MIDI / MPE ---------------------------------------------------------
// The pitch map is nine numbers, pushed by the companion over 'P' and held in
// RAM. meantonal represents a pitch as (w, h) = whole steps and diatonic
// semitones above C-1, and a LAYOUT is a 2x2 integer basis change from this
// board's grid axes into those: identity is Bosanquet (+x a whole tone, +y a
// diatonic semitone), and (1,-2,0,-1) is Wicki-Hayden.
//
// A TUNING is one number, the width of the fifth, because every tuning in the
// meantone family is determined by it -- an EDO is just one way to choose it.
// That is why no per-EDO step lattice and no per-key table are needed here:
//
//   cents(w, h) = fifth*(2w - 5h) + 1200*(3h - w)
//
// carried in MILLICENTS so it stays integer. The authoring-side source of truth
// is companion/src/lib/tuning.ts, which asks meantonal instead; the two are
// cross-checked by companion/scripts/check-tuning.mjs.
#define TUNE_W0             23     // default anchor: puts D4 on the centre key (3,3)
#define TUNE_H0             7
#define TUNE_FIFTH_DEFAULT  696774 // 31-EDO: round(18 * 1200 / 31) millicents
#define TUNE_FIFTH_MIN      685714 // below this a fifth has no diatonic scale
#define TUNE_FIFTH_MAX      720000 // above this likewise
#define TUNE_MATRIX_MAX     64     // sanity bound on a pushed basis change
#define TUNE_ANCHOR_MAX     4096   // sanity bound on a pushed anchor
#define PITCH_MSG_LEN       16     // payload bytes after 'P'
#define MPE_MEMBER_FIRST    1      // MIDI channel index of member channel 1 (= channel 2)
#define MPE_MEMBER_COUNT    15     // channels 2..16
#define MPE_BEND_SEMITONES  48     // MPE default; 0.59 cents per bend unit
#define MPE_BEND_CENTS      (MPE_BEND_SEMITONES * 100)
#define MIDI_CIN_NOTE_OFF   0x08
#define MIDI_CIN_NOTE_ON    0x09
#define MIDI_CIN_CC         0x0B
#define MIDI_CIN_PITCHBEND  0x0E

// --- Procedural colour mapping ------------------------------------------
// Colour has the same shape as pitch above: parameters, not a table. A key is
// coloured by the ACCIDENTAL of the note that lands on it -- its Bosanquet row
// -- which is a function of the absolute grid coordinate, so one small block of
// parameters colours a grid of any size and a board evaluates its own keys.
//
// The (w, h) arithmetic is the same as pitch_for_xy()'s; on top of it, taken
// straight from meantonal (see companion/src/lib/colorMaps.ts, the authoring
// side, which asks the library instead):
//
//   chroma     = 2w - 5h
//   accidental = floor((chroma + 1) / 7)        the row
//   pc7        = mod7(w + h)                    letter index, C D E = 0 1 2
//
// A tuning is not involved: an accidental is a spelling, so the colours mean the
// same thing in every tuning. Unlike 'P', these parameters DO travel down the
// mesh (every board lights its own LEDs, while only the master sounds notes).
#define COLORGEN_PAL_MAX      8    // palette entries; rows beyond it wrap
#define COLORGEN_MSG_LEN      40   // payload bytes after 'K'
_Static_assert(COLORGEN_MSG_LEN == MESH_CGEN_BYTES,
               "mesh.h's MESH_CGEN_BYTES must match COLORGEN_MSG_LEN");
#define COLORGEN_LIGHTEN_CDE  0x01 // flags bit 0: tint C, D, E paler as a landmark
#define COLORGEN_CDE_PCT      35   // how far toward white that tint goes
#define COLORGEN_WAIT_MS      1200 // boot: longest a child waits for colours before rippling

// Velocity engine. Positions are normalized per key: 0 = rest, 1 = calibrated
// full press. Thresholds chosen against measured spans of 510-700 counts with
// a noise floor of ~2 counts, so even 4% of span is far above noise.
#define KEY_START_POS       0.10f   // arm timing when travel passes this
#define KEY_ABORT_POS       0.04f   // un-arm if it falls back below this
#define KEY_END_POS         0.70f   // key-down fires here; dt = END - START time
#define KEY_RELEASE_POS     0.30f   // key-up fires when travel falls below this
#define KEY_EMA_ALPHA       0.3f    // light smoothing on raw readings
#define VEL_DT_FAST_US      3000u   // transit this fast (or faster) = velocity 127
#define VEL_DT_SLOW_US      120000u // transit this slow (or slower) = velocity 1
#define REST_CAL_SCANS      128u    // boot scans averaged into per-key rest level
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

UART_HandleTypeDef hlpuart1;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

TIM_HandleTypeDef htim1;
DMA_HandleTypeDef hdma_tim1_ch1;

/* USER CODE BEGIN PV */
// Lives in .noinit RAM: keeps its value across NRST resets, only lost on power-off.
__attribute__((section(".noinit"))) static volatile uint32_t bootloader_flag;

// 16-bit, because that is what the DMA channel is configured to read
// (MemDataAlignment = HALFWORD, Miso.ioc).
//
// This used to be uint32_t, and that was the flicker. The DMA walked the array
// two bytes at a time, so every second beat was the zero upper half of a word,
// and Length is in beats -- 1494 beats covered only the first 747 entries:
//
//   per bit     2 beats / 2.5 us  instead of  1 beat / 1.25 us
//   reset       3 words / 7.5 us  instead of  750 words / 937 us
//   guarded     first half of the data instead of all of it
//   total       1867.5 us either way, which is why nobody noticed
//
// An SK6812 latches a frame after the line is low for 80 us, so with only
// 7.5 us of it driven, every frame was being latched by the floating line that
// HAL_TIM_PWM_Stop_DMA() left behind (see led_line_hold_low). A latch with no
// margin, next to the USB pins: hence flicker that tracked USB activity.
static uint16_t dma_buffer[DMA_BUF_LEN] __attribute__((aligned(4)));
uint8_t  led_data[NUM_LEDS][3];   // [G][R][B]
uint16_t sensor_raw[31];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// Must run before HAL_Init/SystemClock_Config, while the MCU is still in its
// reset state (HSI clock, no interrupts, peripherals untouched) — the ROM
// bootloader expects to start from those conditions.
static void bootloader_check(void)
{
  // *Why* we reset matters. The double-tap gesture is about the NRST button,
  // so only a pin reset (or our own NVIC_SystemReset) may be read as a tap.
  // The comment on bootloader_flag assumes power-off clears .noinit RAM, but a
  // rail that sags without fully collapsing keeps it — and a bouncing supply
  // (a pogo-pin mate, a hand-plugged USB connector) produces exactly the burst
  // of resets that looks like a deliberate double tap. The board then boots
  // into the ROM bootloader with its LEDs dark and reads as dead.
  // Flags are cleared here so each boot only ever sees its own reset cause.
  uint32_t csr = RCC->CSR;
  RCC->CSR |= RCC_CSR_RMVF;

  // BORRSTF covers power-on and brown-out alike. Note a POR asserts NRST
  // internally and so sets PINRSTF too, which is why this has to be checked
  // first rather than just testing for a pin reset.
  if ((csr & RCC_CSR_BORRSTF) ||
      !(csr & (RCC_CSR_PINRSTF | RCC_CSR_SFTRSTF))) {
    bootloader_flag = 0;  // stale magic, or uninitialized RAM after a POR
    return;
  }

  if (bootloader_flag != BOOTLOADER_MAGIC) {
    return;
  }
  bootloader_flag = 0;  // one-shot: next reset boots the app normally

  __set_MSP(*(volatile uint32_t *)BOOTLOADER_SYSMEM_BASE);
  ((void (*)(void))(*(volatile uint32_t *)(BOOTLOADER_SYSMEM_BASE + 4)))();
  while (1);  // never reached
}

// Call from anywhere (e.g. a future MIDI/serial "enter DFU" command) to reboot
// into the USB DFU bootloader without touching the reset button.
void Bootloader_RequestDFU(void)
{
  bootloader_flag = BOOTLOADER_MAGIC;
  NVIC_SystemReset();
}

void set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
  if (index >= NUM_LEDS) return;
  led_data[index][0] = g;   // SK6812 is GRB
  led_data[index][1] = r;
  led_data[index][2] = b;
}

void fill_solid(uint8_t r, uint8_t g, uint8_t b)
{
  for (int i = 0; i < NUM_LEDS; i++) {
    set_pixel(i, r, g, b);
  }
}

void prepare_dma_buffer(void)
{
  uint32_t idx = 0;

  for (int led = 0; led < NUM_LEDS; led++) {
    for (int colour = 0; colour < 3; colour++) {
      uint8_t byte = led_data[led][colour];
      for (int bit = 7; bit >= 0; bit--) {
        dma_buffer[idx++] = (byte & (1 << bit)) ? LED_CODE_1 : LED_CODE_0;
      }
    }
  }

  // Reset pulse (low for a while)
  for (int i = 0; i < LED_RESET_WORDS; i++) {
    dma_buffer[idx++] = 0;
  }
}

// Rewriting dma_buffer while the previous transfer is still streaming it clocks
// garbage into the chain, and HAL_TIM_PWM_Start_DMA's HAL_BUSY return was being
// discarded. Skip the refresh instead; the next one is only 30 ms away.
static volatile uint8_t  led_dma_busy = 0;
static volatile uint32_t usb_defers = 0;   // USB sends held off during the bitstream

/* Diagnostic: busy-wait this many microseconds per scan, set by 'p<N>'.
 * Lets -Os code be run at -O0's scan rate, which separates "the optimiser
 * broke something" from "running twice as fast is what breaks it" (twice the
 * mux switching and ADC transients on the 3V3 rail that also drives the LED
 * data line, whose 3.3 V high already sits under the SK6812's ~3.5 V VIH). */
volatile uint32_t scan_throttle_us = 0;

/* Diagnostic: 'n' stops the Hall scan entirely while leaving the loop, USB and
 * LED refresh running. If the LEDs go clean with scanning off, the disturbance
 * is the analog side -- 16 mux channel switches and 31 ADC conversions per
 * scan, whose current transients ride on the same 3V3 rail that drives the LED
 * data line -- rather than CPU or USB activity. */
volatile uint8_t scan_enabled = 1;

/* Objective check on the two remaining possibilities, so this stops depending
 * on watching the board:
 *   led_churn  - led_data changed between refreshes. With no keys held and no
 *                colour frames arriving it must not, so any count means
 *                something is writing over it.
 *   dma_churn  - dma_buffer differed at the end of the transfer from what was
 *                put there at the start, i.e. it was modified mid-flight.
 * If both stay zero while the LEDs visibly flash, the bytes leaving the MCU
 * were correct and correctly transmitted, and the fault is on the wire. */
static uint32_t led_sum_prev = 0;
static uint32_t dma_sum_start = 0;
static volatile uint32_t led_churn = 0;
static volatile uint32_t dma_churn = 0;

static uint32_t sum32(const uint32_t *p, uint32_t n)
{
  uint32_t s = 0;
  while (n--) s = (s << 1) ^ *p++;   /* order-sensitive, unlike a plain sum */
  return s;
}

static uint32_t dma_sum(void)
{
  return sum32((const uint32_t *)(const void *)dma_buffer, sizeof(dma_buffer) / 4);
}

/**
 * True only while the DMA is still clocking out LED DATA.
 *
 * The SK6812 encodes each bit as a pulse width, so one late DMA beat mis-sizes
 * that bit and every LED after it in the chain receives shifted data -- which is
 * why the corrupt region moves around. Keeping our own USB sends out of that
 * window costs nothing, so they wait.
 *
 * The trailing reset words are deliberately excluded: the line is held low
 * throughout them, so a late beat there cannot corrupt anything. That keeps the
 * exclusion window to ~930 us of each 30 ms refresh rather than ~1.9 ms.
 */
static uint8_t led_critical(void)
{
  return led_dma_busy && (__HAL_DMA_GET_COUNTER(&hdma_tim1_ch1) > LED_RESET_WORDS);
}

/*
 * Between refreshes the data line is DRIVEN LOW, never released.
 *
 * HAL_TIM_PWM_Stop_DMA() clears CC1E and MOE, and with OSSI off that makes TIM1
 * release PA8 altogether: the pin goes high-impedance, and with no pull on the
 * board the LED data line then floats for the ~28 ms between one refresh and
 * the next -- over 90% of the time.
 *
 * On its own that would be survivable; paired with the buffer-width bug above
 * it was not, because the frame's 80 us latch period then fell almost entirely
 * inside the floating stretch. The chain was committing each frame on an
 * undriven node sitting next to the USB pins, with no noise margin at all.
 *
 * Note what this is NOT. The floating line does not pick up countable edges
 * from USB traffic -- measured at zero over 15 s of streaming with the counter
 * in led_edge_selftest() validated either side. It is the latch that was
 * fragile, not the data.
 *
 * So the timer is never stopped. It runs on with CCR1 = 0, a 0% duty cycle,
 * which holds the output at its inactive level with the driver on; only the DMA
 * request is switched off. The pin also carries a pull-down for the moments the
 * timer does not own it (reset, and the diagnostic below).
 */
static void led_line_hold_low(void)
{
  TIM1->CCR1  = 0;
  TIM1->CCER |= TIM_CCER_CC1E;
  TIM1->BDTR |= TIM_BDTR_MOE;
  TIM1->CR1  |= TIM_CR1_CEN;
}

/* Diagnostic, toggled by 'f': release the line between refreshes exactly as
 * the code used to (HAL_TIM_PWM_Stop_DMA, no pull), but with PA8 switched to
 * an input that counts every edge on the floating line. 'i' reports the total
 * as idleedges, and 'F' proves the counter can count before a zero from it is
 * believed. Measured result: zero, quiet bus or streaming alike -- see the
 * README. Kept because it is the only way to tell "the line is quiet" from
 * "nothing is watching the line". */
volatile uint8_t led_float_diag = 0;
static volatile uint32_t led_idle_edges = 0;
static uint8_t led_pin_listening = 0;

static void led_pin_config(uint8_t listen)
{
  GPIO_InitTypeDef g = {0};
  g.Pin = GPIO_PIN_8;
  if (listen) {
    g.Mode = GPIO_MODE_IT_RISING_FALLING;
    g.Pull = GPIO_NOPULL;                   /* the old, floating, condition */
    HAL_GPIO_Init(GPIOA, &g);
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_8);
    HAL_NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
  } else {
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    EXTI->IMR1 &= ~GPIO_PIN_8;              /* HAL_GPIO_Init leaves the line armed */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLDOWN;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF6_TIM1;
    HAL_GPIO_Init(GPIOA, &g);
  }
  led_pin_listening = listen;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_8) led_idle_edges++;
}

void show_leds(void)
{
  if (led_dma_busy) return;
  if (led_pin_listening) led_pin_config(0);   /* diagnostic had the line floating */
  prepare_dma_buffer();
  if (HAL_TIM_PWM_Start_DMA(&htim1, TIM_CHANNEL_1,
                            (const uint32_t *)(const void *)dma_buffer,
                            DMA_BUF_LEN) != HAL_OK) {
    return;
  }
  led_dma_busy = 1;

  uint32_t led_sum = sum32((const uint32_t *)(const void *)led_data,
                           sizeof(led_data) / 4);
  if (led_sum_prev && led_sum != led_sum_prev) led_churn++;
  led_sum_prev = led_sum;
  dma_sum_start = dma_sum();
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM1)
  {
    if (led_float_diag) {
      HAL_TIM_PWM_Stop_DMA(&htim1, TIM_CHANNEL_1);   /* releases PA8, as it always did */
      led_pin_config(1);
    } else {
      /* Only the request goes; the timer runs on at CCR1 = 0 and keeps the
       * line driven low until the next refresh restarts the DMA. */
      __HAL_TIM_DISABLE_DMA(&htim1, TIM_DMA_CC1);
    }
    if (dma_sum() != dma_sum_start) dma_churn++;
    led_dma_busy = 0;
  }
}

// Wait out an in-flight LED refresh (~1.9 ms), so a caller with no next frame
// to fall back on isn't silently skipped by show_leds(). Bounded, because this
// sits on the path into DFU and must never be the reason the board hangs there.
static void led_dma_settle(void)
{
  uint32_t t0 = HAL_GetTick();
  while (led_dma_busy && (HAL_GetTick() - t0) < 10u) { }
}

// Paint every LED dim green on the way into the ROM bootloader. The bootloader
// never touches the chain and the reset doesn't cut its power, so whatever is
// latched here stays lit for the whole DFU session: the board reads as waiting
// for a firmware upload rather than as dead.
static void led_dfu_indicate(void)
{
  led_dma_settle();
  fill_solid(0, LED_DFU_GREEN, 0);
  show_leds();
  led_dma_settle();   // the frame has to reach the chain before the reset
}

void select_mux_channel(uint8_t channel)
{
  // channel = 0..15
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, (channel & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET); // S0
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, (channel & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET); // S1
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, (channel & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET); // S2
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, (channel & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET); // S3
}

uint16_t read_adc_channel(uint32_t channel)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  sConfig.Channel = channel;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;

  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
    Error_Handler();
  }

  HAL_ADC_Start(&hadc1);
  HAL_ADC_PollForConversion(&hadc1, 10);
  uint16_t value = HAL_ADC_GetValue(&hadc1);
  HAL_ADC_Stop(&hadc1);

  return value;
}

// --- Microsecond timing via the DWT cycle counter ---------------------------

static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles = us * CYCLES_PER_US;
  while ((uint32_t)(DWT->CYCCNT - start) < cycles);
}

// Monotonic microsecond clock. CYCCNT wraps every ~29.8 s at 144 MHz, so this
// accumulates deltas into a 64-bit base — it must be called at least that
// often, which the scan loop guarantees. Returned value wraps at 2^32 µs
// (~71.6 min); consumers must use wrap-safe delta arithmetic.
static uint32_t micros32(void)
{
  static uint64_t total_cycles = 0;
  static uint32_t last_cyccnt = 0;
  uint32_t now = DWT->CYCCNT;
  total_cycles += (uint32_t)(now - last_cyccnt);
  last_cyccnt = now;
  return (uint32_t)(total_cycles / CYCLES_PER_US);
}

// --- Sensor scanning ---------------------------------------------------------

// Both 4067s share the S0-S3 select lines, so each select setting yields two
// sensors: M1 (PA1/ch2) -> keys 0-15, M2 (PA0/ch1) -> keys 16-30.
void scan_all(void)
{
  for (uint8_t ch = 0; ch < 16; ch++) {
    select_mux_channel(ch);
    delay_us(MUX_SETTLE_US);
    sensor_raw[ch] = read_adc_channel(ADC_CHANNEL_2);
    if (ch < 15) {
      sensor_raw[16 + ch] = read_adc_channel(ADC_CHANNEL_1);
    }
  }
}

// --- USB CDC streaming + commands --------------------------------------------

volatile uint8_t  stream_on   = 0;
volatile uint32_t stream_decim = STREAM_DECIM_DEFAULT;
volatile uint8_t  led_viz_on  = 1;
volatile uint8_t  info_req    = 0;
volatile uint8_t  link_req    = 0;
volatile uint8_t  dfu_req     = 0;
volatile uint8_t  topo_req    = 0;
uint32_t scan_hz = 0;  // measured full-scan rate, updated once per second

// Background LED colors (RGB, LED-chain order), initialized from the boot
// pattern and replaced by host color frames.
static uint8_t led_background[NUM_LEDS][3];

// --- Boot ripple -------------------------------------------------------------
//
// The colour mapping does not appear all at once: starting from a dark board,
// a bright whitened wavefront sweeps across from left to right and leaves the
// mapped colour behind it, so the board looks like it is waking up into its
// mapping rather than switching into it.
//
// SWEEP AXIS. The board is a sheared axial hex grid, so "left to right" on the
// physical board is not the x column index. Rendering rotates the grid until
// the octave direction (5,2) lies horizontal -- the standard Bosanquet
// orientation, which is how the board is physically laid out -- and under that
// rotation the screen x of a key works out to exactly
//
//     screen_x = (4.5 / sqrt(117)) * (4x + 3y)
//
// so 4x + 3y IS the left-to-right coordinate and the sweep needs no
// trigonometry, just that integer per LED. (Cross-checked against LED_PIXEL in
// companion/src/lib/layout.ts, which derives the rotation independently: the
// ratio is constant to nine decimal places across all 31 keys.)
//
// The 31 keys span 4x + 3y = 8 (at (2,0)) to 34 (at (4,6)), 26 units across 26
// distinct values, so nearly every key gets its own arrival time.
#define WAVE_POS_MIN    8u     // min 4x+3y over the 31 keys
#define WAVE_POS_MAX    34u    // max
#define WAVE_UNIT       256    // fixed-point sub-steps per grid unit
#define WAVE_LEAD       (3 * WAVE_UNIT)   // dark -> crest, ahead of the front
#define WAVE_TAIL       (7 * WAVE_UNIT)   // crest -> mapped colour, behind it
#define WAVE_TRAVEL_MS  900u   // leading edge entering to trailing edge leaving
#define WAVE_FRAME_MS   20u    // ~1.6 ms of that is the LED DMA itself
#define WAVE_FRAMES     (WAVE_TRAVEL_MS / WAVE_FRAME_MS)
#define WAVE_WHITE      150u   // crest level; the target colour is lifted to it

// 4x + 3y per LED chain index: where each key sits along the sweep.
static const uint8_t led_wave_pos[NUM_LEDS] = {
  12,  9, 10, 13, 16, 19, 23, 20, 17, 14, 11,  8, 12, 15, 18, 21,
  24, 27, 30, 34, 31, 28, 25, 22, 19, 23, 26, 29, 32, 33, 30,
};

// One frame of the ripple, with the front edge at `front` (fixed point).
static void wave_frame(int32_t front)
{
  for (uint8_t l = 0; l < NUM_LEDS; l++) {
    int32_t d = front - (int32_t)led_wave_pos[l] * WAVE_UNIT;
    if (d <= -WAVE_LEAD) { set_pixel(l, 0, 0, 0); continue; }   // not reached yet

    const uint8_t *t = led_background[l];

    // Whiten towards a level that is never below the colour's own brightest
    // channel, so the crest is always a lightened version of what it leaves
    // behind and never -- for a bright mapping pushed by the host -- a dimmer
    // one. At full weight every channel sits at `level`, i.e. white.
    uint32_t level = WAVE_WHITE;
    for (uint8_t c = 0; c < 3; c++) if (t[c] > level) level = t[c];

    uint32_t w;   // weight of white over the mapped colour, 0..WAVE_UNIT
    if (d < 0) {
      // Leading ramp: black up into the crest, so nothing pops on. The colour
      // is not mixed in yet -- this is the wave arriving, not the mapping.
      uint32_t f = (uint32_t)(d + WAVE_LEAD) * WAVE_UNIT / WAVE_LEAD;
      uint8_t v = (uint8_t)(level * f / WAVE_UNIT);
      set_pixel(l, v, v, v);
      continue;
    }

    if (d < WAVE_TAIL) {
      // Trailing decay, squared: off the crest quickly, into the colour softly.
      uint32_t u = (uint32_t)(WAVE_TAIL - d) * WAVE_UNIT / WAVE_TAIL;
      w = u * u / WAVE_UNIT;
    } else {
      w = 0;    // settled: the mapping itself
    }

    uint8_t rgb[3];
    for (uint8_t c = 0; c < 3; c++) {
      rgb[c] = (uint8_t)(t[c] + (level - t[c]) * w / WAVE_UNIT);
    }
    set_pixel(l, rgb[0], rgb[1], rgb[2]);
  }
  show_leds();
}

// Animate led_background[] into life, left to right. Blocking, and deliberately
// so: it runs before the scan loop, so the per-key rest calibration that opens
// the loop still happens under a settled, static LED pattern. Calibrating while
// a bright crest swept the board would fold the chain's own current draw --
// which rides the same 3V3 rail as the Hall sensors -- into every key's rest
// level.
static void boot_wave_play(void)
{
  const int32_t start = (int32_t)WAVE_POS_MIN * WAVE_UNIT - WAVE_LEAD;
  const int32_t span  = (int32_t)(WAVE_POS_MAX - WAVE_POS_MIN) * WAVE_UNIT
                      + WAVE_LEAD + WAVE_TAIL;

  for (uint32_t f = 0; f <= WAVE_FRAMES; f++) {
    if (f) HAL_Delay(WAVE_FRAME_MS);
    wave_frame(start + (int32_t)(span * f / WAVE_FRAMES));
  }
}

// Incoming color frames are QUEUED, not single-buffered. The host sends one
// frame per board back to back, so a single staging buffer meant the USB
// interrupt began overwriting frame 2 while the main loop was still copying
// frame 1 out of it: one board lost its colors and the other got a torn mix of
// both. With painting debounced at 100 ms that tears repeatedly, which shows up
// as flicker. The interrupt only ever writes the head slot and the main loop
// only ever reads the tail, so neither can touch the other's.
//
// Layout per slot: int16 x | int16 y | 93 RGB bytes. 'C' ("this board") is
// staged as an 'L' aimed at our own origin, so there is one path, not two.
#define COLOR_MSG_LEN   (4 + NUM_LEDS * 3)
#define COLOR_Q_SLOTS   4
static volatile uint8_t  color_q[COLOR_Q_SLOTS][COLOR_MSG_LEN];
static volatile uint8_t  cq_head = 0, cq_tail = 0;
static volatile uint16_t cq_fill = 0;     // bytes written into the head slot
static volatile uint8_t  cq_active = 0;
static volatile uint32_t cq_dropped = 0;  // queue full; surfaced by 'i'

// The pushed pitch map. A single buffer rather than a queue: tuning parameters
// are idempotent and last-write-wins, so there is nothing to lose by dropping an
// older frame. The ISR only fills this and raises pq_ready; the main loop
// installs it, because pitch_for_xy() runs from the main loop and must never
// read a set being written under it.
static volatile uint8_t  pq_buf[PITCH_MSG_LEN];
static volatile uint8_t  pq_fill = 0;
static volatile uint8_t  pq_active = 0;   // mid-frame, collecting payload bytes
static volatile uint8_t  pq_ready = 0;    // a complete frame awaits the main loop
static volatile uint32_t pq_rejected = 0; // failed validation; surfaced by 'i'

// The pushed colour generator, single-buffered for the same reason: 'K' carries
// parameters, which are idempotent and last-write-wins.
static volatile uint8_t  kq_buf[COLORGEN_MSG_LEN];
static volatile uint8_t  kq_fill = 0;
static volatile uint8_t  kq_active = 0;
static volatile uint8_t  kq_ready = 0;
static volatile uint32_t kq_rejected = 0; // failed validation; surfaced by 'i'
static volatile uint32_t last_rx_tick = 0;

// Binary scan frame: A5 5A 01 | u32 t_us | 31 x u16 raw | u8 checksum(payload)
static void stream_frame(uint32_t t_us)
{
  static uint8_t buf[3 + 4 + NUM_SENSORS * 2 + 1];
  if (CDC_IsTxBusy()) return;  // drop the frame rather than stall the scan loop
  if (led_critical()) { usb_defers++; return; }  // never during the bitstream
  buf[0] = STREAM_MAGIC0;
  buf[1] = STREAM_MAGIC1;
  buf[2] = FRAME_TYPE_SCAN;
  memcpy(&buf[3], &t_us, 4);
  memcpy(&buf[7], (const void *)sensor_raw, NUM_SENSORS * 2);
  uint8_t sum = 0;
  for (uint32_t i = 3; i < sizeof(buf) - 1; i++) sum += buf[i];
  buf[sizeof(buf) - 1] = sum;
  CDC_Transmit_FS(buf, sizeof(buf));
}

// Text output (info lines, key events). Waits briefly for the endpoint to
// free up; data is copied to a static buffer that stays valid during TX.
static void cdc_send_text(const char *s)
{
  static char txt[288];
  size_t n = strlen(s);
  if (n > sizeof(txt)) n = sizeof(txt);
  uint32_t t0 = HAL_GetTick();
  while (CDC_IsTxBusy()) {
    if (HAL_GetTick() - t0 > 5) return;
  }
  memcpy(txt, s, n);
  CDC_Transmit_FS((uint8_t *)txt, (uint16_t)n);
}

/* Positive control for the idle-edge counter, run by 'F'.
 *
 * Zero edges is the answer the fix predicts, and it is also exactly what a
 * diagnostic that never ran would print. So the count means nothing until
 * something known-nonzero has been through it. EXTI watches the pin itself and
 * does not care that the pin is configured as an output, so driving PA8
 * directly sends a known number of edges through the same SYSCFG routing, EXTI
 * mask, NVIC entry and callback that the real measurement uses.
 *
 * Expect edges = 2 x pulses. Materially fewer means the measurement is broken,
 * not that the line is quiet. */
volatile uint8_t led_selftest_req = 0;

static void led_edge_selftest(void)
{
  const uint32_t pulses = 50;

  led_dma_settle();                 /* don't fight an in-flight refresh */
  uint8_t was_listening = led_pin_listening;
  uint32_t before = led_idle_edges;

  led_pin_config(1);                /* arm exactly as the diagnostic does */
  uint32_t moder = GPIOA->MODER;
  GPIOA->MODER = (moder & ~GPIO_MODER_MODE8_Msk)
               | (0x1u << GPIO_MODER_MODE8_Pos);   /* general-purpose output */
  for (uint32_t i = 0; i < pulses; i++) {
    GPIOA->BSRR = GPIO_PIN_8;                      /* rising  */
    delay_us(20);
    GPIOA->BSRR = (uint32_t)GPIO_PIN_8 << 16u;     /* falling */
    delay_us(20);
  }
  GPIOA->MODER = moder;
  uint32_t after = led_idle_edges;

  if (!was_listening) led_pin_config(0);
  led_idle_edges = before;          /* don't pollute the real measurement */

  char line[128];
  snprintf(line, sizeof(line), "SELFTEST pulses=%lu edges=%lu expect=%lu %s\r\n",
           (unsigned long)pulses, (unsigned long)(after - before),
           (unsigned long)(pulses * 2u),
           (after - before) >= pulses ? "OK" : "COUNTER-DEAD");
  cdc_send_text(line);
}

// --- Velocity engine ---------------------------------------------------------
// Per-key calibrated full-press values, measured 2026-09-08 with the companion
// app ("miso-cal-1" sweep). Rest levels are re-measured at every boot instead
// (REST_CAL_SCANS), since they drift less than a press but more than a flash
// cycle. Perimeter keys genuinely swing less than center keys (~2635 vs ~2828,
// mechanical bottom-out differences) — normalizing per key absorbs that.
static const uint16_t key_cal_max[NUM_SENSORS] = {
  2810, 2769, 2816, 2828, 2811, 2774, 2746, 2816, 2715, 2797,
  2825, 2672, 2821, 2781, 2777, 2817, 2759, 2635, 2804, 2761,
  2704, 2703, 2815, 2700, 2807, 2812, 2800, 2824, 2806, 2790,
  2820,
};

// LED chain index for each sensor, from the board layout traced 2026-09-08
// (7x7 sparse grid, serpentine LED chain; see README "Board layout"). The
// physical grid coordinates live in companion/src/lib/layout.ts — firmware
// only needs sensor -> LED to light the key that was actually pressed.
static const uint8_t led_for_sensor[NUM_SENSORS] = {
   8,  0,  4, 15, 23, 24, 25, 14, 12, 13,
   9, 11, 10,  2,  1,  3, 18, 19, 21, 20,
  28, 29, 27, 30, 22, 26,  7,  5, 17, 16,
   6,
};

// Board grid coordinate (x, y) of each sensor, i.e. LED_POS[led_for_sensor[i]].
static const int8_t sensor_grid[NUM_SENSORS][2] = {
  {2,3}, {0,4}, {1,4}, {3,3}, {4,2}, {4,1}, {5,1}, {3,2},
  {3,0}, {3,1}, {2,2}, {2,0}, {2,1}, {1,2}, {0,3}, {1,3},
  {3,6}, {4,6}, {4,4}, {4,5}, {5,4}, {6,3}, {5,3}, {6,2},
  {4,3}, {5,2}, {2,4}, {1,5}, {3,5}, {3,4}, {2,5},
};


enum { KS_IDLE, KS_PRESSING, KS_HELD };
static float    key_rest[NUM_SENSORS];
static float    key_ema[NUM_SENSORS];
static uint8_t  key_state[NUM_SENSORS];
static uint32_t key_t0[NUM_SENSORS];      // µs timestamp of START crossing
static uint8_t  key_vel[NUM_SENSORS];     // last key-down velocity, 1..127
static float    rest_acc[NUM_SENSORS];
static uint32_t rest_scans = 0;           // < REST_CAL_SCANS while calibrating

// --- MIDI / MPE --------------------------------------------------------------

// One record per MPE member channel, keyed by PACKED ABSOLUTE COORDINATE rather
// than by local sensor index, so keys on neighbouring boards are representable
// at all. The note number is cached here because a note-off must send the note
// that was actually started; recomputing it would go wrong if the pitch map
// ever moved under a held key.
typedef struct {
  uint8_t  active;
  uint32_t key;     // packed absolute (x, y)
  uint8_t  note;
  uint32_t age;
} mpe_voice_t;
static mpe_voice_t mpe_voice[MPE_MEMBER_COUNT];
static uint32_t mpe_alloc_counter = 0;

static uint32_t key_id(int16_t x, int16_t y)
{
  return ((uint32_t)(uint16_t)x << 16) | (uint32_t)(uint16_t)y;
}
volatile uint8_t events_text_on = 1;   // 'e' toggles the EV text lines
volatile uint8_t mpe_setup_req = 0;    // 'M' re-sends the MPE configuration

// Integer divide, rounding to nearest (C division truncates toward zero).
static int32_t div_round(int32_t num, int32_t den)
{
  return (num >= 0) ? (num + den / 2) / den : (num - den / 2) / den;
}

static void midi_send(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2)
{
  const uint8_t pkt[4] = { cin, status, d1, d2 };  // cable 0
  USBD_MIDI_Send(pkt);
}

// The live pitch map. Seeded to Bosanquet in 31-EDO with D4 on the centre key,
// which is what this board played before 'P' existed -- so a board that never
// hears from the companion sounds exactly as it always did, and a fresh install
// of the companion pushes these same values back.
typedef struct {
  int32_t fifth_milli;                  /* width of the fifth, millicents */
  int16_t m00, m01, m10, m11;           /* grid -> (w, h) basis change */
  int16_t aw, ah;                       /* anchor added after the map */
} pitch_params_t;

static volatile pitch_params_t pitch_params = {
  TUNE_FIFTH_DEFAULT, 1, 0, 0, 1, TUNE_W0, TUNE_H0
};

// Pitch for ANY absolute grid coordinate, in integers: cents above C-1 ->
// nearest MIDI note plus a bend for the remainder. No floating point and no Hz:
// MIDI wants note+bend. Taking a coordinate rather than a sensor index is what
// lets the master sound keys that live on a neighbouring board.
// Returns 0 if the pitch falls outside MIDI range.
static uint8_t pitch_for_xy(int32_t x, int32_t y, uint8_t *note_out, uint16_t *bend_out)
{
  // Snapshot first: the main loop can install new parameters between any two
  // reads, and a torn set would put w and h in different tunings.
  pitch_params_t p = pitch_params;

  int32_t w = p.m00 * x + p.m01 * y + p.aw;
  int32_t h = p.m10 * x + p.m11 * y + p.ah;

  // Millicents above C-1. Bounded well inside int32: the term is ~1e7 over a
  // grid of any size a link can address.
  int32_t cm = p.fifth_milli * (2 * w - 5 * h) + 1200000 * (3 * h - w);

  // div_round rather than C division: on a multi-board grid a coordinate can be
  // negative, and truncation toward zero would round the wrong way there.
  int32_t note = div_round(cm, 100000);
  int32_t rem  = cm - note * 100000;                 /* |rem| <= 50000 */
  int32_t bend = 8192 + div_round(rem * 8192, MPE_BEND_CENTS * 1000);

  if (note < 0 || note > 127 || bend < 0 || bend > 16383) return 0;
  *note_out = (uint8_t)note;
  *bend_out = (uint16_t)bend;
  return 1;
}

// Install a pushed pitch map, rejecting a frame that cannot be one. These bytes
// come from a host, and a wild fifth or basis change would put every key out of
// MIDI range and silence the instrument with no way to tell why.
// Returns 0 if the frame was rejected.
static uint8_t pitch_params_apply(const uint8_t *m)
{
  int32_t fifth = (int32_t)((uint32_t)m[0] | ((uint32_t)m[1] << 8)
                            | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24));
  if (fifth < TUNE_FIFTH_MIN || fifth > TUNE_FIFTH_MAX) return 0;

  int16_t v[6];
  for (int i = 0; i < 6; i++) {
    v[i] = (int16_t)((uint16_t)m[4 + i * 2] | ((uint16_t)m[5 + i * 2] << 8));
  }
  for (int i = 0; i < 4; i++) {
    if (v[i] < -TUNE_MATRIX_MAX || v[i] > TUNE_MATRIX_MAX) return 0;
  }
  if (v[0] * v[3] - v[1] * v[2] == 0) return 0;   /* singular: rows collapse */
  for (int i = 4; i < 6; i++) {
    if (v[i] < -TUNE_ANCHOR_MAX || v[i] > TUNE_ANCHOR_MAX) return 0;
  }

  pitch_params.fifth_milli = fifth;
  pitch_params.m00 = v[0]; pitch_params.m01 = v[1];
  pitch_params.m10 = v[2]; pitch_params.m11 = v[3];
  pitch_params.aw  = v[4]; pitch_params.ah  = v[5];
  // Sounding voices keep the note they started with (see mpe_voice_t), so a
  // retune under a held key cannot strand it -- deliberately nothing to do here.
  return 1;
}

// ============================ Procedural colour ==============================
// The same idea as the pitch map above, and the same shape on the wire: a small
// block of parameters, evaluated at an ABSOLUTE grid coordinate, so one push
// colours a grid of any size and every board can work out its own keys.

// The live colour generator. Seeded to the pattern this board showed before 'K'
// existed -- Bosanquet rows in red / orange / yellow / green / blue at 10%
// brightness -- so a board that never hears from the companion looks exactly as
// it always did. companion/scripts/check-firmware-colors.py asserts that byte
// for byte against the hardcoded fill_bosanquet() this replaced.
typedef struct {
  int16_t m00, m01, m10, m11;   /* grid -> (w, h) basis change */
  int16_t aw, ah;               /* anchor, with the generator's (x, y) offset folded in */
  int8_t  start_acc;            /* the accidental that receives pal[0] */
  uint8_t pal_n;                /* 1..COLORGEN_PAL_MAX */
  uint8_t flags;                /* COLORGEN_LIGHTEN_CDE */
  uint8_t brightness;           /* percent, 1..100 */
  uint8_t pal[COLORGEN_PAL_MAX][3];   /* RGB at FULL brightness */
} colorgen_params_t;

static volatile colorgen_params_t colorgen = {
  1, 0, 0, 1, TUNE_W0, TUNE_H0,
  -2,   /* pal[0] is the double-flat row */
  5, 0, 10,
  { {250, 20, 20}, {250, 100, 20}, {200, 200, 20}, {50, 250, 50}, {50, 50, 250} },
};

// Whether led_background[] is generated from those parameters or was written
// verbatim by a 'C'/'L' frame. The master reports it on the 'i' line, and it is
// what a future per-key editor branches on.
enum { COLOR_MODE_PROCEDURAL = 0, COLOR_MODE_EXPLICIT };
static volatile uint8_t color_mode = COLOR_MODE_PROCEDURAL;

// Floor division and non-negative modulo. C's / and % truncate toward zero,
// which is the wrong rounding for an accidental: a grid coordinate on a
// neighbouring board is routinely negative, and floor((-1+1)/7) must be 0 while
// (-6)/7 in C is 0 too but (-8)/7 is -1 where floor wants -2.
static int32_t floor_div(int32_t num, int32_t den)
{
  int32_t q = num / den;
  if ((num % den) != 0 && ((num < 0) != (den < 0))) q--;
  return q;
}

static int32_t mod_pos(int32_t v, int32_t m)
{
  int32_t r = v % m;
  return (r < 0) ? r + m : r;
}

// Round to nearest on non-negative operands, matching JavaScript's Math.round
// (which the companion's toHex() applies) so the two sides agree byte for byte.
static uint8_t round_pct(uint32_t num, uint32_t den)
{
  uint32_t v = (num + den / 2) / den;
  return (uint8_t)((v > 255u) ? 255u : v);
}

// Colour for ANY absolute grid coordinate. Taking a coordinate rather than an
// LED index is what lets a board in the middle of a grid colour itself.
static void colorgen_for_xy(int32_t x, int32_t y, uint8_t *rgb)
{
  // Snapshot first: a 'K' frame can be installed between any two reads, and a
  // torn set would mix one palette with another's geometry.
  colorgen_params_t p = colorgen;

  int32_t w = p.m00 * x + p.m01 * y + p.aw;
  int32_t h = p.m10 * x + p.m11 * y + p.ah;

  int32_t acc = floor_div(2 * w - 5 * h + 1, 7);   /* the Bosanquet row */
  int32_t pc7 = mod_pos(w + h, 7);                 /* letter index, C D E = 0 1 2 */

  uint8_t n = (p.pal_n == 0 || p.pal_n > COLORGEN_PAL_MAX) ? 1 : p.pal_n;
  uint8_t idx = (uint8_t)mod_pos(acc - p.start_acc, n);   /* rows beyond the palette wrap */

  uint8_t lighten = (p.flags & COLORGEN_LIGHTEN_CDE) && pc7 < 3;
  for (int c = 0; c < 3; c++) {
    uint32_t v = p.pal[idx][c];
    // Lighten first, then scale -- the order the companion's generator uses,
    // rounding at both steps as its toHex() does. Reversing them would differ
    // by a count on some channels, and this is checked byte for byte.
    if (lighten) {
      v = round_pct(v * (100 - COLORGEN_CDE_PCT) + 255u * COLORGEN_CDE_PCT, 100);
    }
    rgb[c] = round_pct(v * p.brightness, 100);
  }
}

// Paint every key of THIS board from the generator, at its discovered place in
// the grid. led_background is RGB and indexed by LED, while the coordinates are
// per sensor, so it walks sensors and uses led_for_sensor[].
static void colorgen_render(void)
{
  int16_t ox = mesh_offset_x(), oy = mesh_offset_y();
  for (uint8_t s = 0; s < NUM_SENSORS; s++) {
    colorgen_for_xy(sensor_grid[s][0] + ox, sensor_grid[s][1] + oy,
                    led_background[led_for_sensor[s]]);
  }
}

// Install a pushed generator, rejecting a frame that cannot be one. These bytes
// come from a host, and a wild basis change would put every key on one row --
// a uniformly coloured board with no way to tell why.
// Returns 0 if the frame was rejected.
static uint8_t colorgen_params_apply(const uint8_t *m)
{
  int16_t v[6];
  for (int i = 0; i < 6; i++) {
    v[i] = (int16_t)((uint16_t)m[i * 2] | ((uint16_t)m[i * 2 + 1] << 8));
  }
  for (int i = 0; i < 4; i++) {
    if (v[i] < -TUNE_MATRIX_MAX || v[i] > TUNE_MATRIX_MAX) return 0;
  }
  if (v[0] * v[3] - v[1] * v[2] == 0) return 0;   /* singular: every row collapses onto one */
  for (int i = 4; i < 6; i++) {
    if (v[i] < -TUNE_ANCHOR_MAX || v[i] > TUNE_ANCHOR_MAX) return 0;
  }

  uint8_t pal_n = m[13];
  if (pal_n == 0 || pal_n > COLORGEN_PAL_MAX) return 0;
  uint8_t brightness = m[15];
  if (brightness == 0 || brightness > 100) return 0;

  colorgen.m00 = v[0]; colorgen.m01 = v[1];
  colorgen.m10 = v[2]; colorgen.m11 = v[3];
  colorgen.aw  = v[4]; colorgen.ah  = v[5];
  colorgen.start_acc  = (int8_t)m[12];
  colorgen.pal_n      = pal_n;
  colorgen.flags      = m[14];
  colorgen.brightness = brightness;
  for (uint8_t i = 0; i < COLORGEN_PAL_MAX; i++) {
    for (int c = 0; c < 3; c++) colorgen.pal[i][c] = m[16 + i * 3 + c];
  }
  return 1;
}

// Serialise the live generator back into the 40 wire bytes, so the master can
// hand what it holds to the mesh layer without keeping a second copy.
static void colorgen_params_pack(uint8_t *m)
{
  colorgen_params_t p = colorgen;
  const int16_t v[6] = { p.m00, p.m01, p.m10, p.m11, p.aw, p.ah };
  for (int i = 0; i < 6; i++) {
    m[i * 2]     = (uint8_t)v[i];
    m[i * 2 + 1] = (uint8_t)((uint16_t)v[i] >> 8);
  }
  m[12] = (uint8_t)p.start_acc;
  m[13] = p.pal_n;
  m[14] = p.flags;
  m[15] = p.brightness;
  for (uint8_t i = 0; i < COLORGEN_PAL_MAX; i++) {
    for (int c = 0; c < 3; c++) m[16 + i * 3 + c] = p.pal[i][c];
  }
}

static void midi_reset_voices(void)
{
  memset(mpe_voice, 0, sizeof(mpe_voice));
  mpe_alloc_counter = 0;
}

// MPE Configuration Message: lower zone, master channel 1, 15 member channels,
// then pitch-bend sensitivity on each member channel.
static void midi_send_mpe_setup(void)
{
  midi_send(MIDI_CIN_CC, 0xB0, 101, 0);   // RPN MSB
  midi_send(MIDI_CIN_CC, 0xB0, 100, 6);   // RPN LSB = 6 (MCM)
  midi_send(MIDI_CIN_CC, 0xB0, 6, MPE_MEMBER_COUNT);

  for (uint8_t c = 0; c < MPE_MEMBER_COUNT; c++) {
    uint8_t status = (uint8_t)(0xB0 | (MPE_MEMBER_FIRST + c));
    midi_send(MIDI_CIN_CC, status, 101, 0);  // RPN 0 = pitch bend sensitivity
    midi_send(MIDI_CIN_CC, status, 100, 0);
    midi_send(MIDI_CIN_CC, status, 6, MPE_BEND_SEMITONES);
    midi_send(MIDI_CIN_CC, status, 38, 0);
  }
}

static void midi_note_on(int16_t x, int16_t y, uint8_t vel)
{
  uint8_t note;
  uint16_t bend;
  if (!pitch_for_xy(x, y, &note, &bend)) return;
  uint32_t id = key_id(x, y);

  for (uint8_t c = 0; c < MPE_MEMBER_COUNT; c++) {
    if (mpe_voice[c].active && mpe_voice[c].key == id) return;   // already sounding
  }

  // Least-recently-used free channel, stealing the oldest voice if all busy.
  uint8_t pick = 0;
  uint32_t best = 0xFFFFFFFFu;
  for (uint8_t c = 0; c < MPE_MEMBER_COUNT; c++) {
    uint32_t age = mpe_voice[c].active ? mpe_voice[c].age + 0x80000000u
                                       : mpe_voice[c].age;
    if (age < best) {
      best = age;
      pick = c;
    }
  }

  uint8_t status_ch = (uint8_t)(MPE_MEMBER_FIRST + pick);
  if (mpe_voice[pick].active) {
    midi_send(MIDI_CIN_NOTE_OFF, (uint8_t)(0x80 | status_ch), mpe_voice[pick].note, 0);
  }

  mpe_voice[pick].active = 1;
  mpe_voice[pick].key = id;
  mpe_voice[pick].note = note;
  mpe_voice[pick].age = ++mpe_alloc_counter;

  // Bend first so the note starts already in tune.
  midi_send(MIDI_CIN_PITCHBEND, (uint8_t)(0xE0 | status_ch),
            (uint8_t)(bend & 0x7F), (uint8_t)((bend >> 7) & 0x7F));
  midi_send(MIDI_CIN_NOTE_ON, (uint8_t)(0x90 | status_ch), note, vel);
}

static void midi_note_off(int16_t x, int16_t y)
{
  uint32_t id = key_id(x, y);
  for (uint8_t c = 0; c < MPE_MEMBER_COUNT; c++) {
    if (!mpe_voice[c].active || mpe_voice[c].key != id) continue;
    midi_send(MIDI_CIN_NOTE_OFF,
              (uint8_t)(0x80 | (MPE_MEMBER_FIRST + c)), mpe_voice[c].note, 0);
    mpe_voice[c].active = 0;
    return;
  }
}

// Restart the boot-time rest calibration (also triggered by the 'r' command).
static void keys_recalibrate_rest(void)
{
  memset(rest_acc, 0, sizeof(rest_acc));
  rest_scans = 0;
}

// Two-threshold transit time -> 1..127, log-mapped so each doubling of speed
// adds a fixed velocity increment (the same principle piano keybeds use).
static uint8_t velocity_from_dt(uint32_t dt_us)
{
  if (dt_us <= VEL_DT_FAST_US) return 127;
  if (dt_us >= VEL_DT_SLOW_US) return 1;
  float f = logf((float)VEL_DT_SLOW_US / (float)dt_us)
          / logf((float)VEL_DT_SLOW_US / (float)VEL_DT_FAST_US);
  int v = 1 + (int)(126.0f * f + 0.5f);
  return (uint8_t)(v < 1 ? 1 : (v > 127 ? 127 : v));
}

// --- Seam shared by local and remote keys -------------------------------------
// Everything below here is driven identically by this board's own scan loop and
// by events arriving from a neighbour over the mesh. Coordinates are absolute,
// so a remote key needs no special case anywhere downstream.

void Miso_EmitKeyDown(uint8_t sensor, int16_t x, int16_t y, uint8_t vel, uint32_t dt_us)
{
  midi_note_on(x, y, vel);
  if (events_text_on) {
    char line[96];
    snprintf(line, sizeof(line), "EV %u DOWN vel=%u dt_us=%lu x=%d y=%d\r\n",
             sensor, vel, (unsigned long)dt_us, x, y);
    cdc_send_text(line);
  }
}

void Miso_EmitKeyUp(uint8_t sensor, int16_t x, int16_t y)
{
  midi_note_off(x, y);
  if (events_text_on) {
    char line[96];
    snprintf(line, sizeof(line), "EV %u UP x=%d y=%d\r\n", sensor, x, y);
    cdc_send_text(line);
  }
}

void Miso_ApplyColors(const uint8_t *rgb)
{
  memcpy(led_background, rgb, sizeof(led_background));
  // A hand-painted board stops being procedural. It keeps the generator seq it
  // already holds, so the master's repeating beacon does not paint over this.
  color_mode = COLOR_MODE_EXPLICIT;
}

uint8_t Miso_ApplyColorGen(const uint8_t *p)
{
  if (!colorgen_params_apply(p)) return 0;
  color_mode = COLOR_MODE_PROCEDURAL;
  colorgen_render();
  return 1;
}

void Miso_PackColorGen(uint8_t *p)
{
  colorgen_params_pack(p);
}

void Miso_SensorCoord(uint8_t sensor, int8_t *x, int8_t *y)
{
  if (sensor >= NUM_SENSORS) { *x = 0; *y = 0; return; }
  *x = sensor_grid[sensor][0];
  *y = sensor_grid[sensor][1];
}

uint32_t Miso_LocalHeldMask(void)
{
  uint32_t m = 0;
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (key_state[i] == KS_HELD) m |= (1u << i);
  }
  return m;
}

void Miso_SendText(const char *s)
{
  cdc_send_text(s);
}

static void keys_process(uint32_t t_us)
{
  if (rest_scans < REST_CAL_SCANS) {
    for (uint8_t i = 0; i < NUM_SENSORS; i++) rest_acc[i] += sensor_raw[i];
    if (++rest_scans == REST_CAL_SCANS) {
      for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        key_rest[i] = rest_acc[i] / (float)REST_CAL_SCANS;
        key_ema[i] = key_rest[i];
        key_state[i] = KS_IDLE;
        key_vel[i] = 0;
      }
    }
    return;
  }

  // Hoisted: these were two function calls per sensor per scan (62 per pass),
  // which at -O0 cost measurable scan rate for a value that cannot change
  // inside the loop.
  const int16_t box = mesh_offset_x();
  const int16_t boy = mesh_offset_y();

  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    key_ema[i] += KEY_EMA_ALPHA * ((float)sensor_raw[i] - key_ema[i]);
    float pos = (key_ema[i] - key_rest[i]) / ((float)key_cal_max[i] - key_rest[i]);
    int16_t kx = (int16_t)(sensor_grid[i][0] + box);
    int16_t ky = (int16_t)(sensor_grid[i][1] + boy);

    switch (key_state[i]) {
      case KS_IDLE:
        if (pos > KEY_START_POS) {
          key_state[i] = KS_PRESSING;
          key_t0[i] = t_us;
        }
        break;
      case KS_PRESSING:
        if (pos > KEY_END_POS) {
          uint32_t dt = t_us - key_t0[i];   // wrap-safe
          key_vel[i] = velocity_from_dt(dt);
          key_state[i] = KS_HELD;
          mesh_key_down(i, kx, ky, key_vel[i], dt);
        } else if (pos < KEY_ABORT_POS) {
          key_state[i] = KS_IDLE;           // grazed, never committed
        }
        break;
      case KS_HELD:
        if (pos < KEY_RELEASE_POS) {
          key_state[i] = KS_IDLE;
          key_vel[i] = 0;
          mesh_key_up(i, kx, ky);
        }
        break;
    }
  }
}

// Command parser, called from the USB interrupt (usbd_cdc_if.c) — only sets
// flags/values; all TX happens in the main loop.
//   i        info line          s/x      start/stop streaming
//   d<N>     stream every Nth scan       l        toggle LED visualization
//   r        redo the rest calibration (hands off the keys)
//   k        dump the state of the four inter-board links
//   T        dump the mesh topology (ports, known boards, counters)
//   L        followed by int16 x, int16 y, then 93 RGB bytes: set the colors
//            of the board at that grid origin, wherever it is in the mesh
//   B!       reboot into the USB DFU bootloader (two bytes, to avoid misfires)
//   f        toggle the LED line float diagnostic (see led_float_diag)
//   F        self-test that diagnostic's edge counter (see led_edge_selftest)
//   C        followed by 93 bytes: RGB for all 31 LEDs, LED-chain order
//   P        followed by 16 bytes: the pitch map -- int32 fifth in millicents,
//            int16 m00, m01, m10, m11 (grid -> (w, h) basis change), int16
//            anchor w, h. Little-endian, validated before it is installed.
//   K        followed by 40 bytes: the colour generator -- int16 m00, m01, m10,
//            m11, int16 anchor w, h, int8 start accidental, u8 palette length,
//            u8 flags, u8 brightness percent, then 8 x RGB at full brightness.
//            Little-endian, validated, and propagated down the mesh.
void Miso_CDC_OnRx(uint8_t *buf, uint32_t len)
{
  static uint8_t collecting_decim = 0;
  static uint32_t decim_val = 0;
  static uint8_t collecting_thr = 0;
  static uint32_t thr_val = 0;
  static uint8_t dfu_armed = 0;

  last_rx_tick = HAL_GetTick();
  for (uint32_t i = 0; i < len; i++) {
    uint8_t c = buf[i];
    if (cq_active) {
      color_q[cq_head][cq_fill++] = c;
      if (cq_fill >= COLOR_MSG_LEN) {
        cq_active = 0;
        uint8_t next = (uint8_t)((cq_head + 1u) % COLOR_Q_SLOTS);
        // Queue full: drop this frame rather than overwrite one the main loop
        // has not consumed. The slot is simply reused by the next frame.
        if (next != cq_tail) cq_head = next;
        else cq_dropped++;
      }
      continue;
    }
    if (pq_active) {
      pq_buf[pq_fill++] = c;
      if (pq_fill >= PITCH_MSG_LEN) {
        pq_active = 0;
        pq_ready = 1;
      }
      continue;
    }
    if (kq_active) {
      kq_buf[kq_fill++] = c;
      if (kq_fill >= COLORGEN_MSG_LEN) {
        kq_active = 0;
        kq_ready = 1;
      }
      continue;
    }
    // 'B' then '!' enters DFU. Two bytes rather than one so a stray character
    // on the port can never reboot the board out from under the host.
    if (dfu_armed) {
      dfu_armed = 0;
      if (c == '!') { dfu_req = 1; continue; }
      // not the confirmation: fall through so c is still read as a command
    }
    if (collecting_thr) {
      if (c >= '0' && c <= '9') {
        thr_val = thr_val * 10 + (c - '0');
        continue;
      }
      scan_throttle_us = thr_val;
      collecting_thr = 0;  // fall through: c may start a new command
    }
    if (collecting_decim) {
      if (c >= '0' && c <= '9') {
        decim_val = decim_val * 10 + (c - '0');
        continue;
      }
      if (decim_val > 0) stream_decim = decim_val;
      collecting_decim = 0;  // fall through: c may start a new command
    }
    switch (c) {
      case 'i': info_req = 1; break;
      case 'k': link_req = 1; break;
      case 'B': dfu_armed = 1; break;
      case 's': stream_on = 1; break;
      case 'x': stream_on = 0; break;
      case 'l': led_viz_on ^= 1; break;
      case 'r': keys_recalibrate_rest(); break;
      case 'd': collecting_decim = 1; decim_val = 0; break;
      case 'C': {
        // "this board": synthesise the target so 'C' and 'L' share one path.
        int16_t ox = mesh_offset_x(), oy = mesh_offset_y();
        color_q[cq_head][0] = (uint8_t)ox;
        color_q[cq_head][1] = (uint8_t)(ox >> 8);
        color_q[cq_head][2] = (uint8_t)oy;
        color_q[cq_head][3] = (uint8_t)(oy >> 8);
        cq_active = 1; cq_fill = 4;
        break;
      }
      case 'L': cq_active = 1; cq_fill = 0; break;
      case 'P': pq_active = 1; pq_fill = 0; break;
      case 'K': kq_active = 1; kq_fill = 0; break;
      case 'T': topo_req = 1; break;
      case 'p': collecting_thr = 1; thr_val = 0; break;
      case 'n': scan_enabled ^= 1; break;
      case 'f': led_float_diag ^= 1; break;
      case 'F': led_selftest_req = 1; break;
      case 'M': mpe_setup_req = 1; break;
      case 'e': events_text_on ^= 1; break;
      default: break;  // ignore CR/LF and unknown bytes
    }
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  // If the previous boot requested DFU (double-tap reset or Bootloader_RequestDFU),
  // jump to the ROM bootloader now, before any clocks/peripherals are configured.
  bootloader_check();

  // Arm the double-tap window: if reset is pressed again before the window
  // closes below, the next boot enters the USB DFU bootloader.
  bootloader_flag = BOOTLOADER_MAGIC;

  // We may have been entered by a jump rather than a reset, in which case the
  // machine is NOT in its reset state and the code below would otherwise run
  // on top of someone else's configuration.
  //
  // That is how flashing ends: STM32_Programmer_CLI's -g leaves DFU by jumping
  // straight here, so everything the ROM bootloader set up is still live. Its
  // PLL settings make SystemClock_Config() fail, which lands in
  // Error_Handler(); its USB interrupt is still enabled in the NVIC, and a
  // pending one fires into a handler whose driver state has not been
  // initialised yet. Either way the board hangs before the boot ripple ever
  // runs -- LEDs frozen on whatever DFU left on them -- and only a power cycle
  // brings it back.
  //
  // Interrupts are silenced first, because one can fire the moment the vector
  // table becomes ours. The clock tree is put back to reset values (HSI, PLL
  // off) after HAL_Init, since HAL_RCC_DeInit times its waits with HAL_GetTick
  // and needs SysTick running. Both are no-ops on a normal reset boot.
  //
  // Ordering note: all of this runs AFTER bootloader_check(), so a mistake in
  // here can never take the double-tap DFU recovery path down with it.
  __disable_irq();
  for (uint32_t i = 0; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
    NVIC->ICER[i] = 0xFFFFFFFFu;   // disable
    NVIC->ICPR[i] = 0xFFFFFFFFu;   // and drop anything already pending
  }
  __enable_irq();

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  // The other half of the jump-entry cleanup above: back to HSI with the PLL
  // off, so SystemClock_Config() below configures the PLL from reset values
  // rather than failing on one that is already running.
  HAL_RCC_DeInit();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_LPUART1_UART_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_TIM1_Init();
  MX_USB_Device_Init();
  /* USER CODE BEGIN 2 */

  // Close the double-tap DFU window. A second reset press must land within
  // this delay (plus init time above) to enter the bootloader.
  HAL_Delay(BOOTLOADER_TAP_WINDOW);
  bootloader_flag = 0;

  dwt_init();
  midi_reset_voices();
  link_init();
  mesh_init();

  // Start dark. The ripple below wakes the board up into its colour mapping,
  // and it also replaces the old red/green/blue/white flash test: the crest is
  // white, so every LED still has all three channels driven on the way past,
  // and a dead one shows up as a gap in a moving wave rather than in a static
  // field.
  fill_solid(0, 0, 0);
  show_leds();

  // A board in a grid does not know its own place yet, and its colours are a
  // function of it -- so wait, dark, for a parent to hand over the generator
  // rather than rippling up into the wrong pattern and snapping a second later.
  //
  // Two exits besides the timeout, and between them they cover every way a
  // board can actually be powered: USB makes us master at (0,0), and a pogo
  // connector brings a parent with the parameters. Link bring-up measures
  // ~500-700 ms, so the ceiling is a backstop, not the usual cost.
  uint32_t wait_t0 = HAL_GetTick();
  while (!mesh_is_master() && !mesh_has_colorgen() &&
         (HAL_GetTick() - wait_t0) < COLORGEN_WAIT_MS) {
    link_tick(HAL_GetTick());
    mesh_tick(HAL_GetTick());
  }

  // Now paint the mapping at whatever place in the grid we settled on. Anything
  // that arrived during the wait is already in colorgen; otherwise these are the
  // compiled-in defaults, which reproduce the pattern this board always showed.
  colorgen_render();
  boot_wave_play();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t scan_count = 0;
  uint32_t rate_scans = 0;
  uint32_t rate_t0 = HAL_GetTick();
  uint32_t last_led_tick = 0;
  uint8_t  mpe_setup_done = 0;

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (scan_throttle_us) delay_us(scan_throttle_us);
    if (scan_enabled) scan_all();
    uint32_t t_us = micros32();
    if (scan_enabled) keys_process(t_us);
    scan_count++;
    rate_scans++;

    uint32_t tick = HAL_GetTick();

    // Announce the MPE zone once the host has configured the device (and on
    // demand via 'M'), then keep the MIDI endpoint draining.
    if (mpe_setup_req || (!mpe_setup_done && USBD_MIDI_Ready() && tick > 1500)) {
      mpe_setup_req = 0;
      mpe_setup_done = 1;
      midi_send_mpe_setup();
    }
    if (!USBD_MIDI_Ready()) {
      mpe_setup_done = 0;   // re-announce after a re-enumeration
    }
    if (!led_critical()) USBD_MIDI_Flush();

    link_tick(tick);
    mesh_tick(tick);

    if (tick - rate_t0 >= 1000) {
      scan_hz = rate_scans * 1000u / (tick - rate_t0);
      rate_scans = 0;
      rate_t0 = tick;
    }

    if (stream_on && (scan_count % stream_decim) == 0) {
      stream_frame(t_us);
    }

    if (info_req) {
      info_req = 0;
      // 288, not 224: the line runs to ~219 bytes at its nominal values, and the
      // counters on it are unbounded.
      char line[288];
      snprintf(line, sizeof(line),
               "INFO fw=%s scan_hz=%lu decim=%lu stream=%u leds=%u links=%c%c%c%c usb=%u cdrop=%lu "
               "usbdef=%lu thr=%lu scan=%u ledchurn=%lu dmachurn=%lu "
               "float=%u idleedges=%lu "
               "fifth=%ld M=%d,%d,%d,%d anchor=%d,%d preject=%lu "
               "cmode=%s cseq=%u kreject=%lu\r\n",
               FW_VERSION, (unsigned long)scan_hz, (unsigned long)stream_decim,
               stream_on, led_viz_on,
               link_state_char(LINK_PORT_TOP), link_state_char(LINK_PORT_BOTTOM),
               link_state_char(LINK_PORT_LEFT), link_state_char(LINK_PORT_RIGHT),
               link_self_has_usb(), (unsigned long)cq_dropped,
               (unsigned long)usb_defers,
               (unsigned long)scan_throttle_us, scan_enabled,
               (unsigned long)led_churn, (unsigned long)dma_churn,
               led_float_diag, (unsigned long)led_idle_edges,
               (long)pitch_params.fifth_milli,
               pitch_params.m00, pitch_params.m01, pitch_params.m10, pitch_params.m11,
               pitch_params.aw, pitch_params.ah, (unsigned long)pq_rejected,
               (color_mode == COLOR_MODE_PROCEDURAL) ? "proc" : "expl",
               mesh_colorgen_seq(), (unsigned long)kq_rejected);
      cdc_send_text(line);
    }

    if (link_req) {
      link_req = 0;
      for (link_port_t lp = 0; lp < LINK_PORT_COUNT; lp++) {
        uint32_t rx = 0, tx = 0, err = 0;
        link_stats(lp, &rx, &tx, &err);
        uint32_t e_uart = 0, e_rxfull = 0, e_txfull = 0, e_sum = 0;
        link_err_breakdown(lp, &e_uart, &e_rxfull, &e_txfull, &e_sum);
        char line[160];
        snprintf(line, sizeof(line),
                 "LINK %-6s %-9s peer=%08lX pp=%u usb=%u rx=%lu tx=%lu err=%lu"
                 " (uart=%lu rxfull=%lu txfull=%lu sum=%lu)\r\n",
                 link_port_name(lp), link_state_name(lp),
                 (unsigned long)link_peer_uid(lp)[0], link_peer_port(lp),
                 link_peer_has_usb(lp),
                 (unsigned long)rx, (unsigned long)tx, (unsigned long)err,
                 (unsigned long)e_uart, (unsigned long)e_rxfull,
                 (unsigned long)e_txfull, (unsigned long)e_sum);
        cdc_send_text(line);
      }
    }

    // Reboot into the ROM bootloader on request. Done here rather than in the
    // CDC callback so the acknowledgement makes it onto the wire first (that
    // runs in USB interrupt context, where nothing would ever drain the TX).
    if (dfu_req) {
      dfu_req = 0;
      cdc_send_text("DFU entering bootloader\r\n");
      led_dfu_indicate();
      HAL_Delay(50);
      Bootloader_RequestDFU();
    }

    // Drain every queued color frame; each targets one board in the grid.
    while (cq_tail != cq_head) {
      const uint8_t *m = (const uint8_t *)color_q[cq_tail];
      int16_t tx = (int16_t)((uint16_t)m[0] | ((uint16_t)m[1] << 8));
      int16_t ty = (int16_t)((uint16_t)m[2] | ((uint16_t)m[3] << 8));
      mesh_set_colors(tx, ty, m + 4);
      cq_tail = (uint8_t)((cq_tail + 1u) % COLOR_Q_SLOTS);
    }
    if (topo_req) {
      topo_req = 0;
      mesh_dump();
    }
    if (led_selftest_req) {
      led_selftest_req = 0;
      led_edge_selftest();
    }
    if (pq_ready) {
      pq_ready = 0;
      if (!pitch_params_apply((const uint8_t *)pq_buf)) pq_rejected++;
    }
    if (kq_ready) {
      kq_ready = 0;
      if (colorgen_params_apply((const uint8_t *)kq_buf)) {
        color_mode = COLOR_MODE_PROCEDURAL;
        colorgen_render();
        // Hand the accepted parameters to the mesh, which bumps the sequence
        // number and beacons them down the tree so every board repaints itself.
        mesh_set_colorgen((const uint8_t *)kq_buf);
      } else {
        kq_rejected++;
      }
    }
    if (cq_active && (tick - last_rx_tick) > COLOR_RX_TIMEOUT_MS) {
      cq_active = 0;   // half-received frame: abandon the slot, keep the queue
    }
    if (pq_active && (tick - last_rx_tick) > COLOR_RX_TIMEOUT_MS) {
      pq_active = 0;   // likewise, so a truncated 'P' cannot wedge the parser
    }
    if (kq_active && (tick - last_rx_tick) > COLOR_RX_TIMEOUT_MS) {
      kq_active = 0;   // and a truncated 'K'
    }

    // LED rendering, decimated so the WS2812 DMA (~1.6 ms per refresh)
    // doesn't eat into the scan rate. Base coat = host-set background colors;
    // a held key's own color is lifted toward full brightness with strike
    // velocity (unlit keys get a faint warm glow so feedback never vanishes).
    // 'l' blanks the LEDs entirely for clean noise measurements.
    if (tick - last_led_tick >= 30) {
      last_led_tick = tick;
      if (!led_viz_on) {
        fill_solid(0, 0, 0);
      } else {
        for (uint8_t l = 0; l < NUM_LEDS; l++) {
          set_pixel(l, led_background[l][0], led_background[l][1], led_background[l][2]);
        }
        for (uint8_t i = 0; i < NUM_SENSORS; i++) {
          if (key_state[i] != KS_HELD) continue;
          uint8_t l = led_for_sensor[i];
          uint32_t r = led_background[l][0];
          uint32_t g = led_background[l][1];
          uint32_t b = led_background[l][2];
          uint32_t maxc = r > g ? r : g;
          if (b > maxc) maxc = b;
          if (maxc == 0) {
            uint32_t w = 10 + (key_vel[i] * 60u) / 127u;
            set_pixel(l, (uint8_t)w, (uint8_t)w, (uint8_t)(w * 3 / 4));
          } else if (maxc < GLOW_TARGET) {
            // scale = (maxc*127 + (target-maxc)*vel) / (maxc*127):
            // 1.0 at vel 0, target/maxc at vel 127 — same hue, brighter
            uint32_t denom = maxc * 127u;
            uint32_t num = denom + (GLOW_TARGET - maxc) * key_vel[i];
            uint32_t rr = r * num / denom, gg = g * num / denom, bb = b * num / denom;
            set_pixel(l, (uint8_t)(rr > 255 ? 255 : rr),
                         (uint8_t)(gg > 255 ? 255 : gg),
                         (uint8_t)(bb > 255 ? 255 : bb));
          }
          // colors already at/above GLOW_TARGET stay as they are
        }
      }
      show_leds();
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV3;
  RCC_OscInitStruct.PLL.PLLN = 108;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV6;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.GainCompensation = 0;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_92CYCLES_5;
  sConfig.SingleDiff = ADC_DIFFERENTIAL_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPUART1_UART_Init(void)
{

  /* USER CODE BEGIN LPUART1_Init 0 */

  /* USER CODE END LPUART1_Init 0 */

  /* USER CODE BEGIN LPUART1_Init 1 */

  /* USER CODE END LPUART1_Init 1 */
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 460800;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPUART1_Init 2 */

  /* USER CODE END LPUART1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 460800;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 460800;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart2, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart2, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 179;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.BreakFilter = 0;
  sBreakDeadTimeConfig.BreakAFMode = TIM_BREAK_AFMODE_INPUT;
  sBreakDeadTimeConfig.Break2State = TIM_BREAK2_DISABLE;
  sBreakDeadTimeConfig.Break2Polarity = TIM_BREAK2POLARITY_HIGH;
  sBreakDeadTimeConfig.Break2Filter = 0;
  sBreakDeadTimeConfig.Break2AFMode = TIM_BREAK_AFMODE_INPUT;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  // Own the output before the pin is handed to the timer below, so PA8 is
  // never high-impedance from here on. The edge counter for the 'f'
  // diagnostic is set up now too; it stays disabled until the diagnostic
  // wants it.
  led_line_hold_low();
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 1, 0);   // with the link UARTs, below USB_LP

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pins : PA4 PA5 PA6 PA7 */
  GPIO_InitStruct.Pin = GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
