#include "sensor_error.h"
uint16_t g_sensor_errormap = 0;
uint8_t g_sensor_failure_streak[12] = {0};
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
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "icm42670.h"
#include "sensor_ak09973d.h"
#include "sensor_tmag3001.h"
#include "tmag_debug.h"
#include "binary_writer.h"
#include "csv_writer.h"
#include <stdio.h>
#include <string.h>
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define PHASE_CAPTURE_CHANNEL_COUNT  3U
#define PHASE_CAPTURE_NO_EDGE        UINT32_MAX

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint32_t g_device_id;
volatile uint8_t g_tmag_init_debug = TMAG_INIT_DEBUG;
icm42670_raw_t imu;
icm42670_t icm = {
    .hspi = &hspi1,
    .cs_port = SPI1_CS_GPIO_Port,
    .cs_pin = SPI1_CS_Pin};
static volatile uint32_t phase_last_capture[PHASE_CAPTURE_CHANNEL_COUNT];
static volatile uint8_t phase_capture_valid[PHASE_CAPTURE_CHANNEL_COUNT];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#ifndef SENSOR_OUTPUT_ICM
#define SENSOR_OUTPUT_ICM   0
#endif
#ifndef SENSOR_OUTPUT_AK
#define SENSOR_OUTPUT_AK    1
#endif
#ifndef SENSOR_OUTPUT_TMAG
#define SENSOR_OUTPUT_TMAG  0
#endif
#if SENSOR_OUTPUT_AK && SENSOR_OUTPUT_TMAG
#error AK and TMAG must not be enabled together
#endif
#ifndef SENSOR_OUTPUT_FORMAT
#define SENSOR_OUTPUT_FORMAT SENSOR_OUTPUT_FORMAT_BINARY
#endif
#ifndef SENSOR_EMPTY_ROUND_PERIOD_MS
#define SENSOR_EMPTY_ROUND_PERIOD_MS 10U
#endif

static uint32_t DeviceId_FromUID(void)
{
    const uint32_t uid[3] = {
        HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()
    };
    uint32_t crc = 0xFFFFFFFFU;

    /* CRC-32/ISO-HDLC over w0, w1, w2, each serialized little-endian. */
    for (size_t word = 0; word < 3U; word++) {
        for (uint32_t byte = 0; byte < 4U; byte++) {
            crc ^= (uid[word] >> (8U * byte)) & 0xFFU;
            for (uint32_t bit = 0; bit < 8U; bit++) {
                crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static uint32_t PhaseCapture_EnterCritical(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return primask;
}

static void PhaseCapture_ExitCritical(uint32_t primask)
{
    __DMB();
    if (primask == 0U) {
        __enable_irq();
    }
}

static void PhaseCapture_Start(void)
{
    uint32_t primask = PhaseCapture_EnterCritical();

    for (uint32_t i = 0; i < PHASE_CAPTURE_CHANNEL_COUNT; i++) {
        phase_last_capture[i] = 0U;
        phase_capture_valid[i] = 0U;
    }
    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_CC1 | TIM_FLAG_CC2 | TIM_FLAG_CC3);

    PhaseCapture_ExitCritical(primask);

    if (HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_1) != HAL_OK ||
        HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_2) != HAL_OK ||
        HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_3) != HAL_OK) {
        Error_Handler();
    }
}

