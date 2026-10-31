#include "tim.h"
#include "sensor_error.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "i2c.h"
#include "csv_writer.h"
#include "binary_writer.h"
#include "sensor_ak09973d.h"
#include "tca9548.h"

uint32_t g_ak_bus_read_us[2];
uint32_t g_ak_parallel_read_us;
extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c2;

static const struct {
    uint8_t i2c_bus;
    uint8_t mask;
    uint8_t addr;
} g_config[] = {
    AK09973D_STATIC_CONFIG
};

// 有效传感器列表
static struct {
    uint8_t i2c_bus;
    uint8_t mask;
    ak09973d_t dev;
} g_valid[AK09973D_COUNT];

static I2C_HandleTypeDef* get_i2c(uint8_t bus)
{
    return (bus == 1) ? &hi2c1 : &hi2c2;
}

static const char *hal_status_name(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK:
        return "OK";
    case HAL_ERROR:
        return "ERROR";
    case HAL_BUSY:
        return "BUSY";
    case HAL_TIMEOUT:
        return "TIMEOUT";
    default:
        return "?";
    }
}

static void print_i2c_diag(const char *tag, I2C_HandleTypeDef *hi2c)
{
    uint32_t err = HAL_I2C_GetError(hi2c);
    HAL_I2C_StateTypeDef state = HAL_I2C_GetState(hi2c);
    GPIO_PinState scl = GPIO_PIN_SET;
    GPIO_PinState sda = GPIO_PIN_SET;

    if (hi2c == &hi2c1) {
        scl = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8);
        sda = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9);
    } else if (hi2c == &hi2c2) {
        scl = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_10);
        sda = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_11);
    }

    printf("%s state=0x%02X err=0x%08lX SCL=%d SDA=%d\r\n",
           tag,
           (unsigned int)state,
           (unsigned long)err,
           (int)scl,
           (int)sda);
}

static void i2c1_gpio_bus_clear(void)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_I2C_DeInit(&hi2c1);

    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8 | GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(1);

    for (int i = 0; i < 18 && HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET; i++) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
        HAL_Delay(1);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
        HAL_Delay(1);
    }

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(1);

    MX_I2C1_Init();
    HAL_Delay(5);
}

