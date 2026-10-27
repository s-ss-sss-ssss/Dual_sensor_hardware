#include "ak09973d.h"
#include "firmware_debug.h"

#define AK09973D_IO_TIMEOUT_MS      100U
#define AK09973D_RESET_DELAY_MS     5U
#define AK09973D_FIRST_SAMPLE_MS    120U
#define AK09973D_DRDY_POLL_MS       2U

static HAL_StatusTypeDef write_reg(ak09973d_t *dev, uint8_t reg, uint8_t val)
{
    HAL_StatusTypeDef result = HAL_I2C_Mem_Write(dev->hi2c, dev->addr7 << 1, reg,
                             I2C_MEMADD_SIZE_8BIT, &val, 1,
                             AK09973D_IO_TIMEOUT_MS);
    AK_INIT_LOG("[AK INIT] addr=0x%02X WRITE reg=0x%02X val=0x%02X hal=%d err=0x%08lX\r\n",
                dev->addr7, reg, val, (int)result,
                (unsigned long)HAL_I2C_GetError(dev->hi2c));
    return result;
}

static HAL_StatusTypeDef read_reg(ak09973d_t *dev, uint8_t reg, uint8_t *val)
{
    return HAL_I2C_Mem_Read(dev->hi2c, dev->addr7 << 1, reg,
                            I2C_MEMADD_SIZE_8BIT, val, 1,
                            AK09973D_IO_TIMEOUT_MS);
}

static HAL_StatusTypeDef read_regs(ak09973d_t *dev, uint8_t reg, uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(dev->hi2c, dev->addr7 << 1, reg,
                            I2C_MEMADD_SIZE_8BIT, buf, len,
                            AK09973D_IO_TIMEOUT_MS);
}

static HAL_StatusTypeDef write_regs(ak09973d_t *dev, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef result = HAL_I2C_Mem_Write(dev->hi2c, dev->addr7 << 1, reg,
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)buf, len,
                             AK09973D_IO_TIMEOUT_MS);
    AK_INIT_LOG("[AK INIT] addr=0x%02X WRITE reg=0x%02X len=%u hal=%d err=0x%08lX\r\n",
                dev->addr7, reg, len, (int)result,
                (unsigned long)HAL_I2C_GetError(dev->hi2c));
    return result;
}

static HAL_StatusTypeDef wait_data_ready(ak09973d_t *dev, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t st = 0;
    HAL_StatusTypeDef last_read = HAL_OK;
    uint32_t last_error = 0;

    do {
        last_read = read_reg(dev, AK09973D_REG_ST, &st);
        last_error = HAL_I2C_GetError(dev->hi2c);
        if (last_read != HAL_OK) {
            AK_INIT_LOG("[AK INIT] addr=0x%02X READY READ FAIL hal=%d err=0x%08lX\r\n",
                        dev->addr7, (int)last_read, (unsigned long)last_error);
        }
        if (last_read == HAL_OK && (st & AK09973D_ST_DRDY) != 0U) {
            AK_INIT_LOG("[AK INIT] addr=0x%02X READY OK status=0x%02X\r\n", dev->addr7, st);
            return HAL_OK;
        }
        HAL_Delay(AK09973D_DRDY_POLL_MS);
    } while ((HAL_GetTick() - start) < timeout_ms);

    AK_INIT_LOG("[AK INIT] addr=0x%02X READY TIMEOUT status=0x%02X last_hal=%d err=0x%08lX\r\n",
                dev->addr7, st, (int)last_read, (unsigned long)last_error);
    (void)last_error;
    return HAL_TIMEOUT;
}