static void PhaseCapture_Sample(
    uint32_t phase_age_us[PHASE_CAPTURE_CHANNEL_COUNT], uint32_t *sample_us)
{
    static const uint32_t channels[PHASE_CAPTURE_CHANNEL_COUNT] = {
        TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_3
    };
    static const uint32_t flags[PHASE_CAPTURE_CHANNEL_COUNT] = {
        TIM_FLAG_CC1, TIM_FLAG_CC2, TIM_FLAG_CC3
    };
    uint32_t captured[PHASE_CAPTURE_CHANNEL_COUNT];
    uint8_t valid[PHASE_CAPTURE_CHANNEL_COUNT];
    uint32_t sample_count;
    uint32_t primask = PhaseCapture_EnterCritical();

    /* 此次 TIM2 读数同时作为轮首时间和三路相位年龄的计算基准。
     * 快照最近捕获的边沿，不等待新边沿；采样后到来的边沿留给下一轮。 */
    sample_count = __HAL_TIM_GET_COUNTER(&htim2);
    *sample_us = sample_count;
    for (uint32_t i = 0; i < PHASE_CAPTURE_CHANNEL_COUNT; i++) {
        captured[i] = phase_last_capture[i];
        valid[i] = phase_capture_valid[i];

        if (__HAL_TIM_GET_FLAG(&htim2, flags[i]) != RESET) {
            uint32_t pending_capture =
                HAL_TIM_ReadCapturedValue(&htim2, channels[i]);

            /* Include a pending edge only if it occurred no later than this sample. */
            if ((int32_t)(sample_count - pending_capture) >= 0) {
                captured[i] = pending_capture;
                valid[i] = 1U;
            }
        }
    }

    PhaseCapture_ExitCritical(primask);

    for (uint32_t i = 0; i < PHASE_CAPTURE_CHANNEL_COUNT; i++) {
        phase_age_us[i] = (valid[i] != 0U)
                            ? (sample_count - captured[i])
                            : PHASE_CAPTURE_NO_EDGE;
    }
}

// USB CDC发送字符串
extern USBD_HandleTypeDef hUsbDeviceFS;

static int USB_CDC_IsBusy(void)
{
    USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    return (hcdc != NULL && hcdc->TxState != 0U);
}

static int USB_Send_Buffer(const uint8_t *buf, uint16_t len)
{
    uint16_t sent = 0;

    while (sent < len) {
        uint16_t chunk = len - sent;

        uint32_t start = HAL_GetTick();
        while (USB_CDC_IsBusy()) {
            if ((HAL_GetTick() - start) > 100U) {
                return 0;
            }
        }

        start = HAL_GetTick();
        uint8_t result;
        while ((result = CDC_Transmit_FS((uint8_t *)(buf + sent), chunk)) == USBD_BUSY) {
            if ((HAL_GetTick() - start) > 100U) {
                return 0;
            }
        }
        if (result != USBD_OK) {
            return 0;
        }

        start = HAL_GetTick();
        while (USB_CDC_IsBusy()) {
            if ((HAL_GetTick() - start) > 100U) {
                return 0;
            }
        }

        sent += chunk;
    }
    return 1;
}

int USB_Send_Raw(const uint8_t *buf, uint16_t len)
{
    return USB_Send_Buffer(buf, len);
}

void USB_Send_String(char *str)
{
    size_t len = strlen(str);
    if (len > UINT16_MAX) {
        len = UINT16_MAX;
    }
    USB_Send_Buffer((const uint8_t *)str, (uint16_t)len);
}

static void USB_Send_PrintBuffer(char *ptr, int len)
{
    if (len <= 0) {
        return;
    }
    while (len > 0) {
        uint16_t chunk = (len > UINT16_MAX) ? UINT16_MAX : (uint16_t)len;
        USB_Send_Buffer((const uint8_t *)ptr, chunk);
        ptr += chunk;
        len -= chunk;
    }
}

