#pragma once
#include <stdint.h>

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t error; } I2C_HandleTypeDef;
#define I2C_MEMADD_SIZE_8BIT 1U

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *, uint16_t, uint16_t,
                                   uint16_t, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *, uint16_t, uint16_t,
                                  uint16_t, uint8_t *, uint16_t, uint32_t);
uint32_t HAL_I2C_GetError(I2C_HandleTypeDef *);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t);
