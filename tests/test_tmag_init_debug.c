#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tmag3001.h"
#include "tmag_debug.h"

volatile uint8_t g_tmag_init_debug = TMAG_INIT_DEBUG;
static uint8_t registers[256];
static uint32_t tick;
static int mode, boot_writes;

uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t ms) { tick += ms; }
uint32_t HAL_I2C_GetError(I2C_HandleTypeDef *i2c) { return i2c->error; }

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *i2c, uint16_t addr,
    uint16_t reg, uint16_t size, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)addr; (void)size; (void)timeout;
    i2c->error = 0;
    if ((mode == 3 && reg == 2) || (mode == 7 && boot_writes++ == 0)) {
        i2c->error = 4;
        return HAL_ERROR;
    }
    memcpy(registers + reg, data, len);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *i2c, uint16_t addr,
    uint16_t reg, uint16_t size, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)addr; (void)size; (void)timeout;
    i2c->error = 0;
    if ((mode == 1 && reg == 0x0E) || (mode == 4 && reg == 2) ||
        (mode == 5 && reg == 0x18)) {
        i2c->error = mode == 1 ? 4 : mode == 4 ? 0x20 : 1;
        return HAL_ERROR;
    }
    memcpy(data, registers + reg, len);
    if (mode == 2 && reg == 2) data[0] = 0;
    return HAL_OK;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    mode = atoi(argv[1]);
    int debug = atoi(argv[2]);
    if (debug >= 0) g_tmag_init_debug = (uint8_t)debug;
    registers[0x0E] = 0x49;
    registers[0x0F] = 0x54;
    registers[0x18] = mode == 6 ? 0x10 : 0x11;
    I2C_HandleTypeDef i2c = {0};
    tmag3001_t dev = {0};
    HAL_StatusTypeDef result = TMAG3001_Init(&dev, &i2c, 0x34);
    printf("RESULT=%d\n", (int)result);
    return 0;
}
