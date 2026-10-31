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

int Sensor_AK09973D_ReadToCSV(char *out, size_t out_size)
{
    size_t off = 0;

    for (int i = 0; i < AK09973D_COUNT && g_valid[i].dev.hi2c != NULL; i++) {
        unsigned bit = SensorError_AK(g_valid[i].i2c_bus, g_valid[i].mask);
        if (SensorError_Has(bit)) continue;
        I2C_HandleTypeDef *hi2c = get_i2c(g_valid[i].i2c_bus);

        if (ak_select_channel(g_valid[i].i2c_bus, g_valid[i].mask) != HAL_OK) {
            SensorError_Failure(bit);
            if (!csv_append_akerr_line(out, out_size, &off,
                                       g_valid[i].i2c_bus, g_valid[i].mask,
                                       "TCA", HAL_I2C_GetError(hi2c))) {
                break;
            }
            continue;
        }
#if AK09973D_TCA_SETTLE_MS > 0
        HAL_Delay(AK09973D_TCA_SETTLE_MS);
#endif

        ak09973d_magdata_t data;
        if (AK09973D_ReadMagData(&g_valid[i].dev, &data) != HAL_OK) {
            SensorError_Failure(bit);
            if (!csv_append_akerr_line(out, out_size, &off,
                                       g_valid[i].i2c_bus, g_valid[i].mask,
                                       "READ", HAL_I2C_GetError(hi2c))) {
                break;
            }
            continue;
        }

        SensorError_Success(bit);
        if (!csv_append_ak_line(out, out_size, &off,
                                g_valid[i].i2c_bus, g_valid[i].mask, &data)) {
            break;
        }

#if AK09973D_POST_READ_MS > 0
        if (g_valid[i].i2c_bus == 1) {
            HAL_Delay(AK09973D_POST_READ_MS);
        }
#endif
    }

#if AK09973D_DESELECT_AFTER_READ
    TCA9548_Select(&hi2c1, AK09973D_TCA_ADDR_7B, 0);
    TCA9548_Select(&hi2c2, AK09973D_TCA_ADDR_7B, 0);
#endif
    return (int)off;
}

int Sensor_AK09973D_ReadToBinary(uint8_t *out, size_t out_size, uint32_t *seq,
                                 uint32_t *frames, uint32_t *skipped,
                                 uint32_t *errors)
{
    size_t off = 0;
    g_ak_bus_read_us[0] = g_ak_bus_read_us[1] = 0;

    for (int i = 0; i < AK09973D_COUNT && g_valid[i].dev.hi2c != NULL; i++) {
        unsigned bit = SensorError_AK(g_valid[i].i2c_bus, g_valid[i].mask);
        if (SensorError_Has(bit)) continue;
        I2C_HandleTypeDef *hi2c = get_i2c(g_valid[i].i2c_bus);

        uint32_t io_start = __HAL_TIM_GET_COUNTER(&htim2);
        HAL_StatusTypeDef io_status = ak_select_channel(g_valid[i].i2c_bus, g_valid[i].mask);
        g_ak_bus_read_us[g_valid[i].i2c_bus - 1] += (uint32_t)(__HAL_TIM_GET_COUNTER(&htim2) - io_start);
        if (io_status != HAL_OK) {
            SensorError_Failure(bit);
            if (errors != NULL) (*errors)++;
            (void)BinaryWriter_AppendError(out, out_size, &off,
                                           (*seq)++, HAL_GetTick(),
                                           BINARY_ERR_SOURCE_AK, 1U,
                                           HAL_I2C_GetError(hi2c));
            continue;
        }

        ak09973d_magdata_t data;
        io_start = __HAL_TIM_GET_COUNTER(&htim2);
        io_status = AK09973D_ReadMagData(&g_valid[i].dev, &data);
        g_ak_bus_read_us[g_valid[i].i2c_bus - 1] += (uint32_t)(__HAL_TIM_GET_COUNTER(&htim2) - io_start);
        if (io_status != HAL_OK) {
            SensorError_Failure(bit);
            if (errors != NULL) (*errors)++;
            (void)BinaryWriter_AppendError(out, out_size, &off,
                                           (*seq)++, HAL_GetTick(),
                                           BINARY_ERR_SOURCE_AK, 2U,
                                           HAL_I2C_GetError(hi2c));
            continue;
        }

        SensorError_Success(bit);
        if ((data.status & AK09973D_ST_DRDY) == 0U) {
            if (skipped != NULL) (*skipped)++;
            continue;
        }

        uint8_t payload[11];
        size_t poff = 0;
        BinaryWriter_PutU8(payload, &poff, g_valid[i].i2c_bus);
        BinaryWriter_PutU8(payload, &poff, g_valid[i].mask);
        BinaryWriter_PutI16(payload, &poff, data.hx);
        BinaryWriter_PutI16(payload, &poff, data.hy);
        BinaryWriter_PutI16(payload, &poff, data.hz);
        BinaryWriter_PutU8(payload, &poff, (data.status & AK09973D_ST_DRDY) ? 1U : 0U);
        BinaryWriter_PutU8(payload, &poff, (data.status & AK09973D_ST_ERR) ? 1U : 0U);
        BinaryWriter_PutU8(payload, &poff, (data.status & AK09973D_ST_DOR) ? 1U : 0U);

        if (BinaryWriter_AppendFrame(out, out_size, &off, BINARY_FRAME_TYPE_AK,
                                     (*seq)++, HAL_GetTick(),
                                     payload, (uint16_t)poff)) {
            if (frames != NULL) (*frames)++;
        } else {
            break;
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
