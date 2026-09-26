/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    i2c.c
  * @brief   This file provides code for the configuration
  *          of the I2C instances.
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
#include "i2c.h"

/* USER CODE BEGIN 0 */

/* ── STM32F1 I2C1 "BUSY 假置位" 恢复（实现在本文件末尾 USER CODE BEGIN 1）──
 * 已确认的故障现场（Debug 实测）：
 *     SR2 = 0x0002 → BUSY = 1 而 MSL = 0，同时 SCL/SDA 实测为高（总线其实空闲）
 *     g_mpu_st = 2 → HAL_BUSY：每次 HAL_I2C_xxx 都在第一步的 BUSY 等待上白等满
 *                    25 ms（I2C_TIMEOUT_BUSY_FLAG 是 HAL 里写死的值）才返回
 * 后果远不止"通信失败"：algo 被拖到 80 ms 一圈、ui 到 500 ms 一圈，
 * 启动体检的 112 个地址本来要 2.8 s —— 那正是"上电即复位"的元凶。
 *
 * 恢复必须分级，而且顺序很重要（上一版把 ①③ 搞反了，所以一次都没救回来）：
 *   ① RCC 强制复位 I2C1：外设连寄存器带状态机一起清。BUSY 是状态机锁存出来的，
 *      只关/开 PE 它不会消失 —— 这是唯一真正管用的一招。
 *   ② CR1.SWRST 软件复位：兜底，万一 ① 没生效。
 *   ③ SCL/SDA 转普通开漏 GPIO 敲满 9 个时钟 + 一个 STOP：这一级治的是
 *      "从机卡在中途把线按住不放"（真 BUSY），对锁存型的假 BUSY 无效，
 *      所以先看一眼两条线，都是高就跳过，别白敲 20 ms。
 * 不卡则零开销。
 * ------------------------------------------------------------------------ */

/* ── 恢复过程的可观测性：挂到现场表达式上，一眼看出 BUSY 到底为什么清不掉 ──
 * g_i2c_rec_step —— 恢复停在哪一步：
 *   0 = BUSY 没置位，无需恢复（正常情况，绝大多数启动都走这里）
 *   1 = RCC 强制复位 I2C1 就解决了
 *   2 = CR1.SWRST 软件复位解决了
 *   3 = 敲满 9 个时钟 + STOP 解决了
 *   7 = ①② 都无效、而且 SCL/SDA 两条线都是"高"（理论上不该出现，出现就是大新闻）
 *   8 = ①② 都无效、线是"低"的；敲满 9 个时钟后 BUSY 依旧
 *   9 = HAL_I2C_Init 返回了非 HAL_OK
 * g_i2c_scl_lvl / g_i2c_sda_lvl —— 进入恢复时读到的两条线电平（1 = 高）。
 *   F1 手册里 SR2.BUSY 的置位条件只有一条 ——"SDA 或 SCL 为低"——
 *   而外设已经被 RCC 整个复位过、PE 重新使能后 BUSY 又立刻变 1，
 *   所以这两个数就是最终判据：只要有 0，BUSY 就是被真实的低电平顶起来的，
 *   跟"锁存假置位"无关 —— 那是硬件问题，代码再怎么改都没用。
 * ------------------------------------------------------------------------ */
volatile uint32_t g_i2c_scl_lvl = 0xFFFFFFFFu;
volatile uint32_t g_i2c_sda_lvl = 0xFFFFFFFFu;
volatile uint32_t g_i2c_scl_ext = 0xFFFFFFFFu;
volatile uint32_t g_i2c_sda_ext = 0xFFFFFFFFu;
volatile uint32_t g_i2c_scl_pd  = 0xFFFFFFFFu;
volatile uint32_t g_i2c_sda_pd  = 0xFFFFFFFFu;
volatile uint32_t g_i2c_rec_step = 0u;

