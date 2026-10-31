#include "sensor_error.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "i2c.h"
#include "csv_writer.h"
#include "binary_writer.h"
#include "sensor_tmag3001.h"
#include "tmag_debug.h"
#include "tca9548.h"

extern I2C_HandleTypeDef hi2c3;
extern void USB_Send_String(char *str);

// DWT microsecond timer
static inline void dwt_init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
static inline uint32_t dwt_us(void) {
    return DWT->CYCCNT / (SystemCoreClock / 1000000U);
}

static void tmag_delay_us(uint32_t us)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000U;
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < (us * cycles_per_us)) {}
}

typedef struct {
    tmag3001_t dev;
    uint8_t inited;
    uint8_t tca_ch_mask;
    uint8_t addr7;
    I2C_HandleTypeDef *i2c;
} TMAG3001_Instance_t;

static TMAG3001_Instance_t g_tmag_list[TMAG3001_TOTAL_NUM];

static const uint8_t g_tmag_addrs[TMAG3001_PER_CHANNEL] = {
    TMAG3001_ADDR_A2_GND,
    TMAG3001_ADDR_A2_3V3,
    TMAG3001_ADDR_A2_SDA
};

static void tmag_hardware_reset(void);

static int tmag_append_line(char *out, size_t out_size, size_t *off,
                            uint8_t tca_ch_mask, uint8_t addr7,
                            const tmag3001_data_t *data)
{
    return CSV_AppendString(out, out_size, off, "TMAG,") &&
           CSV_AppendU32(out, out_size, off, tca_ch_mask) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendHex8(out, out_size, off, addr7) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendI32(out, out_size, off, data->x) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendI32(out, out_size, off, data->y) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendI32(out, out_size, off, data->z) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendU32(out, out_size, off, (data->status & 0x01U) ? 1U : 0U) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendU32(out, out_size, off, (data->status & 0x02U) ? 1U : 0U) &&
           CSV_AppendCRLF(out, out_size, off);
}

static void tmag_i2c_recover(void)
{
    I2C3_BusRecover_Fast();
}

static inline void tmag_i2c3_pins_output_pp(void)
{
    GPIOA->MODER = (GPIOA->MODER & ~(3U << (8U * 2U))) | (1U << (8U * 2U));
    GPIOC->MODER = (GPIOC->MODER & ~(3U << (9U * 2U))) | (1U << (9U * 2U));
    GPIOA->OTYPER &= ~(1U << 8U);
    GPIOC->OTYPER &= ~(1U << 9U);
    GPIOA->PUPDR &= ~(3U << (8U * 2U));
    GPIOC->PUPDR &= ~(3U << (9U * 2U));
    GPIOA->OSPEEDR |= (3U << (8U * 2U));
    GPIOC->OSPEEDR |= (3U << (9U * 2U));
}

static inline void tmag_i2c3_pins_af_od(void)
{
    GPIOA->MODER = (GPIOA->MODER & ~(3U << (8U * 2U))) | (2U << (8U * 2U));
    GPIOC->MODER = (GPIOC->MODER & ~(3U << (9U * 2U))) | (2U << (9U * 2U));
    GPIOA->OTYPER |= (1U << 8U);
    GPIOC->OTYPER |= (1U << 9U);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3U << (8U * 2U))) | (1U << (8U * 2U));
    GPIOC->PUPDR = (GPIOC->PUPDR & ~(3U << (9U * 2U))) | (1U << (9U * 2U));
    GPIOA->OSPEEDR |= (3U << (8U * 2U));
    GPIOC->OSPEEDR |= (3U << (9U * 2U));
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFU << ((8U - 8U) * 4U))) | (4U << ((8U - 8U) * 4U));
    GPIOC->AFR[1] = (GPIOC->AFR[1] & ~(0xFU << ((9U - 8U) * 4U))) | (4U << ((9U - 8U) * 4U));
}