HAL_StatusTypeDef AK09973D_Probe(I2C_HandleTypeDef *hi2c, uint8_t addr7, uint8_t *company, uint8_t *device)
{
    ak09973d_t dev = {
        .hi2c = hi2c,
        .addr7 = addr7
    };
    uint8_t wia[4] = {0};

    HAL_StatusTypeDef result = read_regs(&dev, AK09973D_REG_WIA, wia, sizeof(wia));
    AK_INIT_LOG("[AK INIT] addr=0x%02X WIA hal=%d err=0x%08lX id=0x%02X%02X valid=%u\r\n",
                addr7, (int)result, (unsigned long)HAL_I2C_GetError(hi2c),
                wia[0], wia[1], result == HAL_OK);
    if (result != HAL_OK) {
        return HAL_ERROR;
    }
    if (wia[0] != AK09973D_WIA_COMPANY || wia[1] != AK09973D_WIA_DEVICE) {
        AK_INIT_LOG("[AK INIT] addr=0x%02X ID MISMATCH expected=0x48C1\r\n", addr7);
    }

    if (company != NULL) {
        *company = wia[0];
    }
    if (device != NULL) {
        *device = wia[1];
    }

    return (wia[0] == AK09973D_WIA_COMPANY && wia[1] == AK09973D_WIA_DEVICE) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef AK09973D_SoftReset(ak09973d_t *dev)
{
    if (write_reg(dev, AK09973D_REG_SRST, 0x01) != HAL_OK) {
        return HAL_ERROR;
    }

    HAL_Delay(AK09973D_RESET_DELAY_MS);
    return HAL_OK;
}

HAL_StatusTypeDef AK09973D_SetMode(ak09973d_t *dev, uint8_t cntl2)
{
    return write_reg(dev, AK09973D_REG_CNTL2, cntl2);
}

HAL_StatusTypeDef AK09973D_ReadConfig(ak09973d_t *dev, ak09973d_config_t *cfg)
{
    uint8_t cntl[2];

    if (cfg == NULL) {
        return HAL_ERROR;
    }

    if (read_regs(dev, AK09973D_REG_CNTL1, cntl, sizeof(cntl)) != HAL_OK) {
        return HAL_ERROR;
    }
    if (read_reg(dev, AK09973D_REG_CNTL2, &cfg->cntl2) != HAL_OK) {
        return HAL_ERROR;
    }

    cfg->cntl1_h = cntl[0];
    cfg->cntl1_l = cntl[1];
    return HAL_OK;
}

HAL_StatusTypeDef AK09973D_Init(ak09973d_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
    return AK09973D_InitWithConfig(dev, hi2c, addr7, AK09973D_MODE_10HZ);
}

HAL_StatusTypeDef AK09973D_InitWithConfig(ak09973d_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7, uint8_t cntl2)
{
    dev->hi2c = hi2c;
    dev->addr7 = addr7;

    if (AK09973D_SoftReset(dev) != HAL_OK) {
        return HAL_ERROR;
    }

    if (AK09973D_Probe(hi2c, addr7, NULL, NULL) != HAL_OK) {
        return HAL_ERROR;
    }

    const uint8_t cntl1[2] = {0x00, 0x00};
    if (write_regs(dev, AK09973D_REG_CNTL1, cntl1, sizeof(cntl1)) != HAL_OK) {
        return HAL_ERROR;
    }

    if (AK09973D_SetMode(dev, cntl2) != HAL_OK) {
        return HAL_ERROR;
    }

    return wait_data_ready(dev, AK09973D_FIRST_SAMPLE_MS);
}

HAL_StatusTypeDef AK09973D_ReadMagData(ak09973d_t *dev, ak09973d_magdata_t *out)
{
    uint8_t buf[7];
    if (read_regs(dev, AK09973D_REG_READ_XYZ, buf, 7) != HAL_OK)
        return HAL_ERROR;

    out->status = buf[0];
    out->hz = (int16_t)((buf[1] << 8) | buf[2]);
    out->hy = (int16_t)((buf[3] << 8) | buf[4]);
    out->hx = (int16_t)((buf[5] << 8) | buf[6]);

    return HAL_OK;
}

uint8_t AK09973D_IsDataReady(ak09973d_t *dev)
{
    uint8_t st;
    if (read_reg(dev, AK09973D_REG_ST, &st) == HAL_OK)
        return (st & AK09973D_ST_DRDY) ? 1 : 0;
    return 0;
}