/* ── 总线取证探针（默认关闭）─────────────────────────────────────────────
 * 置 1 = 每次进恢复流程时，把外设从线上摘下来，分别用 MCU 内部上拉/下拉读
 *        一次两条线的电平，写进下面那组变量。当初就是靠它定位到"SCL 一路
 *        根本没有外部上拉"的：下拉探针 0 / 上拉探针 1 = 这条线纯悬空。
 *        它要动引脚、多花约 4 ms；只在总线已经卡死时才会跑（正常启动零开
 *        销），但正常固件用不着，所以默认关掉。下次再遇到"扫不到器件"时
 *        把它打开，一次就能分清是线的问题还是器件的问题。 */
#define I2C_BUS_FORENSICS   0

/* 三条线读数怎么读：
 *   _lvl —— 原始读数（PE=1，外设可能自己在驱动引脚，仅供参考）
 *   _ext —— 上拉探针：外设摘下来 + 开 MCU 内部上拉（30~50 kΩ）后读。
 *           读到 1 = 没有东西在"硬按住"这条线（真上拉和纯悬空都会是 1）
 *   _pd  —— 下拉探针：外设摘下来 + 开 MCU 内部下拉后读。
 *           内部下拉只有 30~50 kΩ，只要外面有一颗几 kΩ 的上拉，
 *           分压后还有 3.3 × 40/(40+4.7) ≈ 2.9 V → 仍是 1；
 *           而完全悬空的线会被它拉到底 → 0。
 * 两个探针一对照，这条线到底是"通的/断的/短路的"就定性了：
 *   上拉=1 下拉=1 → 外面有上拉（几 kΩ）      → 线是通的，模块上拉也在 ✔
 *   上拉=1 下拉=0 → 外面什么都没有，纯悬空    → 这条线没接到模块   ← 我猜是这个
 *   上拉=0 下拉=0 → 外面被硬拉低              → 短路到 GND / 从机死拉 / 未上电钳位
 *   上拉=0 下拉=1 → 外面硬接高电平（3.3V）    → 接错到 VCC 了
 * 触发时机：只在 BUSY 已置位、进恢复流程时才测；而且要 I2C_BUS_FORENSICS=1
 * 才会更新。正常启动零开销。 */

/* ── ★ 临时救命档：外部上拉缺失时，用 MCU 内部上拉顶上 ──────────────────────
 * 背景：实测 g_i2c_scl_pd = 0 —— SCL 上压根没有外部上拉，开漏输出只能拉低、
 * 拉不高，时钟沿就出不来，从机全都看不到时钟。而 SCL/SDA 的上拉电阻长在
 * 模块那一侧，所以这多半是接线问题。
 *
 * 打开这个开关：I2C 引脚改成"开漏 + MCU 内部上拉"（30~50 kΩ 顶替模块的
 * 4.7 kΩ），并把速率降到 100 kHz（40 kΩ 的上升沿在 400 kHz 下根本来不及）。
 *
 * 它的作用是"判案"，不一定能长期用：
 *   - 如果烧完 g_i2c_found 变成 2、屏亮了 → SCL 线其实是通的，坏的只是
 *     模块那颗上拉电阻（或它没接上）→ 换模块 / 补一根上拉电阻。
 *   - 如果 g_i2c_scl_pd 还是 0、g_i2c_found 还是 0 → SCL 线确实没通到模块。
 *
 * ⚠ 线路修好之后把它改回 0，恢复"外部上拉 + 400 kHz"的正常配置。 */
#define I2C_USE_INTERNAL_PULLUP   0

/* ── I2C1 引脚：PB8 = SCL / PB9 = SDA（重映射脚）──────────────────────────
 * F103 的 I2C1 有两组引脚：
 *   默认组    PB6 = SCL, PB7 = SDA
 *   重映射组  PB8 = SCL, PB9 = SDA   ← 现在用这组，必须同时打开 AFIO 重映射
 *             （__HAL_AFIO_REMAP_I2C1_ENABLE()，见 HAL_I2C_MspInit）
 * 切到这一组是为了绕开原来的 PB6 一路（怀疑引脚虚焊/损坏 + 老孔位接触不良）。
 * 想切回默认组：下面两个引脚号改回 6/7，删掉 MspInit 里的重映射那行，
 * 并把 .ioc 里的 PB8/PB9 改回 PB6/PB7（否则 CubeMX 重新生成会打架）。 */
