#include "tmag3001.h"
#include "tmag_debug.h"

// TMAG3001 standard register access uses a write of the register pointer
// followed by a repeated START read. The device auto-increments the pointer.
#define TMAG_WRITE_TIMEOUT_MS  5
#define TMAG_READ_TIMEOUT_MS   5
#define TMAG_READY_TIMEOUT_MS  5
#define TMAG_PROBE_RETRIES     15

static HAL_StatusTypeDef write_reg(tmag3001_t *dev, uint8_t reg, uint8_t val)
{
    return HAL_I2C_Mem_Write(dev->hi2c, dev->addr7 << 1, reg,
                             I2C_MEMADD_SIZE_8BIT, &val, 1, TMAG_WRITE_TIMEOUT_MS);
}

static HAL_StatusTypeDef read_reg(tmag3001_t *dev, uint8_t reg, uint8_t *val)
{
    return HAL_I2C_Mem_Read(dev->hi2c, dev->addr7 << 1, reg,
                            I2C_MEMADD_SIZE_8BIT, val, 1, TMAG_READ_TIMEOUT_MS);
}

static HAL_StatusTypeDef read_regs(tmag3001_t *dev, uint8_t start_reg, uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(dev->hi2c, dev->addr7 << 1, start_reg,
                            I2C_MEMADD_SIZE_8BIT, buf, len, TMAG_READ_TIMEOUT_MS);
}

static HAL_StatusTypeDef read_mfg_id(tmag3001_t *dev, uint16_t *out_mfg)
{
    uint8_t mfg_data[2];
    HAL_StatusTypeDef s = read_regs(dev, TMAG3001_REG_MFG_LSB, mfg_data, sizeof(mfg_data));
    if (s != HAL_OK) {
        return s;
    }

    *out_mfg = ((uint16_t)mfg_data[1] << 8) | mfg_data[0];
    return HAL_OK;
}

static HAL_StatusTypeDef read_mfg_id_retry(tmag3001_t *dev, uint16_t *out_mfg)
{
    HAL_StatusTypeDef last = HAL_ERROR;

    for (int i = 0; i < TMAG_PROBE_RETRIES; i++) {
        last = read_mfg_id(dev, out_mfg);
        uint32_t error = HAL_I2C_GetError(dev->hi2c);
        TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X MFG try=%d hal=%d err=0x%08lX id=0x%04X\r\n",
                      (unsigned int)dev->addr7, i + 1, (int)last,
                      (unsigned long)error,
                      (unsigned int)(last == HAL_OK ? *out_mfg : 0xFFFFU));
        if (last == HAL_OK && *out_mfg == 0x5449) {
            return HAL_OK;
        }
        HAL_Delay(20);
    }

    return last;
}

static HAL_StatusTypeDef write_checked(tmag3001_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t rb = 0;
    HAL_StatusTypeDef s = write_reg(dev, reg, val);
    uint32_t error = HAL_I2C_GetError(dev->hi2c);
    if (s != HAL_OK) {
        TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X WRITE FAIL reg=0x%02X wr=0x%02X hal=%d err=0x%08lX\r\n",
                      (unsigned int)dev->addr7, (unsigned int)reg,
                      (unsigned int)val, (int)s, (unsigned long)error);
        return s;
    }

    HAL_Delay(1);
    s = read_reg(dev, reg, &rb);
    error = HAL_I2C_GetError(dev->hi2c);
    if (s != HAL_OK) {
        TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X READBACK FAIL reg=0x%02X wr=0x%02X hal=%d err=0x%08lX\r\n",
                      (unsigned int)dev->addr7, (unsigned int)reg,
                      (unsigned int)val, (int)s, (unsigned long)error);
        return s;
    }

    TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X VERIFY %s reg=0x%02X wr=0x%02X rd=0x%02X\r\n",
                  (unsigned int)dev->addr7, rb == val ? "OK" : "FAIL",
                  (unsigned int)reg, (unsigned int)val, (unsigned int)rb);
    return (rb == val) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef wait_result_ready(tmag3001_t *dev)
{
    uint32_t start = HAL_GetTick();
    uint8_t status = 0;

    do {
        HAL_StatusTypeDef s = read_reg(dev, TMAG3001_REG_CONV_STATUS, &status);
        uint32_t error = HAL_I2C_GetError(dev->hi2c);
        if (s != HAL_OK) {
            TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X READY READ FAIL hal=%d err=0x%08lX\r\n",
                          (unsigned int)dev->addr7, (int)s, (unsigned long)error);
            return s;
        }
        if ((status & TMAG3001_STATUS_DRDY) != 0) {
            TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X READY OK status=0x%02X\r\n",
                          (unsigned int)dev->addr7, (unsigned int)status);
            return HAL_OK;
        }
        HAL_Delay(1);
    } while ((HAL_GetTick() - start) < TMAG_READY_TIMEOUT_MS);

    TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X READY TIMEOUT status=0x%02X timeout_ms=%u\r\n",
                  (unsigned int)dev->addr7, (unsigned int)status,
                  (unsigned int)TMAG_READY_TIMEOUT_MS);
    return HAL_TIMEOUT;
}

