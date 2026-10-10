/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

extern uint32_t g_device_id;

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
void USB_Send_String(char *str);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define key_Pin GPIO_PIN_13
#define key_GPIO_Port GPIOC
#define LEDR_Pin GPIO_PIN_0
#define LEDR_GPIO_Port GPIOC
#define LEDG_Pin GPIO_PIN_1
#define LEDG_GPIO_Port GPIOC
#define LEDB_Pin GPIO_PIN_2
#define LEDB_GPIO_Port GPIOC
#define SPI1_CS_Pin GPIO_PIN_0
#define SPI1_CS_GPIO_Port GPIOB
#define I2C2_RESET_Pin GPIO_PIN_12
#define I2C2_RESET_GPIO_Port GPIOB
#define I2C3_RESET_Pin GPIO_PIN_8
#define I2C3_RESET_GPIO_Port GPIOC
#define I2C1_RESET_Pin GPIO_PIN_7
#define I2C1_RESET_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