// Send N SCL pulses with TCA channel already selected so the sensor sees them.
// A TMAG3001 read transaction is 7 bytes = 63 bits (56 data + 7 ACK).
// We send 72 pulses (8 bytes × 9 bits) to guarantee clocking out any stuck
// transaction, followed by a STOP condition to reset the sensor state machine.
// Uses TMAG_PULSE_HALF_US half-cycle for the recovery waveform.
#ifndef TMAG_RECOVERY_SCL_PULSES
#define TMAG_RECOVERY_SCL_PULSES  72
#endif
#ifndef TMAG_PULSE_HALF_US
#define TMAG_PULSE_HALF_US 1
#endif
#ifndef TMAG_PULSE_NORMAL
#define TMAG_PULSE_NORMAL 18
#endif
#ifndef TMAG_PULSE_SDA
#define TMAG_PULSE_SDA 36
#endif
#ifndef TMAG_GUARD_US
#define TMAG_GUARD_US 0
#endif
#ifndef TMAG_RESELECT_AFTER_SENSOR_PULSE
#define TMAG_RESELECT_AFTER_SENSOR_PULSE 0
#endif
#ifndef TMAG_SKIP_GND_SENSOR_PULSE
#define TMAG_SKIP_GND_SENSOR_PULSE 1
#endif
#ifndef TMAG_DESELECT_AFTER_CHANNEL
#define TMAG_DESELECT_AFTER_CHANNEL 0
#endif

static void tmag_send_scl_pulses_and_stop(int n_pulses)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000U;
    #define _DUS(us) do { uint32_t _s=DWT->CYCCNT; while((DWT->CYCCNT-_s)<(us)*cycles_per_us){} } while(0)

    CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
    _DUS(TMAG_PULSE_HALF_US);
    while (READ_BIT(hi2c3.Instance->CR1, I2C_CR1_PE)) {}

    tmag_i2c3_pins_output_pp();

    GPIOA->BSRR = GPIO_PIN_8;
    GPIOC->BSRR = GPIO_PIN_9;
    _DUS(TMAG_PULSE_HALF_US);

    // N SCL pulses at ~100kHz — sensor sees them because TCA channel is selected
    for (int i = 0; i < n_pulses; i++) {
        GPIOA->BSRR = (uint32_t)GPIO_PIN_8 << 16U; _DUS(TMAG_PULSE_HALF_US);
        GPIOA->BSRR = GPIO_PIN_8;                  _DUS(TMAG_PULSE_HALF_US);
    }
    // STOP: SDA low→high while SCL high
    GPIOC->BSRR = (uint32_t)GPIO_PIN_9 << 16U; _DUS(TMAG_PULSE_HALF_US);
    GPIOC->BSRR = GPIO_PIN_9;                  _DUS(TMAG_PULSE_HALF_US);

    tmag_i2c3_pins_af_od();

    SET_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
    _DUS(20);

    if (__HAL_I2C_GET_FLAG(&hi2c3, I2C_FLAG_BUSY)) {
        SET_BIT(hi2c3.Instance->CR1, I2C_CR1_ANFOFF);
        _DUS(10);
        CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
        _DUS(10);
        while (READ_BIT(hi2c3.Instance->CR1, I2C_CR1_PE)) {}
        CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_ANFOFF);
        SET_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
        _DUS(20);
    }

    #undef _DUS
}

// Pre-read recovery: select TCA channel, then send 72 SCL pulses to clock out
// any stuck transaction from the previous cycle.
static void tmag_recover_sensor_on_channel(uint8_t tca_ch_mask)
{
    tmag_send_scl_pulses_and_stop(TMAG_RECOVERY_SCL_PULSES);
    TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, tca_ch_mask);
}

// Post-read cleanup: send 72 SCL pulses while TCA channel is still selected,
// so sensors are left in a clean state before TCA deselect.
static void tmag_cleanup_channel(uint8_t tca_ch_mask)
{
    (void)tca_ch_mask;  // channel already selected by caller
    tmag_send_scl_pulses_and_stop(TMAG_RECOVERY_SCL_PULSES);
}