HAL_StatusTypeDef TMAG3001_Probe(I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
    tmag3001_t dev = {
        .hi2c = hi2c,
        .addr7 = addr7
    };
    uint16_t mfg_id;

    if (read_mfg_id_retry(&dev, &mfg_id) != HAL_OK) {
        return HAL_ERROR;
    }

    return (mfg_id == 0x5449) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef TMAG3001_Init(tmag3001_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
    dev->hi2c = hi2c;
    dev->addr7 = addr7;

    HAL_StatusTypeDef boot_status = write_reg(dev, TMAG3001_REG_DEV_CFG1, TMAG3001_DEV_CFG1_DEFAULT);
    uint32_t boot_error = HAL_I2C_GetError(hi2c);
    TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X BOOT WRITE reg=0x00 wr=0x00 hal=%d err=0x%08lX\r\n",
                  (unsigned int)addr7, (int)boot_status, (unsigned long)boot_error);
    HAL_Delay(2);
    boot_status = write_reg(dev, TMAG3001_REG_DEV_CFG2, TMAG3001_DEV_CFG2_STANDBY);
    boot_error = HAL_I2C_GetError(hi2c);
    TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X BOOT WRITE reg=0x01 wr=0x00 hal=%d err=0x%08lX\r\n",
                  (unsigned int)addr7, (int)boot_status, (unsigned long)boot_error);
    HAL_Delay(2);

    uint16_t mfg_id;
    if (read_mfg_id_retry(dev, &mfg_id) != HAL_OK || mfg_id != 0x5449) {
        TMAG_INIT_LOG("[TMAG INIT] addr=0x%02X ABORT MFG\r\n", (unsigned int)addr7);
        return HAL_ERROR;
    }

    // Configure only while the device is in standby; enter continuous mode last.
    if (write_checked(dev, TMAG3001_REG_DEV_CFG2, TMAG3001_DEV_CFG2_STANDBY) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_DEV_CFG1, TMAG3001_DEV_CFG1_DEFAULT) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_SENS_CFG1, TMAG3001_SENS_CFG1_XYZ_EN) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_SENS_CFG2, TMAG3001_SENS_CFG2_RAW_DEFAULT) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_THR_CFG1, 0x00) != HAL_OK ||
        write_checked(dev, TMAG3001_REG_THR_CFG2, 0x00) != HAL_OK ||
        write_checked(dev, TMAG3001_REG_THR_CFG3, 0x00) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_SENS_CFG3, TMAG3001_SENS_CFG3_DEFAULT) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_INT_CFG1, TMAG3001_INT_CFG1_DISABLED) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_SENS_CFG4, 0x00) != HAL_OK ||
        write_checked(dev, TMAG3001_REG_SENS_CFG5, 0x00) != HAL_OK ||
        write_checked(dev, TMAG3001_REG_SENS_CFG6, 0x00) != HAL_OK) {
        return HAL_ERROR;
    }

    if (write_checked(dev, TMAG3001_REG_DEV_CFG2, TMAG3001_DEV_CFG2_CONTINUOUS) != HAL_OK) {
        return HAL_ERROR;
    }

    if (wait_result_ready(dev) != HAL_OK) {
        return HAL_ERROR;
    }

    return HAL_OK;
}

HAL_StatusTypeDef TMAG3001_ReadData(tmag3001_t *dev, tmag3001_data_t *out)
{
    // Read 7 bytes: X_MSB, X_LSB, Y_MSB, Y_LSB, Z_MSB, Z_LSB, Conv_Status
    uint8_t buf[7];
    if (read_regs(dev, TMAG3001_REG_X_MSB, buf, sizeof(buf)) != HAL_OK)
        return HAL_ERROR;

    out->x = (int16_t)((buf[0] << 8) | buf[1]);
    out->y = (int16_t)((buf[2] << 8) | buf[3]);
    out->z = (int16_t)((buf[4] << 8) | buf[5]);
    out->status = buf[6];

    return HAL_OK;
}

uint8_t TMAG3001_IsDataReady(tmag3001_t *dev)
{
    uint8_t status;
    if (read_reg(dev, TMAG3001_REG_CONV_STATUS, &status) == HAL_OK)
        return (status & TMAG3001_STATUS_DRDY) ? 1 : 0;
    return 0;
}

HAL_StatusTypeDef TMAG3001_ReadConfig(tmag3001_t *dev, uint8_t *buf, uint16_t len)
{
    if (len == 0) {
        return HAL_OK;
    }

    return read_regs(dev, TMAG3001_REG_DEV_CFG1, buf, len);
}

HAL_StatusTypeDef TMAG3001_SetAddress(tmag3001_t *dev, uint8_t new_addr7)
{
    uint8_t val = (new_addr7 << 1) | 0x01;
    return write_reg(dev, TMAG3001_REG_I2C_ADDR, val);
}