static void ak_bus_reset(uint8_t bus)
{
    I2C_HandleTypeDef *hi2c = get_i2c(bus);
    GPIO_TypeDef *reset_port = (bus == 1) ? I2C1_RESET_GPIO_Port : I2C2_RESET_GPIO_Port;
    uint16_t reset_pin = (bus == 1) ? I2C1_RESET_Pin : I2C2_RESET_Pin;

    printf("[AK] Reset I2C%d...\r\n", bus);

    HAL_I2C_DeInit(hi2c);

    HAL_GPIO_WritePin(reset_port, reset_pin, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(reset_port, reset_pin, GPIO_PIN_SET);
    HAL_Delay(100);

    if (bus == 1) {
        MX_I2C1_Init();
    } else {
        MX_I2C2_Init();
    }

    HAL_Delay(20);
    print_i2c_diag((bus == 1) ? "[AK] I2C1 after reset" : "[AK] I2C2 after reset", hi2c);
}

static HAL_StatusTypeDef ak_select_channel(uint8_t bus, uint8_t mask)
{
    I2C_HandleTypeDef *hi2c = get_i2c(bus);
    HAL_StatusTypeDef status = TCA9548_Select(hi2c, AK09973D_TCA_ADDR_7B, mask);

    if (0) {
        if (bus == 1) {
            i2c1_gpio_bus_clear();
        }
        ak_bus_reset(bus);
        status = TCA9548_Select(hi2c, AK09973D_TCA_ADDR_7B, mask);
    }

    if (0 && status == HAL_OK && bus == 1 && HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET) {
        printf("[AK] I2C1 CH0x%02X holds SDA low, bus-clear retry\r\n", mask);
        i2c1_gpio_bus_clear();
        ak_bus_reset(1);
        status = TCA9548_Select(&hi2c1, AK09973D_TCA_ADDR_7B, mask);
    }

    return status;
}

static int csv_append_ak_line(char *out, size_t out_size, size_t *off,
                              uint8_t bus, uint8_t mask,
                              const ak09973d_magdata_t *data)
{
    return CSV_AppendString(out, out_size, off, "AK,") &&
           CSV_AppendU32(out, out_size, off, bus) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendHex8(out, out_size, off, mask) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendI32(out, out_size, off, data->hx) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendI32(out, out_size, off, data->hy) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendI32(out, out_size, off, data->hz) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendU32(out, out_size, off, (data->status & 0x01U) ? 1U : 0U) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendU32(out, out_size, off, (data->status & AK09973D_ST_ERR) ? 1U : 0U) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendU32(out, out_size, off, (data->status & AK09973D_ST_DOR) ? 1U : 0U) &&
           CSV_AppendCRLF(out, out_size, off);
}

static int csv_append_akerr_line(char *out, size_t out_size, size_t *off,
                                 uint8_t bus, uint8_t mask,
                                 const char *kind, uint32_t error)
{
    return CSV_AppendString(out, out_size, off, "AKERR,") &&
           CSV_AppendU32(out, out_size, off, bus) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendHex8(out, out_size, off, mask) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendString(out, out_size, off, kind) &&
           CSV_AppendChar(out, out_size, off, ',') &&
           CSV_AppendHex32(out, out_size, off, error) &&
           CSV_AppendCRLF(out, out_size, off);
}

void Sensor_AK09973D_Init_All(void)
{
    memset(g_valid, 0, sizeof(g_valid));
    ak_bus_reset(1);
    ak_bus_reset(2);

    TCA9548_Select(&hi2c1, AK09973D_TCA_ADDR_7B, 0);
    TCA9548_Select(&hi2c2, AK09973D_TCA_ADDR_7B, 0);

    int count = 0;
    int tca_fail_bus1 = 0, tca_fail_bus2 = 0;
    int dev_fail_bus1 = 0, dev_fail_bus2 = 0;
    int init_bus1 = 0, init_bus2 = 0;

    for (int i = 0; i < AK09973D_COUNT; i++) {
        I2C_HandleTypeDef *hi2c = get_i2c(g_config[i].i2c_bus);
        uint8_t mask = g_config[i].mask;
        uint8_t addr = g_config[i].addr;
        uint8_t company = 0;
        uint8_t device = 0;
        if (g_config[i].i2c_bus == 1) init_bus1++;
        else init_bus2++;

        HAL_StatusTypeDef tca = ak_select_channel(g_config[i].i2c_bus, mask);
        if (tca != HAL_OK) {
            SensorError_Set(SensorError_AK(g_config[i].i2c_bus, mask));
            printf("AKINITERR,%d,0x%02X,TCA,%s,0x%08lX\r\n",
                   g_config[i].i2c_bus, mask, hal_status_name(tca), (unsigned long)HAL_I2C_GetError(hi2c));
            if (g_config[i].i2c_bus == 1) tca_fail_bus1++;
            else tca_fail_bus2++;
            continue;
        }

        HAL_StatusTypeDef probe = AK09973D_Probe(hi2c, addr, &company, &device);
        if (probe != HAL_OK) {
            SensorError_Set(SensorError_AK(g_config[i].i2c_bus, mask));
            printf("AKINITERR,%d,0x%02X,WIA,%02X%02X,0x%08lX\r\n",
                   g_config[i].i2c_bus, mask, company, device, (unsigned long)HAL_I2C_GetError(hi2c));
            if (g_config[i].i2c_bus == 1) dev_fail_bus1++;
            else dev_fail_bus2++;
            continue;
        }

        ak09973d_t dev = {0};
        HAL_StatusTypeDef ak = AK09973D_InitWithConfig(&dev, hi2c, addr, AK09973D_ACTIVE_CNTL2);
        if (ak != HAL_OK) {
            SensorError_Set(SensorError_AK(g_config[i].i2c_bus, mask));
            printf("AKINITERR,%d,0x%02X,INIT,%s,0x%08lX\r\n",
                   g_config[i].i2c_bus, mask, hal_status_name(ak), (unsigned long)HAL_I2C_GetError(hi2c));
            if (g_config[i].i2c_bus == 1) dev_fail_bus1++;
            else dev_fail_bus2++;
            continue;
        }

        g_valid[count].i2c_bus = g_config[i].i2c_bus;
        g_valid[count].mask = mask;
        g_valid[count].dev = dev;
        count++;

        TCA9548_Select(hi2c, AK09973D_TCA_ADDR_7B, 0);
    }

    printf("[AK] Result: %d/%d (I2C1: init=%d tca=%d dev=%d, I2C2: init=%d tca=%d dev=%d)\r\n",
           count, AK09973D_COUNT,
           init_bus1, tca_fail_bus1, dev_fail_bus1,
           init_bus2, tca_fail_bus2, dev_fail_bus2);

    for (int i = 0; i < count; i++) {
        HAL_GPIO_TogglePin(LEDG_GPIO_Port, LEDG_Pin);
        HAL_Delay(30);
    }
    if (count == AK09973D_COUNT) {
        // 全部成功 - 绿灯亮一下
        HAL_GPIO_WritePin(LEDG_GPIO_Port, LEDG_Pin, GPIO_PIN_RESET);
        HAL_Delay(100);
        HAL_GPIO_WritePin(LEDG_GPIO_Port, LEDG_Pin, GPIO_PIN_SET);
    } else if (count == 0) {
        // 全失败 - 红灯亮
        /* Shared fault count controls red LED. */
    }
}

/* Callbacks publish only per-bus completion; all shared results are written here. */
typedef enum { AK_IDLE, AK_SELECT, AK_READ, AK_DONE } ak_state_t;
typedef struct {
    I2C_HandleTypeDef *i2c;
    unsigned bus, index;
    uint8_t mask, rx[7];
    ak_state_t state;
    volatile uint8_t pending, completed;
    volatile uint32_t completed_us, error;
    uint32_t op_start, bus_start, bus_end;
    uint8_t started;
} ak_bus_t;
typedef struct {
    ak09973d_magdata_t data;
    uint32_t completed_us, error;
    uint8_t result; /* 0 not visited, 1 read, 2 select error, 3 read error */
} ak_result_t;
static ak_bus_t g_buses[2];
static ak_result_t g_results[AK09973D_COUNT];

static void ak_callback(I2C_HandleTypeDef *i2c, ak_state_t expected, uint8_t failed)
{
    for (unsigned i = 0; i < 2; ++i) {
        ak_bus_t *b = &g_buses[i];
        if (b->i2c == i2c && b->pending && !b->completed &&
            (failed || b->state == expected)) {
            b->completed_us = __HAL_TIM_GET_COUNTER(&htim2);
            b->error = failed ? HAL_I2C_GetError(i2c) : 0;
            b->completed = failed ? 2U : 1U;
            break;
        }
    }
}
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *i2c) { ak_callback(i2c, AK_SELECT, 0); }
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *i2c) { ak_callback(i2c, AK_READ, 0); }
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *i2c) { ak_callback(i2c, AK_IDLE, 1); }