#define I2C1_SCL_PIN       GPIO_PIN_8
#define I2C1_SDA_PIN       GPIO_PIN_9

/* USER CODE END 0 */

I2C_HandleTypeDef hi2c1;

/* I2C1 init function */
void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

#if I2C_USE_INTERNAL_PULLUP
  /* 救命档：内部上拉只有 30~50 kΩ，400 kHz 的上升沿爬不起来，临时降到 100 kHz。
   * 特意放在 USER CODE 区而不是改上面那行 ClockSpeed —— 生成区会被 CubeMX
   * 重新生成时覆盖掉，放这儿才留得住。 */
  hi2c1.Init.ClockSpeed = 100000;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
#endif

  /* 上电时 BUSY 可能已经锁死了（实测就是）。这里先恢复一次，否则后面
   * 每一次 HAL_I2C_xxx 都要先白等 25 ms 才返回 HAL_BUSY。 */
  i2c1_recover_bus();

  /* USER CODE END I2C1_Init 2 */

}

void HAL_I2C_MspInit(I2C_HandleTypeDef* i2cHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(i2cHandle->Instance==I2C1)
  {
  /* USER CODE BEGIN I2C1_MspInit 0 */

  /* USER CODE END I2C1_MspInit 0 */

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**I2C1 GPIO Configuration
    PB8     ------> I2C1_SCL
    PB9     ------> I2C1_SDA
    */
    /* ★ I2C1 走重映射脚 PB8/PB9 —— 必须先打开 AFIO 重映射！
     *   不开的话 PB8/PB9 只是两个普通 GPIO，I2C1 仍然留在 PB6/PB7 上，
     *   等于完全没接通（这就是"接了线却一个器件都扫不到"的成因之一）。 */
    __HAL_AFIO_REMAP_I2C1_ENABLE();

    GPIO_InitStruct.Pin = I2C1_SCL_PIN|I2C1_SDA_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
#if I2C_USE_INTERNAL_PULLUP
    /* 救命档：拿 MCU 内部上拉顶替模块那一侧的上拉电阻 */
    GPIO_InitStruct.Pull = GPIO_PULLUP;
#else
    /* 正常配置：I2C 是开漏总线，上拉电阻长在模块那一侧，芯片内部不该给 */
    GPIO_InitStruct.Pull = GPIO_NOPULL;
#endif
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* I2C1 clock enable */
    __HAL_RCC_I2C1_CLK_ENABLE();
  /* USER CODE BEGIN I2C1_MspInit 1 */

  /* USER CODE END I2C1_MspInit 1 */
  }
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef* i2cHandle)
{

  if(i2cHandle->Instance==I2C1)
  {
  /* USER CODE BEGIN I2C1_MspDeInit 0 */

  /* USER CODE END I2C1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_I2C1_CLK_DISABLE();

    /**I2C1 GPIO Configuration
    PB6     ------> I2C1_SCL
    PB7     ------> I2C1_SDA
    */
    HAL_GPIO_DeInit(GPIOB, I2C1_SCL_PIN);

    HAL_GPIO_DeInit(GPIOB, I2C1_SDA_PIN);

  /* USER CODE BEGIN I2C1_MspDeInit 1 */

  /* USER CODE END I2C1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

uint32_t i2c1_recover_bus(void)
{
	GPIO_InitTypeDef gpio = {0};

	/* BUSY 没置位 = 正常，零开销直接返回（绝大多数调用都走这里） */
	if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) == RESET)
	{
		g_i2c_rec_step = 0u;
		return 1u;
	}

