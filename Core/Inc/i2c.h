/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    i2c.h
  * @brief   This file contains all the function prototypes for
  *          the i2c.c file
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
#ifndef __I2C_H__
#define __I2C_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern I2C_HandleTypeDef hi2c1;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_I2C1_Init(void);

/* USER CODE BEGIN Prototypes */

/* I2C1 总线恢复：清掉 F1 的 SR2.BUSY 假置位 / 把被从机按住的线顶出来。
 * 返回 1 = 恢复后总线可用；0 = 仍然 BUSY（真救不回来）。
 * BUSY 未置位时零开销直接返回 1，运行时可以放心反复调用。 */
uint32_t i2c1_recover_bus(void);

/* ── 总线体检 ──────────────────────────────────────────────────────────
 * 扫一遍 0x08~0x77，结果写进下面三个镜像变量。
 * 在调度器启动前调用最划算 —— 那时还没有任务，最坏阻塞 1 秒也不影响任何心跳。 */
void i2c_selftest(void);

extern volatile uint32_t g_i2c_found;   /* 总线上应答的器件个数 */
extern volatile uint32_t g_i2c_addr1;   /* 第一个应答的 7 位地址 */
extern volatile uint32_t g_i2c_addr2;   /* 第二个应答的 7 位地址 */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __I2C_H__ */