static void ak_recover_timeout(ak_bus_t *b)
{
    IRQn_Type ev = b->bus == 1 ? I2C1_EV_IRQn : I2C2_EV_IRQn;
    IRQn_Type er = b->bus == 1 ? I2C1_ER_IRQn : I2C2_ER_IRQn;
    HAL_NVIC_DisableIRQ(ev); HAL_NVIC_DisableIRQ(er);
    b->pending = 0;
    HAL_I2C_DeInit(b->i2c);
    if (b->bus == 1) MX_I2C1_Init(); else MX_I2C2_Init();
    HAL_NVIC_ClearPendingIRQ(ev); HAL_NVIC_ClearPendingIRQ(er);
    HAL_NVIC_EnableIRQ(ev); HAL_NVIC_EnableIRQ(er);
}

static void ak_start_op(ak_bus_t *b, ak_state_t state)
{
    HAL_StatusTypeDef status;
    b->state = state;
    b->completed = 0;
    b->error = 0;
    b->op_start = __HAL_TIM_GET_COUNTER(&htim2);
    if (!b->started) { b->started = 1; b->bus_start = b->op_start; }
    b->pending = 1;
    if (state == AK_SELECT) {
        b->mask = g_valid[b->index].mask;
        status = HAL_I2C_Master_Transmit_IT(b->i2c, AK09973D_TCA_ADDR_7B << 1, &b->mask, 1);
    } else {
        status = HAL_I2C_Mem_Read_IT(b->i2c, g_valid[b->index].dev.addr7 << 1,
                                     AK09973D_REG_READ_XYZ, I2C_MEMADD_SIZE_8BIT, b->rx, 7);
    }
    if (status != HAL_OK) {
        b->error = HAL_I2C_GetError(b->i2c);
        b->completed_us = __HAL_TIM_GET_COUNTER(&htim2);
        b->completed = 2;
    }
}