int _write(int file, char *ptr, int len)
{
    (void)file;
#if SENSOR_OUTPUT_FORMAT == SENSOR_OUTPUT_FORMAT_CSV
    USB_Send_PrintBuffer(ptr, len);
#else
    if (g_tmag_init_debug != 0U) {
        USB_Send_PrintBuffer(ptr, len);
    }
#endif
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, len, 100);
    return len;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  // Immediate test - LED GPIO should work even before clock init
  // This confirms firmware is running
  // Note: Can't printf yet - clock not configured
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

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
  MX_I2C1_Init();
  MX_I2C2_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USB_DEVICE_Init();
  MX_I2C3_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */

  g_device_id = DeviceId_FromUID();

  HAL_GPIO_WritePin(LEDR_GPIO_Port, LEDR_Pin, SensorError_Count() >= 3 ? GPIO_PIN_RESET : GPIO_PIN_SET);
  HAL_GPIO_WritePin(LEDG_GPIO_Port, LEDG_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LEDB_GPIO_Port, LEDB_Pin, GPIO_PIN_SET);

  HAL_Delay(100);
  HAL_Delay(2000);  // 等待 USB CDC 准备好

  /* Give the host time to reopen the USB port after a board reset. */
  if (g_tmag_init_debug != 0U) {
      HAL_Delay(TMAG_INIT_DEBUG_WAIT_MS);
  }

  printf("=== MAG READ START ===\r\n");
  TMAG_INIT_LOG("[TMAG INIT] debug=1 build=%s %s wait_ms=%lu\r\n",
                __DATE__, __TIME__, (unsigned long)TMAG_INIT_DEBUG_WAIT_MS);

  if (ICM42670_Init(&icm) != HAL_OK)
  {
      printf("ICM init FAILED\r\n");
      /* Red LED is controlled by the shared fault count. */
  }
  else
  {
      printf("ICM init OK (WHO=0x67)\r\n");
  }

  int ak_count = 0;
  int tmag_count = 0;
  for (int attempt = 1; attempt <= 1; attempt++) {
      printf("MAG init attempt %d\r\n", attempt);
#if SENSOR_OUTPUT_AK
      Sensor_AK09973D_Init_All();
#endif
#if SENSOR_OUTPUT_TMAG
      Sensor_TMAG3001_Init_All();
#endif

#if SENSOR_OUTPUT_AK
      ak_count = Sensor_AK09973D_GetCount();
#else
      ak_count = AK09973D_COUNT;
#endif
#if SENSOR_OUTPUT_TMAG
      tmag_count = Sensor_TMAG3001_GetCount();
#else
      tmag_count = TMAG3001_TOTAL_NUM;
#endif
      printf("MAG init count AK=%d/%d TMAG=%d/%d\r\n",
             ak_count, AK09973D_COUNT, tmag_count, TMAG3001_TOTAL_NUM);

      if (ak_count == AK09973D_COUNT && tmag_count == TMAG3001_TOTAL_NUM) {
          HAL_GPIO_WritePin(LEDR_GPIO_Port, LEDR_Pin, SensorError_Count() >= 3 ? GPIO_PIN_RESET : GPIO_PIN_SET);
          break;
      }
      HAL_Delay(500);
      /* Red LED is controlled by the shared fault count. */
  }

  HAL_GPIO_WritePin(LEDR_GPIO_Port, LEDR_Pin, SensorError_Count() >= 3 ? GPIO_PIN_RESET : GPIO_PIN_SET);
  PhaseCapture_Start();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

#if SENSOR_OUTPUT_FORMAT == SENSOR_OUTPUT_FORMAT_BINARY
      static uint8_t binary_frame[4096];
#else
      static char frame[3072];
#endif
      uint32_t phase_us[PHASE_CAPTURE_CHANNEL_COUNT];
      uint32_t round_start_us;
      int n = 0;

#if SENSOR_OUTPUT_FORMAT == SENSOR_OUTPUT_FORMAT_BINARY
      uint32_t round_start_ms = HAL_GetTick();
#endif
      /* 每轮时序：轮首相位快照 → ICM → AK → TMAG → 附加记录 → 封帧发送。
       * 仅执行已启用的传感器读取；round_start_us 为此轮 TIM2 微秒基准，并读相位。 */
      PhaseCapture_Sample(phase_us, &round_start_us);