// Fast BUSY clear: STM32H7 errata — analog filter can set false BUSY after STOP.
// Also sends 9 SCL pulses to release any sensor holding SDA low.
// ~200us total. Only call when BUSY is actually set.
static void tmag_clear_false_busy(void)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000U;
    #define _DUSB(us) do { uint32_t _s=DWT->CYCCNT; while((DWT->CYCCNT-_s)<(us)*cycles_per_us){} } while(0)

    CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
    _DUSB(5);
    while (READ_BIT(hi2c3.Instance->CR1, I2C_CR1_PE)) {}

    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_8; HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_9; HAL_GPIO_Init(GPIOC, &g);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
    _DUSB(5);

    // 9 SCL pulses to release any sensor holding SDA low
    for (int i = 0; i < 9; i++) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET); _DUSB(5);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);   _DUSB(5);
    }
    // STOP: SDA low→high while SCL high
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_RESET); _DUSB(5);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);   _DUSB(5);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_9);
    g.Mode = GPIO_MODE_AF_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF4_I2C3;
    g.Pin = GPIO_PIN_8; HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_9; HAL_GPIO_Init(GPIOC, &g);

    SET_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
    _DUSB(20);

    // ANFOFF errata fix if still BUSY
    for (int _a = 0; _a < 3; _a++) {
        if (!__HAL_I2C_GET_FLAG(&hi2c3, I2C_FLAG_BUSY)) break;
        SET_BIT(hi2c3.Instance->CR1, I2C_CR1_ANFOFF);
        _DUSB(5);
        CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
        _DUSB(5);
        while (READ_BIT(hi2c3.Instance->CR1, I2C_CR1_PE)) {}
        CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_ANFOFF);
        SET_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
        _DUSB(20);
    }

    #undef _DUSB
}

static int tmag_read_channel_to_csv(uint8_t tca_ch_mask, char *out, size_t out_size, size_t *off)
{
    int sensor_count = 0;
    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        TMAG3001_Instance_t *inst = &g_tmag_list[i];
        if (inst->inited && !SensorError_Has(SensorError_TMAG(inst->tca_ch_mask, inst->addr7)) && inst->tca_ch_mask == tca_ch_mask) {
            sensor_count++;
        }
    }

    if (sensor_count == 0) return 1;

    // Pre-TCA: recover main bus if stuck (fixes TCA itself being stuck)
    if (__HAL_I2C_GET_FLAG(&hi2c3, I2C_FLAG_BUSY)) {
        tmag_i2c_recover();
    }

    if (TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, tca_ch_mask) != HAL_OK) {
        for(unsigned a=0x34;a<=0x36;a++) SensorError_Failure(SensorError_TMAG(tca_ch_mask,a));
        return 1;
    }

    // Channel-level recovery: send 72 SCL pulses after TCA select to clock out
    // any stuck sensor state from the previous cycle. This is the primary recovery
    // for the first sensor on each channel.
    #if TMAG_RECOVERY_SCL_PULSES > 0
    tmag_recover_sensor_on_channel(tca_ch_mask);
#endif

    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        TMAG3001_Instance_t *inst = &g_tmag_list[i];
        if (!inst->inited || SensorError_Has(SensorError_TMAG(inst->tca_ch_mask, inst->addr7))) continue;
        if (inst->tca_ch_mask != tca_ch_mask) continue;

        // Per-sensor recovery: send SCL pulses + STOP + TCA reselect before each read.
        // 0x36 (A2=SDA) needs more pulses — its address pin is on SDA so it's more
        // likely to be mid-transaction after a TCA switch. Use 36 pulses for 0x36,
        // 18 pulses for other addresses.
        {
            int n_pulses = (inst->addr7 == TMAG3001_ADDR_A2_SDA) ? TMAG_PULSE_SDA : TMAG_PULSE_NORMAL;
#if TMAG_SKIP_GND_SENSOR_PULSE
            if (inst->addr7 == TMAG3001_ADDR_A2_GND) {
                n_pulses = 0;
            }
#endif
            if (n_pulses > 0) {
                tmag_send_scl_pulses_and_stop(n_pulses);
#if TMAG_RESELECT_AFTER_SENSOR_PULSE
                TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, tca_ch_mask);
#endif
            }
        }

        tmag3001_data_t data;
        HAL_StatusTypeDef read_status = HAL_ERROR;
        for (int retry = 0; retry < 1; retry++) {
            read_status = TMAG3001_ReadData(&inst->dev, &data);
            if (read_status == HAL_OK) break;
            SensorError_Failure(SensorError_TMAG(inst->tca_ch_mask, inst->addr7));
            break;
            // Full bus recovery + TCA reselect
            tmag_i2c_recover();
            TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, tca_ch_mask);
            // Channel-aware recovery if sensor still stuck after TCA reselect
            if (__HAL_I2C_GET_FLAG(&hi2c3, I2C_FLAG_BUSY)) {
                tmag_recover_sensor_on_channel(tca_ch_mask);
            }
        }

        if (read_status != HAL_OK) continue;

        SensorError_Success(SensorError_TMAG(inst->tca_ch_mask, inst->addr7));
        if (!tmag_append_line(out, out_size, off, tca_ch_mask, inst->addr7, &data)) {
            TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, 0);
            return 0;
        }

        #if TMAG_GUARD_US > 0
        tmag_delay_us(TMAG_GUARD_US);
