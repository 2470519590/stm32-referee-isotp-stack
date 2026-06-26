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
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define TIM2_CH2_Servo2_Pin GPIO_PIN_1
#define TIM2_CH2_Servo2_GPIO_Port GPIOA
#define USART2_TX_Pin GPIO_PIN_2
#define USART2_TX_GPIO_Port GPIOA
#define TIM2_CH1_Servo1_Pin GPIO_PIN_5
#define TIM2_CH1_Servo1_GPIO_Port GPIOA
#define TIM1_CH2N_HeatPower_Pin GPIO_PIN_0
#define TIM1_CH2N_HeatPower_GPIO_Port GPIOB
#define TIM2_CH3_Servo3_Pin GPIO_PIN_10
#define TIM2_CH3_Servo3_GPIO_Port GPIOB
#define TIM2_CH4_Servo4_Pin GPIO_PIN_11
#define TIM2_CH4_Servo4_GPIO_Port GPIOB
#define TIM15_CH1_LEDctrl_Pin GPIO_PIN_14
#define TIM15_CH1_LEDctrl_GPIO_Port GPIOB
#define TIM1_CH1_Servo5_Pin GPIO_PIN_8
#define TIM1_CH1_Servo5_GPIO_Port GPIOA
#define TIM16_CH1_BUZZER_Pin GPIO_PIN_8
#define TIM16_CH1_BUZZER_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