#if SENSOR_OUTPUT_FORMAT == SENSOR_OUTPUT_FORMAT_BINARY
      static uint32_t previous_timing_us[3] = {0};
      static uint32_t round_seq = 0;
      static uint32_t record_seq = 0;
      static uint32_t ak_frames = 0;
      static uint32_t tmag_frames = 0;
      static uint32_t icm_frames = 0;
      static uint32_t skipped = 0;
      static uint32_t errors = 0;
      static uint32_t usb_send_failures = 0;
      static uint32_t pending_usb_failures = 0;
      static uint32_t last_stats_ms = 0;
      const uint32_t ak_frames_before = ak_frames;
      const uint32_t tmag_frames_before = tmag_frames;
      const uint32_t icm_frames_before = icm_frames;
      size_t off = 0;
      int build_ok = BinaryWriter_BeginRound(binary_frame, sizeof(binary_frame),
                                             &off, round_seq++, round_start_us,
                                             round_start_us);

#if SENSOR_OUTPUT_ICM
      /* ① 相位快照后先读 ICM；读取结束后将有效数据追加到本轮缓冲区。
       * 记录中的 offset_us 是追加记录时相对轮首的时间，不是内部转换时刻。 */
      n = ICM42670_ReadToBinary(&icm, binary_frame + off,
                                sizeof(binary_frame) - off,
                                &record_seq, &icm_frames, &skipped, &errors);
      if (n > 0) off += (size_t)n;
#endif

#if SENSOR_OUTPUT_AK
      /* ② ICM 处理完后依次读取 AK 各通道；未启用 ICM 时从这里开始读传感器。
       * 各条有效记录分别标记读取完成偏移，因此同轮各通道并非同时采样。 */
      n = Sensor_AK09973D_ReadToBinary(binary_frame + off,
                                       sizeof(binary_frame) - off,
                                       &record_seq, &ak_frames, &skipped, &errors);
      if (n > 0) off += (size_t)n;
#endif

#if SENSOR_OUTPUT_TMAG
      /* ③ AK 处理完后依次读取 TMAG 各通道，数据继续追加到同一轮缓冲区。
       * 各条有效记录的读取完成偏移仍相对轮首 TIM2 计数计算。 */
      {
          n = Sensor_TMAG3001_ReadAllToBinary(binary_frame + off,
                                              sizeof(binary_frame) - off,
                                              &record_seq, &tmag_frames,
                                              &skipped, &errors);
          if (n > 0) off += (size_t)n;
      }
#endif

      if (pending_usb_failures != 0U) {
          build_ok &= BinaryWriter_AppendError(binary_frame, sizeof(binary_frame), &off,
                                                record_seq++, HAL_GetTick(),
                                                BINARY_ERR_SOURCE_USB, 1U,
                                                usb_send_failures);
      }
      /* ④ 传感器读取结束后追加 PHASE，但其内容仍是本轮开始时的相位快照。
       * 此处仅打包，不重新采集相位；有待报告的 USB 错误时先追加 ERR。 */
      build_ok &= BinaryWriter_AppendPhase(binary_frame, sizeof(binary_frame), &off,
                                           record_seq++, HAL_GetTick(), phase_us);

      uint32_t now = HAL_GetTick();
      if ((now - last_stats_ms) >= 1000U) {
          build_ok &= BinaryWriter_AppendStats(binary_frame, sizeof(binary_frame), &off,
                                               record_seq++, now,
                                               ak_frames, tmag_frames, icm_frames,
                                               skipped, errors, g_device_id, g_sensor_errormap, previous_timing_us);
          last_stats_ms = now;
      }
      /* ⑤ 到期时追加 STATS 后，填写整轮长度和 CRC，再通过 USB 发送整轮。
       * 发送处理结束后才进入轮尾等待/下一轮；此前传感器读取不逐条发送。 */
      uint32_t usb_start_us;
      uint32_t usb_elapsed_us = 0;
      if (!build_ok || !BinaryWriter_EndRound(binary_frame, sizeof(binary_frame), &off)) {
          errors++;
      } else {
          usb_start_us = __HAL_TIM_GET_COUNTER(&htim2);
          int usb_ok = USB_Send_Raw(binary_frame, (uint16_t)off);
          usb_elapsed_us = (uint32_t)(__HAL_TIM_GET_COUNTER(&htim2) - usb_start_us);
          if (!usb_ok) {
              errors++;
              usb_send_failures++;
              pending_usb_failures++;
          } else {
              pending_usb_failures = 0U;
          }
      }