static void ak_service(ak_bus_t *b)
{
    if (b->state == AK_DONE) return;
    if (b->pending) {
        uint32_t now = __HAL_TIM_GET_COUNTER(&htim2);
        uint32_t limit = b->state == AK_SELECT ? 10000U : 100000U;
        if (!b->completed && (uint32_t)(now - b->op_start) >= limit) {
            /* Disable IRQ first and recheck: a completion may have raced the timeout. */
            IRQn_Type ev = b->bus == 1 ? I2C1_EV_IRQn : I2C2_EV_IRQn;
            IRQn_Type er = b->bus == 1 ? I2C1_ER_IRQn : I2C2_ER_IRQn;
            HAL_NVIC_DisableIRQ(ev); HAL_NVIC_DisableIRQ(er);
            if (!b->completed) {
                b->error = HAL_I2C_ERROR_TIMEOUT;
                b->completed_us = now;
                b->completed = 2;
                ak_recover_timeout(b);
                b->pending = 1; /* consume this failure once below */
                b->started = 2; /* stop this bus after the failure */
            } else {
                HAL_NVIC_EnableIRQ(ev); HAL_NVIC_EnableIRQ(er);
            }
        }
        if (!b->completed) return;
        b->pending = 0;
        b->bus_end = b->completed_us;
        ak_result_t *r = &g_results[b->index];
        if (b->completed == 2) {
            r->result = b->state == AK_SELECT ? 2 : 3;
            r->error = b->error;
            SensorError_Failure(SensorError_AK(b->bus, g_valid[b->index].mask));
            ++b->index;
            if (b->started == 2) { b->state = AK_DONE; return; }
        } else if (b->state == AK_SELECT) {
            ak_start_op(b, AK_READ);
            return;
        } else {
            r->data.status = b->rx[0];
            r->data.hz = (int16_t)((b->rx[1] << 8) | b->rx[2]);
            r->data.hy = (int16_t)((b->rx[3] << 8) | b->rx[4]);
            r->data.hx = (int16_t)((b->rx[5] << 8) | b->rx[6]);
            r->completed_us = b->completed_us;
            r->result = 1;
            SensorError_Success(SensorError_AK(b->bus, g_valid[b->index].mask));
            ++b->index;
        }
    }
    while (b->index < AK09973D_COUNT && g_valid[b->index].dev.hi2c != NULL) {
        if (g_valid[b->index].i2c_bus == b->bus &&
            !SensorError_Has(SensorError_AK(b->bus, g_valid[b->index].mask))) {
            ak_start_op(b, AK_SELECT);
            return;
        }
        ++b->index;
    }
    b->state = AK_DONE;
}