#endif
    }

    // Post-read cleanup removed: per-sensor recovery at the start of each read
    // handles bus cleanup, so no cleanup needed after the last sensor.

#if TMAG_DESELECT_AFTER_CHANNEL
    TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, 0);
#endif
    return 1;
}

static int tmag_read_channel_to_binary(uint8_t tca_ch_mask, uint8_t *out,
                                       size_t out_size, size_t *off,
                                       uint32_t *seq, uint32_t *frames,
                                       uint32_t *skipped, uint32_t *errors)
{
    int sensor_count = 0;
    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        TMAG3001_Instance_t *inst = &g_tmag_list[i];
        if (inst->inited && !SensorError_Has(SensorError_TMAG(inst->tca_ch_mask, inst->addr7)) && inst->tca_ch_mask == tca_ch_mask) {
            sensor_count++;
        }
    }

    if (sensor_count == 0) return 1;

    if (__HAL_I2C_GET_FLAG(&hi2c3, I2C_FLAG_BUSY)) {
        tmag_i2c_recover();
    }

    if (TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, tca_ch_mask) != HAL_OK) {
        for(unsigned a=0x34;a<=0x36;a++) SensorError_Failure(SensorError_TMAG(tca_ch_mask,a));
        if (errors != NULL) (*errors)++;
        (void)BinaryWriter_AppendError(out, out_size, off,
                                       (*seq)++, HAL_GetTick(),
                                       BINARY_ERR_SOURCE_TMAG, 1U,
                                       HAL_I2C_GetError(&hi2c3));
        return 1;
    }

#if TMAG_RECOVERY_SCL_PULSES > 0
    tmag_recover_sensor_on_channel(tca_ch_mask);
#endif

    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        TMAG3001_Instance_t *inst = &g_tmag_list[i];
        if (!inst->inited || SensorError_Has(SensorError_TMAG(inst->tca_ch_mask, inst->addr7))) continue;
        if (inst->tca_ch_mask != tca_ch_mask) continue;

        int n_pulses = (inst->addr7 == TMAG3001_ADDR_A2_SDA) ? TMAG_PULSE_SDA : TMAG_PULSE_NORMAL;
#if TMAG_SKIP_GND_SENSOR_PULSE
        if (inst->addr7 == TMAG3001_ADDR_A2_GND) {
            n_pulses = 0;
        }
#endif
        if (n_pulses > 0) {
            tmag_send_scl_pulses_and_stop(n_pulses);
#if TMAG_RESELECT_AFTER_SENSOR_PULSE
            TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, tca_ch_mask);
#endif
        }

        tmag3001_data_t data;
        HAL_StatusTypeDef read_status = HAL_ERROR;
        for (int retry = 0; retry < 1; retry++) {
            read_status = TMAG3001_ReadData(&inst->dev, &data);
            if (read_status == HAL_OK) break;
            SensorError_Failure(SensorError_TMAG(inst->tca_ch_mask, inst->addr7));
            break;
            tmag_i2c_recover();
            TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, tca_ch_mask);
            if (__HAL_I2C_GET_FLAG(&hi2c3, I2C_FLAG_BUSY)) {
                tmag_recover_sensor_on_channel(tca_ch_mask);
            }
        }

        if (read_status != HAL_OK) {
            if (errors != NULL) (*errors)++;
            (void)BinaryWriter_AppendError(out, out_size, off,
                                           (*seq)++, HAL_GetTick(),
                                           BINARY_ERR_SOURCE_TMAG, 2U,
                                           HAL_I2C_GetError(&hi2c3));
            continue;
        }

        SensorError_Success(SensorError_TMAG(inst->tca_ch_mask, inst->addr7));
        if ((data.status & TMAG3001_STATUS_DRDY) == 0U) {
            if (skipped != NULL) (*skipped)++;
            continue;
        }

        uint8_t payload[11];
        size_t poff = 0;
        BinaryWriter_PutU8(payload, &poff, tca_ch_mask);
        BinaryWriter_PutU8(payload, &poff, inst->addr7);
        BinaryWriter_PutI16(payload, &poff, data.x);
        BinaryWriter_PutI16(payload, &poff, data.y);
        BinaryWriter_PutI16(payload, &poff, data.z);
        BinaryWriter_PutU8(payload, &poff, (data.status & TMAG3001_STATUS_DRDY) ? 1U : 0U);
        BinaryWriter_PutU8(payload, &poff, (data.status & TMAG3001_STATUS_OVF) ? 1U : 0U);

        if (BinaryWriter_AppendFrame(out, out_size, off, BINARY_FRAME_TYPE_TMAG,
                                     (*seq)++, HAL_GetTick(),
                                     payload, (uint16_t)poff)) {
            if (frames != NULL) (*frames)++;
        } else {
            return 0;
        }