#if I2C_BUS_FORENSICS
	/* ── 取一次现场（默认关，见文件头 I2C_BUS_FORENSICS 说明）─────────────
	 * 关键是"把外设从线上摘下来"再读：PE=1 时外设自己可能正驱动着引脚，
	 * 读到 0 属于自证，分不清是外设干的还是外面干的。
	 * 顺序不能反 —— 上拉探针必须放最后，它会把两条线留在高电平上。 */
	g_i2c_scl_lvl = (HAL_GPIO_ReadPin(GPIOB, I2C1_SCL_PIN) == GPIO_PIN_SET) ? 1u : 0u;
	g_i2c_sda_lvl = (HAL_GPIO_ReadPin(GPIOB, I2C1_SDA_PIN) == GPIO_PIN_SET) ? 1u : 0u;

	__HAL_I2C_DISABLE(&hi2c1);              /* 把外设从线上摘下来 */
	gpio.Pin   = I2C1_SCL_PIN | I2C1_SDA_PIN;
	gpio.Mode  = GPIO_MODE_INPUT;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;

	gpio.Pull = GPIO_PULLDOWN;              /* 判"外面到底有没有上拉" */
	HAL_GPIO_Init(GPIOB, &gpio);
	HAL_Delay(2);
	g_i2c_scl_pd = (HAL_GPIO_ReadPin(GPIOB, I2C1_SCL_PIN) == GPIO_PIN_SET) ? 1u : 0u;
	g_i2c_sda_pd = (HAL_GPIO_ReadPin(GPIOB, I2C1_SDA_PIN) == GPIO_PIN_SET) ? 1u : 0u;

	gpio.Pull = GPIO_PULLUP;                /* 判"有没有东西在硬按住线" */
	HAL_GPIO_Init(GPIOB, &gpio);
	HAL_Delay(2);
	g_i2c_scl_ext = (HAL_GPIO_ReadPin(GPIOB, I2C1_SCL_PIN) == GPIO_PIN_SET) ? 1u : 0u;
	g_i2c_sda_ext = (HAL_GPIO_ReadPin(GPIOB, I2C1_SDA_PIN) == GPIO_PIN_SET) ? 1u : 0u;
#else
	/* 动手复位之前，先把两条线"松开"抬回高电平。
	 * BUSY 的置位条件只有一条 ——"检测到 SDA 或 SCL 为低"。线还低着就重新
	 * 使能外设，BUSY 会立刻又锁回去。实测这一步做与不做，恢复成功率直接是
	 * 1 比 8（见 g_i2c_rec_step）。用"输入 + 内部上拉"把线松开最省事：
	 * 不驱动总线，只是让上拉把它带起来。 */
	__HAL_I2C_DISABLE(&hi2c1);
	gpio.Pin   = I2C1_SCL_PIN | I2C1_SDA_PIN;
	gpio.Mode  = GPIO_MODE_INPUT;
	gpio.Pull  = GPIO_PULLUP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);
	HAL_Delay(1);
