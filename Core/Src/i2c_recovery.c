#include "i2c.h"

/*
 * Application-specific I2C3 recovery helpers.
 *
 * Keep this code outside CubeMX-generated i2c.c so code generation cannot
 * remove it.  PA8 is I2C3_SCL and PC9 is I2C3_SDA on this board.
 */

static void i2c3_dwt_delay_us(uint32_t us)
{
    const uint32_t cycles_per_us = SystemCoreClock / 1000000U;
    const uint32_t start = DWT->CYCCNT;

    while ((DWT->CYCCNT - start) < (us * cycles_per_us)) {
    }
}

void I2C3_Reset(void)
{
    HAL_I2C_DeInit(&hi2c3);
    HAL_Delay(5);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_9);

    MX_I2C3_Init();
    HAL_Delay(10);
}

void I2C3_BusRecover(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_I2C3_FORCE_RESET();
    HAL_Delay(10);

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOC, &gpio);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(100);

    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_9) == GPIO_PIN_RESET) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
        HAL_Delay(5);
        for (int i = 0; i < 9; ++i) {
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
            HAL_Delay(5);
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
            HAL_Delay(5);
        }
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
        HAL_Delay(50);
    }

    for (int i = 0; i < 40; ++i) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
        HAL_Delay(5);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
        HAL_Delay(5);
    }

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(100);

    __HAL_RCC_I2C3_RELEASE_RESET();
    HAL_Delay(20);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_9);
    HAL_Delay(20);

    MX_I2C3_Init();
    HAL_Delay(100);
}

void I2C3_BusRecover_Fast(void)
{
    __HAL_I2C_RESET_HANDLE_STATE(&hi2c3);

    __HAL_RCC_I2C3_FORCE_RESET();
    i2c3_dwt_delay_us(100);

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOC, &gpio);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
    i2c3_dwt_delay_us(50);

    for (int i = 0; i < 9; ++i) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
        i2c3_dwt_delay_us(50);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
        i2c3_dwt_delay_us(50);
    }

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_RESET);
    i2c3_dwt_delay_us(50);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
    i2c3_dwt_delay_us(50);

    __HAL_RCC_I2C3_RELEASE_RESET();
    i2c3_dwt_delay_us(100);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_9);
    i2c3_dwt_delay_us(50);

    __HAL_I2C_RESET_HANDLE_STATE(&hi2c3);
    MX_I2C3_Init();
    i2c3_dwt_delay_us(200);

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!__HAL_I2C_GET_FLAG(&hi2c3, I2C_FLAG_BUSY)) {
            break;
        }
        SET_BIT(hi2c3.Instance->CR1, I2C_CR1_ANFOFF);
        i2c3_dwt_delay_us(10);
        CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
        i2c3_dwt_delay_us(10);
        while (READ_BIT(hi2c3.Instance->CR1, I2C_CR1_PE)) {
        }
        CLEAR_BIT(hi2c3.Instance->CR1, I2C_CR1_ANFOFF);
        SET_BIT(hi2c3.Instance->CR1, I2C_CR1_PE);
        i2c3_dwt_delay_us(100);
    }
}
