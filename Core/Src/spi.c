/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    spi.c
  * @brief   This file provides code for the configuration
  *          of the SPI instances.
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
/* Includes ------------------------------------------------------------------*/
#include "spi.h"

/* USER CODE BEGIN 0 */

/* ---- SPI1 速率档位 ---------------------------------------------------
 * F103 的 SPI1 挂在 PCLK2 = 72 MHz 上，分频档位**只有 2 的幂次**
 * （硬件 CR1.BR[2:0] 就给到 /2 /4 /8 /16 /32 /64 /128 /256，没有 /6 这种
 *  中间档 —— 12 MHz 是拿不到的，HAL 里也没有 SPI_BAUDRATEPRESCALER_6
 *  这个宏，写上去会直接编译不过）：
 *
 *   PRESCALER_16 → 4.5 MHz（更保守）
 *   PRESCALER_8  → 9 MHz  （CubeMX 默认，也是之前调屏时稳定跑通的档）
 *   PRESCALER_4  → 18 MHz （F103 主模式上限）
 *
 * 为什么值得提：屏幕没有 framebuffer，每次刷新都是实打实往外灌像素。
 * 9 MHz 时一整屏要 128 ms，肉眼能看出"从上往下扫"的过程；18 MHz 直接砍半。
 * 差分的意义是让平时只写变化的那几列，提速的意义是让"该整屏刷的时候"
 * （换页、开机自检）不拖泥带水 —— 两件事互补，缺一个都还是会觉得卡。
 *
 * ⚠ 18 MHz 对杜邦线是一次压力测试，而且严格说超规格：ST7789 各版本手册
 *   给的最小串行时钟周期不一致，有标 66 ns（≈15 MHz）的。实践中大多能跑，
 *   但如果出现花屏、像素错位、或者偶发"某一行整体偏移"，先别怀疑驱动 ——
 *   把这个值改回 8 确认是不是信号完整性问题，再改回 16 试更低的档。
 *
 * 放在 USER CODE 区而不是改上面那行 Init：生成区会被 CubeMX 重新覆盖。
 * -------------------------------------------------------------------- */
#define SPI1_PRESCALER_FIX  4    /* 4 = 18 MHz / 8 = 9 MHz / 16 = 4.5 MHz */

/* USER CODE END 0 */

SPI_HandleTypeDef hspi1;

/* SPI1 init function */
void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* 按文件开头选的档位再初始化一次。没必要去改上面生成区那行 BaudRatePrescaler
   * —— HAL_SPI_Init 会按新的 Init 结构把 CR1 整个重写，代价只有一次函数调用。 */
#if (SPI1_PRESCALER_FIX == 4)
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;   /* 18 MHz */
#elif (SPI1_PRESCALER_FIX == 16)
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;  /* 4.5 MHz */
#else
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;   /* 9 MHz（CubeMX 默认） */
#endif
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE END SPI1_Init 2 */

}

void HAL_SPI_MspInit(SPI_HandleTypeDef* spiHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(spiHandle->Instance==SPI1)
  {
  /* USER CODE BEGIN SPI1_MspInit 0 */

  /* USER CODE END SPI1_MspInit 0 */
    /* SPI1 clock enable */
    __HAL_RCC_SPI1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**SPI1 GPIO Configuration
    PA5     ------> SPI1_SCK
    PA7     ------> SPI1_MOSI
    */
    GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN SPI1_MspInit 1 */

  /* USER CODE END SPI1_MspInit 1 */
  }
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef* spiHandle)
{

  if(spiHandle->Instance==SPI1)
  {
  /* USER CODE BEGIN SPI1_MspDeInit 0 */

  /* USER CODE END SPI1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SPI1_CLK_DISABLE();

    /**SPI1 GPIO Configuration
    PA5     ------> SPI1_SCK
    PA7     ------> SPI1_MOSI
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_5|GPIO_PIN_7);

  /* USER CODE BEGIN SPI1_MspDeInit 1 */

  /* USER CODE END SPI1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