#endif

	/* 切回 AF_OD 复用，让外设接管引脚。注意别在这之前把线放空 */
	gpio.Pin   = I2C1_SCL_PIN | I2C1_SDA_PIN;
	gpio.Mode  = GPIO_MODE_AF_OD;
	gpio.Pull  = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);

	/* ① RCC 强制复位：外设连寄存器带状态机一起清。
	 *    关键点：BUSY 是"总线状态机"锁存出来的位，不是配置位，
	 *    所以关/开 PE、重写一遍寄存器都没用，必须整个外设复位。 */
	__HAL_RCC_I2C1_FORCE_RESET();
	__HAL_RCC_I2C1_RELEASE_RESET();

	hi2c1.State = HAL_I2C_STATE_RESET;   /* 让 HAL_I2C_Init 重走 MspInit 并清 Lock */
	if (HAL_I2C_Init(&hi2c1) != HAL_OK)
	{
		g_i2c_rec_step = 9u;
		return 0u;
	}
	if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) == RESET)
	{
		g_i2c_rec_step = 1u;
		return 1u;
	}

	/* ② 兜底：CR1.SWRST 软件复位（F1 的 CR1 bit15 确实存在，CMSIS 里是 I2C_CR1_SWRST） */
	SET_BIT(hi2c1.Instance->CR1, I2C_CR1_SWRST);
	CLEAR_BIT(hi2c1.Instance->CR1, I2C_CR1_SWRST);

	hi2c1.State = HAL_I2C_STATE_RESET;
	if (HAL_I2C_Init(&hi2c1) != HAL_OK)
	{
		g_i2c_rec_step = 9u;
		return 0u;
	}
	if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) == RESET)
	{
		g_i2c_rec_step = 2u;
		return 1u;
	}

	/* ③ 最后一级：先放下外设，再看线到底有没有被按住。
	 *    AF_OD 下输入通路是通的，可以直接读 IDR 看真实电平。 */
	__HAL_I2C_DISABLE(&hi2c1);
	if ((HAL_GPIO_ReadPin(GPIOB, I2C1_SCL_PIN) == GPIO_PIN_SET) &&
	    (HAL_GPIO_ReadPin(GPIOB, I2C1_SDA_PIN) == GPIO_PIN_SET))
	{
		/* 两条线都是高 = 总线真的闲，没有从机按着它 → 敲时钟毫无意义。
		 * 而 ①② 已经把外设复位两遍了，BUSY 还在，说明是更顽固的情况。
		 * 别在这里空转 20 ms，如实报失败，让调用方降级（干脆跳过 I2C）。 */
		hi2c1.State = HAL_I2C_STATE_RESET;
		(void)HAL_I2C_Init(&hi2c1);
		g_i2c_rec_step = 7u;
		return 0u;
	}

	/* 真的有线被拉住 → 用普通开漏 GPIO 敲满 9 个时钟把它放掉，再造一个 STOP。
	 * 注意这里不能"一读到 SDA 高就 break"：总线本身空闲时第一次就会退出，
	 * 实际只敲了 1 个时钟 —— 上一版就是这么白跑的。 */
	gpio.Pin   = I2C1_SCL_PIN | I2C1_SDA_PIN;   /* PB6 = SCL, PB7 = SDA */
	gpio.Mode  = GPIO_MODE_OUTPUT_OD;
	gpio.Pull  = GPIO_PULLUP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);
	HAL_GPIO_WritePin(GPIOB, I2C1_SCL_PIN | I2C1_SDA_PIN, GPIO_PIN_SET);

	for (uint8_t i = 0u; i < 9u; i++)
	{
		HAL_GPIO_WritePin(GPIOB, I2C1_SCL_PIN, GPIO_PIN_RESET);
		HAL_Delay(1);
		HAL_GPIO_WritePin(GPIOB, I2C1_SCL_PIN, GPIO_PIN_SET);
		HAL_Delay(1);
	}

	/* 造一个 STOP：SCL 为高时，把 SDA 由低拉高 */
	HAL_GPIO_WritePin(GPIOB, I2C1_SDA_PIN, GPIO_PIN_RESET);
	HAL_Delay(1);
	HAL_GPIO_WritePin(GPIOB, I2C1_SCL_PIN, GPIO_PIN_SET);
	HAL_Delay(1);
	HAL_GPIO_WritePin(GPIOB, I2C1_SDA_PIN, GPIO_PIN_SET);
	HAL_Delay(1);

	/* 恢复 AF 复用，并把外设彻底重新初始化一次 */
	gpio.Mode = GPIO_MODE_AF_OD;
	gpio.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOB, &gpio);

	hi2c1.State = HAL_I2C_STATE_RESET;
	if (HAL_I2C_Init(&hi2c1) != HAL_OK)
	{
		g_i2c_rec_step = 9u;
		return 0u;
	}

	if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) == RESET)
	{
		g_i2c_rec_step = 3u;
		return 1u;
	}

	g_i2c_rec_step = 8u;
	return 0u;
}

/* USER CODE END 1 */

