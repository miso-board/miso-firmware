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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define NUM_LEDS        31
#define BITS_PER_LED    24
#define DMA_BUF_LEN     (NUM_LEDS * BITS_PER_LED + 750)   // +40 for reset

// Duty cycle values (adjust these if colours look wrong)
#define LED_CODE_0      45     // ~0.35 µs high
#define LED_CODE_1      110    // ~0.8 µs high

// USB DFU bootloader entry
#define BOOTLOADER_MAGIC        0x4D49534Fu  // "MISO"
#define BOOTLOADER_SYSMEM_BASE  0x1FFF0000u  // STM32G4 system memory (ROM bootloader)
#define BOOTLOADER_TAP_WINDOW   500u         // ms: second reset within this window enters DFU

// Hall sensor scanning + CDC streaming
#define NUM_SENSORS         31
#define CYCLES_PER_US       144u   // SYSCLK in MHz, for DWT cycle-counter timing
#define MUX_SETTLE_US       5u     // 4067 switch + ADC input settling after channel select
#define STREAM_MAGIC0       0xA5
#define STREAM_MAGIC1       0x5A
#define FRAME_TYPE_SCAN     0x01
#define STREAM_DECIM_DEFAULT 2u    // stream every Nth scan
#define FW_VERSION          "miso 0.3.0"

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

uint32_t dma_buffer[DMA_BUF_LEN];
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