static void ak_collect(void)
{
    memset(g_results, 0, sizeof(g_results));
    memset(g_buses, 0, sizeof(g_buses));
    g_buses[0].i2c = &hi2c1; g_buses[0].bus = 1;
    g_buses[1].i2c = &hi2c2; g_buses[1].bus = 2;
    uint32_t start = __HAL_TIM_GET_COUNTER(&htim2);
    ak_service(&g_buses[0]); ak_service(&g_buses[1]);
    while (g_buses[0].state != AK_DONE || g_buses[1].state != AK_DONE) {
        ak_service(&g_buses[0]); ak_service(&g_buses[1]);
    }
    g_ak_parallel_read_us = __HAL_TIM_GET_COUNTER(&htim2) - start;
    for (unsigned i = 0; i < 2; ++i)
        g_ak_bus_read_us[i] = g_buses[i].started ? g_buses[i].bus_end - g_buses[i].bus_start : 0;
}

int Sensor_AK09973D_ReadToCSV(char *out, size_t out_size)
{
    size_t off = 0;
    ak_collect();
    for (unsigned i = 0; i < AK09973D_COUNT; ++i) {
        ak_result_t *r = &g_results[i];
        if (r->result >= 2) {
            if (!csv_append_akerr_line(out, out_size, &off, g_valid[i].i2c_bus,
                                       g_valid[i].mask, r->result == 2 ? "TCA" : "READ", r->error)) break;
        } else if (r->result == 1 && (r->data.status & AK09973D_ST_DRDY)) {
            if (!csv_append_ak_line(out, out_size, &off, g_valid[i].i2c_bus, g_valid[i].mask, &r->data)) break;
        }
    }
    return (int)off;
}

int Sensor_AK09973D_ReadToBinary(uint8_t *out, size_t out_size, uint32_t *seq,
                                 uint32_t *frames, uint32_t *skipped, uint32_t *errors)
{
    size_t off = 0;
    ak_collect();
    for (unsigned i = 0; i < AK09973D_COUNT; ++i) {
        ak_result_t *r = &g_results[i];
        if (r->result >= 2) {
            if (errors) ++*errors;
            (void)BinaryWriter_AppendError(out, out_size, &off, (*seq)++, HAL_GetTick(),
                                           BINARY_ERR_SOURCE_AK, r->result - 1U, r->error);
        } else if (r->result == 1) {
            ak09973d_magdata_t *data = &r->data;
            if (!(data->status & AK09973D_ST_DRDY)) { if (skipped) ++*skipped; continue; }
            uint8_t payload[11]; size_t poff = 0;
            BinaryWriter_PutU8(payload, &poff, g_valid[i].i2c_bus);
            BinaryWriter_PutU8(payload, &poff, g_valid[i].mask);
            BinaryWriter_PutI16(payload, &poff, data->hx);
            BinaryWriter_PutI16(payload, &poff, data->hy);
            BinaryWriter_PutI16(payload, &poff, data->hz);
            BinaryWriter_PutU8(payload, &poff, !!(data->status & AK09973D_ST_DRDY));
            BinaryWriter_PutU8(payload, &poff, !!(data->status & AK09973D_ST_ERR));
            BinaryWriter_PutU8(payload, &poff, !!(data->status & AK09973D_ST_DOR));
            if (BinaryWriter_AppendFrameAt(out, out_size, &off, BINARY_FRAME_TYPE_AK,
                                           (*seq)++, HAL_GetTick(), payload, (uint16_t)poff, r->completed_us)) {
                if (frames) ++*frames;
            } else break;
        }
    }
    return (int)off;
}

void Sensor_AK09973D_ReadAll(void)
{
    char frame[1024];
    int n = Sensor_AK09973D_ReadToCSV(frame, sizeof(frame));
    if (n > 0) {
        USB_Send_String(frame);
    }
}

int Sensor_AK09973D_GetCount(void)
{
    int count = 0;
    for (int i = 0; i < AK09973D_COUNT; i++) {
        if (g_valid[i].dev.hi2c != NULL) {
            count++;
        }
    }
    return count;
}