#if SENSOR_OUTPUT_AK
      previous_timing_us[0] = g_ak_bus_read_us[0];
      previous_timing_us[1] = g_ak_bus_read_us[1];
#endif
      previous_timing_us[2] = usb_elapsed_us;

      /* ERR, PHASE and STATS do not count as sensor records. Include reads,
       * frame construction and USB waits, even when construction/send fails. */
      if (ak_frames == ak_frames_before &&
          tmag_frames == tmag_frames_before &&
          icm_frames == icm_frames_before) {
          uint32_t elapsed_ms = HAL_GetTick() - round_start_ms;
          if (elapsed_ms < SENSOR_EMPTY_ROUND_PERIOD_MS) {
              HAL_Delay(SENSOR_EMPTY_ROUND_PERIOD_MS - elapsed_ms);
          }
      }

#else
      (void)round_start_us;
      /* CSV 调试模式也按 ICM → AK → TMAG 顺序读取已启用的传感器。
       * 各类数据读完即发送，最后发送轮首相位快照，不聚合为二进制整轮帧。 */
#if SENSOR_OUTPUT_ICM
      n = ICM42670_ReadToCSV(&icm, frame, sizeof(frame));
      if (n > 0) {
          if (n < (int)sizeof(frame)) {
              frame[n] = '\0';
              USB_Send_String(frame);
          }
      }
#endif

#if SENSOR_OUTPUT_AK
      n = Sensor_AK09973D_ReadToCSV(frame, sizeof(frame));
      if (n > 0) {
          if (n < (int)sizeof(frame)) {
              frame[n] = '\0';
              USB_Send_String(frame);
          }
      }
#endif

#if SENSOR_OUTPUT_TMAG
      {
          n = Sensor_TMAG3001_ReadAllToCSV(frame, sizeof(frame));

          if (n > 0) {
              if (n < (int)sizeof(frame)) {
                  frame[n] = '\0';
                  USB_Send_String(frame);
              }
          }
      }
#endif

      {
          size_t off = 0;
          if (CSV_AppendPhase(frame, sizeof(frame), &off, phase_us) &&
              off < sizeof(frame)) {
              frame[off] = '\0';
              USB_Send_String(frame);
          }
      }
#endif

      // LED 状态指示
      HAL_GPIO_WritePin(LEDR_GPIO_Port, LEDR_Pin,
                        SensorError_Count() >= 3U ? GPIO_PIN_RESET : GPIO_PIN_SET);
      static uint32_t last_toggle = 0;
      if (HAL_GetTick() - last_toggle > 500)
      {
          HAL_GPIO_TogglePin(LEDG_GPIO_Port, LEDG_Pin);
          last_toggle = HAL_GetTick();
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

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 48;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 5;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV4;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    /* 三路上升沿由 TIM2 硬件持续捕获，中断更新最近边沿，与主循环读取异步。
     * 每轮开始的 PhaseCapture_Sample() 读取这些捕获值并计算边沿年龄。 */
    if (htim->Instance != TIM2) {
        return;
    }

    switch (htim->Channel) {
        case HAL_TIM_ACTIVE_CHANNEL_1:
            phase_last_capture[0] = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
            phase_capture_valid[0] = 1U;
            break;
        case HAL_TIM_ACTIVE_CHANNEL_2:
            phase_last_capture[1] = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_2);
            phase_capture_valid[1] = 1U;
            break;
        case HAL_TIM_ACTIVE_CHANNEL_3:
            phase_last_capture[2] = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_3);
            phase_capture_valid[2] = 1U;
            break;
        default:
            break;
    }
}

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

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