void fill_bosanquet(
		uint8_t ds_r, uint8_t ds_g, uint8_t ds_b,
		uint8_t s_r, uint8_t s_g, uint8_t s_b,
		uint8_t n_r, uint8_t n_g, uint8_t n_b,
		uint8_t f_r, uint8_t f_g, uint8_t f_b,
		uint8_t df_r, uint8_t df_g, uint8_t df_b)
{
  for (int i = 0; i < NUM_LEDS; i++) {
	switch (i) {
	case 0:
	case 5:
	case 6:
	case 18:
	case 19:
		set_pixel(i, df_r, df_g, df_b);
		break;
	case 1:
	case 3:
	case 4:
	case 7:
	case 16:
	case 17:
	case 20:
        set_pixel(i, f_r, f_g, f_b);
        break;
	case 2:
	case 8:
	case 9:
	case 15:
	case 21:
	case 22:
	case 28:
		set_pixel(i, n_r, n_g, n_b);
		break;
	case 10:
	case 13:
	case 14:
	case 23:
	case 26:
	case 27:
	case 29:
		set_pixel(i, s_r, s_g, s_b);
		break;
	case 11:
	case 12:
	case 24:
	case 25:
	case 30:
		set_pixel(i, ds_r, ds_g, ds_b);
		break;
    }
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
  for (int i = 0; i < 750; i++) {
    dma_buffer[idx++] = 0;
  }
}

void show_leds(void)
{
  prepare_dma_buffer();
  HAL_TIM_PWM_Start_DMA(&htim1, TIM_CHANNEL_1, dma_buffer, DMA_BUF_LEN);
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM1)
  {
    HAL_TIM_PWM_Stop_DMA(&htim1, TIM_CHANNEL_1);
  }
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
uint32_t scan_hz = 0;  // measured full-scan rate, updated once per second

// Binary scan frame: A5 5A 01 | u32 t_us | 31 x u16 raw | u8 checksum(payload)
static void stream_frame(uint32_t t_us)
{
  static uint8_t buf[3 + 4 + NUM_SENSORS * 2 + 1];
  if (CDC_IsTxBusy()) return;  // drop the frame rather than stall the scan loop
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
  static char txt[128];
  size_t n = strlen(s);
  if (n > sizeof(txt)) n = sizeof(txt);
  uint32_t t0 = HAL_GetTick();
  while (CDC_IsTxBusy()) {
    if (HAL_GetTick() - t0 > 5) return;
  }
  memcpy(txt, s, n);
  CDC_Transmit_FS((uint8_t *)txt, (uint16_t)n);
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

enum { KS_IDLE, KS_PRESSING, KS_HELD };
static float    key_rest[NUM_SENSORS];
static float    key_ema[NUM_SENSORS];
static uint8_t  key_state[NUM_SENSORS];
static uint32_t key_t0[NUM_SENSORS];      // µs timestamp of START crossing
static uint8_t  key_vel[NUM_SENSORS];     // last key-down velocity, 1..127
static float    rest_acc[NUM_SENSORS];
static uint32_t rest_scans = 0;           // < REST_CAL_SCANS while calibrating

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

  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    key_ema[i] += KEY_EMA_ALPHA * ((float)sensor_raw[i] - key_ema[i]);
    float pos = (key_ema[i] - key_rest[i]) / ((float)key_cal_max[i] - key_rest[i]);
    char line[48];

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
          snprintf(line, sizeof(line), "EV %u DOWN vel=%u dt_us=%lu\r\n",
                   i, key_vel[i], (unsigned long)dt);
          cdc_send_text(line);
        } else if (pos < KEY_ABORT_POS) {
          key_state[i] = KS_IDLE;           // grazed, never committed
        }
        break;
      case KS_HELD:
        if (pos < KEY_RELEASE_POS) {
          key_state[i] = KS_IDLE;
          key_vel[i] = 0;
          snprintf(line, sizeof(line), "EV %u UP\r\n", i);
          cdc_send_text(line);
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
void Miso_CDC_OnRx(uint8_t *buf, uint32_t len)
{
  static uint8_t collecting_decim = 0;
  static uint32_t decim_val = 0;

  for (uint32_t i = 0; i < len; i++) {
    uint8_t c = buf[i];
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
      case 's': stream_on = 1; break;
      case 'x': stream_on = 0; break;
      case 'l': led_viz_on ^= 1; break;
      case 'r': keys_recalibrate_rest(); break;
      case 'd': collecting_decim = 1; decim_val = 0; break;
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

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

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

  // Simple test patterns (shortened so flash-test cycles stay quick)
  fill_solid(5, 0, 0);     // dim red
  show_leds();
  HAL_Delay(250);

  fill_solid(0, 5, 0);     // dim green
  show_leds();
  HAL_Delay(250);

  fill_solid(0, 0, 5);     // dim blue
  show_leds();
  HAL_Delay(250);

  fill_solid(5, 5, 5);   // white
  show_leds();
  HAL_Delay(250);

  fill_bosanquet(5, 5, 25, 5, 25, 5, 20, 20, 2, 25, 10, 2, 25, 2, 2);
  show_leds();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t scan_count = 0;
  uint32_t rate_scans = 0;
  uint32_t rate_t0 = HAL_GetTick();
  uint32_t last_led_tick = 0;

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    scan_all();
    uint32_t t_us = micros32();
    keys_process(t_us);
    scan_count++;
    rate_scans++;

    uint32_t tick = HAL_GetTick();
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
      char line[96];
      snprintf(line, sizeof(line), "INFO fw=%s scan_hz=%lu decim=%lu stream=%u leds=%u\r\n",
               FW_VERSION, (unsigned long)scan_hz, (unsigned long)stream_decim,
               stream_on, led_viz_on);
      cdc_send_text(line);
    }

    // Velocity feedback on the LEDs: a held key glows green with brightness
    // proportional to its strike velocity. Decimated so the WS2812 DMA
    // (~1.6 ms per refresh) doesn't eat into the scan rate; 'l' toggles it
    // off for clean noise measurements.
    if (led_viz_on && (tick - last_led_tick) >= 30) {
      last_led_tick = tick;
      fill_solid(0, 0, 0);
      for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        if (key_state[i] == KS_HELD) {
          // vel 1..127 maps directly to green, on the pressed key's own LED
          set_pixel(led_for_sensor[i], 0, key_vel[i], 0);
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
  hlpuart1.Init.BaudRate = 209700;
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
  huart1.Init.BaudRate = 115200;
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
  huart2.Init.BaudRate = 115200;
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