#if TMAG_GUARD_US > 0
        tmag_delay_us(TMAG_GUARD_US);
#endif
    }

#if TMAG_DESELECT_AFTER_CHANNEL
    TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, 0);
#endif
    return 1;
}

// TMAG硬件复位：通过I2C3_RESET线复位TCA Mux和所有TMAG传感器
static void tmag_hardware_reset(void)
{
    TMAG_INIT_LOG("[TMAG INIT] RESET I2C3/TCA PC8 low=300ms high=300ms\r\n");
    // Force-reset I2C3 peripheral before DeInit to avoid hanging on stuck bus
    __HAL_RCC_I2C3_FORCE_RESET();
    HAL_Delay(5);
    __HAL_RCC_I2C3_RELEASE_RESET();
    HAL_Delay(2);
    HAL_I2C_DeInit(&hi2c3);

    HAL_GPIO_WritePin(I2C3_RESET_GPIO_Port, I2C3_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(300);

    HAL_GPIO_WritePin(I2C3_RESET_GPIO_Port, I2C3_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(300);

    extern void MX_I2C3_Init(void);
    MX_I2C3_Init();

    HAL_Delay(100);
}

void Sensor_TMAG3001_Init_All(void)
{
    int idx = 0;

    memset(g_tmag_list, 0, sizeof(g_tmag_list));
    dwt_init();

    printf("TMAG: init start\r\n");
    fflush(stdout);

    tmag_hardware_reset();

    TMAG_INIT_LOG("[TMAG INIT] bus=I2C3 timing=0x%08lX kernel_hz=%lu SCL=%d SDA=%d mask=0x%02X\r\n",
                  (unsigned long)hi2c3.Instance->TIMINGR,
                  (unsigned long)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_I2C3),
                  (int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8),
                  (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_9),
                  (unsigned int)TMAG3001_ACTIVE_TCA_MASK);

    // Test TCA
    HAL_StatusTypeDef tca_status = TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, 0);
    uint32_t tca_error = HAL_I2C_GetError(&hi2c3);
    TMAG_INIT_LOG("[TMAG INIT] TCA addr=0x%02X mask=0x00 hal=%d err=0x%08lX\r\n",
                  (unsigned int)TMAG3001_TCA_ADDR_7B, (int)tca_status,
                  (unsigned long)tca_error);
    if (tca_status != HAL_OK) {
        g_sensor_errormap = 0x0FFFU;
        printf("TMAG: TCA not responding - abort\r\n");
        fflush(stdout);
        return;
    }
    printf("TMAG: TCA OK\r\n");
    fflush(stdout);

    // Direct init: CH1, CH2, CH3, CH4 (normal order)
    uint8_t ch_order[] = {1, 2, 3, 4};
    for (int ci = 0; ci < 4 && idx < TMAG3001_TOTAL_NUM; ci++) {
        uint8_t ch = ch_order[ci];
        uint8_t mask = 1 << ch;

        if ((TMAG3001_ACTIVE_TCA_MASK & mask) == 0U) continue;

        int ch_start_idx = idx;
        int ch_ok = 0;
        for (int ch_attempt = 0; ch_attempt < 1 && ch_ok == 0; ch_attempt++) {
            idx = ch_start_idx;

        // Select TCA channel directly (no prior deselect needed)
        tca_status = TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, mask);
        tca_error = HAL_I2C_GetError(&hi2c3);
        TMAG_INIT_LOG("[TMAG INIT] CH%u mask=0x%02X select try=%d hal=%d err=0x%08lX SCL=%d SDA=%d\r\n",
                      (unsigned int)ch, (unsigned int)mask, ch_attempt + 1,
                      (int)tca_status, (unsigned long)tca_error,
                      (int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8),
                      (int)HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_9));
        if (tca_status != HAL_OK) {
            for(unsigned a=0x34;a<=0x36;a++) SensorError_Set(SensorError_TMAG(mask,a));
            printf("TMAG CH%d: TCA select FAIL\r\n", ch);
            fflush(stdout);
            continue;
        }
        HAL_Delay(20);

        for (uint8_t sub = 0; sub < TMAG3001_PER_CHANNEL && idx < TMAG3001_TOTAL_NUM; sub++) {
            TMAG3001_Instance_t *inst = &g_tmag_list[idx];
            uint8_t addr = g_tmag_addrs[sub];

            TMAG_INIT_LOG("[TMAG INIT] CH%u addr=0x%02X BEGIN\r\n",
                          (unsigned int)ch, (unsigned int)addr);
            HAL_StatusTypeDef init_status = TMAG3001_Init(&inst->dev, &hi2c3, addr);
            uint32_t init_error = HAL_I2C_GetError(&hi2c3);
            TMAG_INIT_LOG("[TMAG INIT] CH%u addr=0x%02X RESULT %s hal=%d err=0x%08lX\r\n",
                          (unsigned int)ch, (unsigned int)addr,
                          init_status == HAL_OK ? "OK" : "FAIL", (int)init_status,
                          (unsigned long)init_error);
            if (init_status == HAL_OK) {
                inst->tca_ch_mask = mask;
                inst->addr7 = addr;
                inst->i2c = &hi2c3;
                inst->inited = 1;
                ch_ok++;
            } else {
                SensorError_Set(SensorError_TMAG(mask, addr));

            }
            idx++;
            HAL_Delay(20);
        }

        if (0) {
            TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, 0);
            HAL_Delay(10);
            tmag_hardware_reset();
        }
        }

        printf("TMAG CH%d: %d/%d OK\r\n", ch, ch_ok, TMAG3001_PER_CHANNEL);
        fflush(stdout);
        TCA9548_Select(&hi2c3, TMAG3001_TCA_ADDR_7B, 0);
        HAL_Delay(10);
    }

    // Count first-pass results
    int inited = 0;
    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        if (g_tmag_list[i].inited) inited++;
    }

    printf("TMAG: first pass %d/%d OK\r\n", inited, TMAG3001_TOTAL_NUM);
    fflush(stdout);

    printf("TMAG: init done\r\n");
    fflush(stdout);
}

int Sensor_TMAG3001_ReadToCSV(uint8_t tca_ch_mask, char *out_line, size_t out_size)
{
    size_t off = 0;
    (void)tmag_read_channel_to_csv(tca_ch_mask, out_line, out_size, &off);
    return (int)off;
}

void Sensor_TMAG3001_ReadAll(void)
{
    char frame[1024];
    int n = Sensor_TMAG3001_ReadAllToCSV(frame, sizeof(frame));
    if (n > 0) {
        USB_Send_String(frame);
    }
}

int Sensor_TMAG3001_ReadAllToCSV(char *out, size_t out_size)
{
    size_t off = 0;

    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        if (!g_tmag_list[i].inited) continue;

        uint8_t mask = g_tmag_list[i].tca_ch_mask;
        int already_read = 0;
        for (int j = 0; j < i; j++) {
            if (g_tmag_list[j].inited && g_tmag_list[j].tca_ch_mask == mask) {
                already_read = 1;
                break;
            }
        }
        if (already_read) continue;

        if (!tmag_read_channel_to_csv(mask, out, out_size, &off)) {
            break;
        }
    }

    return (int)off;
}

int Sensor_TMAG3001_ReadAllToBinary(uint8_t *out, size_t out_size, uint32_t *seq,
                                    uint32_t *frames, uint32_t *skipped,
                                    uint32_t *errors)
{
    size_t off = 0;

    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        if (!g_tmag_list[i].inited) continue;

        uint8_t mask = g_tmag_list[i].tca_ch_mask;
        int already_read = 0;
        for (int j = 0; j < i; j++) {
            if (g_tmag_list[j].inited && g_tmag_list[j].tca_ch_mask == mask) {
                already_read = 1;
                break;
            }
        }
        if (already_read) continue;

        if (!tmag_read_channel_to_binary(mask, out, out_size, &off,
                                         seq, frames, skipped, errors)) {
            break;
        }
    }

    return (int)off;
}

int Sensor_TMAG3001_GetCount(void)
{
    int count = 0;
    for (int i = 0; i < TMAG3001_TOTAL_NUM; i++) {
        if (g_tmag_list[i].inited) {
            count++;
        }
    }
    return count;
}
