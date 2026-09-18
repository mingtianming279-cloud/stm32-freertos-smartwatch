/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "cmsis_os.h"
#include "i2c.h"
#include "iwdg.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ssd1306.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 备份寄存器"签名"：只有读到它，才认为 DR1~DR9 里的数据是程序认真写过的 */
#define BKP_MAGIC   0x5A5Bu
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile uint32_t g_bkp_dr1 = 0;      /* 备份寄存器 DR1 的镜像（调试用） */
volatile uint32_t g_rcc_csr = 0;      /* 复位原因快照（RCC->CSR 的镜像，调试用） */
volatile uint32_t g_last_dead_task = 0;   /* 上次复位前死掉的第几个任务（1 起；0=没死过） */
volatile uint32_t g_last_dead_secs = 0;   /* 它连续停滞了几秒 */
volatile uint32_t g_boot_loop_cnt   = 0;   /* 连续"没活够"的次数 */
volatile uint32_t g_boot_loop_guard = 0;   /* 1 = 判定疑似 boot loop（本步只观测） */
volatile uint32_t g_alive_sec          = 0;   /* 本次启动已经活了多少秒 */
volatile uint32_t g_boot_loop_healthy  = 0;   /* 1 = 已经上报"活够了" */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  MX_IWDG_Init();
  /* USER CODE BEGIN 2 */
  /* ---- 备份域：跨复位保留的自留地 ------------------------------------
   * ① 三个前提缺一不可，少一个就是"静默失败"（写不进去还不报错）
   * ② DR1 = 复位计数；DR10 = 签名；DR3/DR4 留给"死亡档案"（谁死的+停滞秒数）
   * ③ 备份寄存器实际只有低 16 位可用（头文件虽声明成 uint32_t）
   * ------------------------------------------------------------------ */
  __HAL_RCC_PWR_CLK_ENABLE();          /* 前提 1：PWR 时钟 */
  __HAL_RCC_BKP_CLK_ENABLE();          /* 前提 2：BKP 时钟 */
  HAL_PWR_EnableBkUpAccess();          /* 前提 3：解除备份域写保护（DBP 位） */

  if (BKP->DR10 != BKP_MAGIC)          /* 没读到签名 → 这份数据不可信 */
  {
    BKP->DR1  = 0;                     /* （首次上电 / 被清过）计数从 0 开始 */
    BKP->DR2 = 0;                      /* 连续次数一并作废（否则"改签名"重置不掉防护状态） */
    BKP->DR3  = 0;                     /* 死亡档案一并作废（0 = 无记录） */
    BKP->DR4  = 0;
    BKP->DR10 = BKP_MAGIC;             /* 打上签名：从此刻起数据有效 */
  }

  BKP->DR1++;                          /* 复位次数 +1 */
  g_bkp_dr1 = BKP->DR1;                /* 镜像到普通变量，供现场表达式观察 */
  g_last_dead_task = BKP->DR3;         /* 读上次的死亡档案：第几个任务（0 = 没死过） */
  g_last_dead_secs = BKP->DR4;         /*                       它停滞了几秒 */

  /* ---- 复位原因：硬件自动记录的"黑匣子" -------------------------------
   * bit27 PORRSTF  上电/掉电复位  ← 真正断过电才会有
   * bit29 IWDGRSTF 独立看门狗复位 ← 看门狗干的
   * bit26 PINRSTF  引脚复位（NRST 键 / 调试器复位）
   * bit28 SFTRSTF  软件复位；bit30 WWDGRSTF 窗口看门狗；bit31 LPWRRSTF 低功耗
   * 标志位是"粘滞"的，读一次就清一次，下次复位才判得准
   * ------------------------------------------------------------------ */
  g_rcc_csr = RCC->CSR;                /* 先快照（调试器求值不了 RCC，因为它也是宏） */
  RCC->CSR |= RCC_CSR_RMVF;            /* 再清标志（写 1 清），为下次复位做准备 */

  if (BKP->DR2 == BKP_HEALTHY)
  {
	  g_boot_loop_cnt = 1;
  }
  else
  {
	  g_boot_loop_cnt = BKP -> DR2 + 1;
  }
  BKP -> DR2 = (uint16_t)g_boot_loop_cnt;
  g_boot_loop_guard = (g_boot_loop_cnt >= BOOT_LOOP_MAX) ? 1u : 0u;

  /* 开机体检：I2C 总线上都有谁（结果看 g_i2c_found / g_i2c_addr1 /
   * g_ssd1306_online）。放在调度器启动前是白送的 —— 此刻没有任务，
   * 扫描最坏阻塞约 1 秒也不影响任何心跳。*/
  ssd1306_selftest();
  if (g_ssd1306_online)
    {
        ssd1306_init();
        ssd1306_fill(0xFFu);      /* 全白：肉眼确认"通了" */
    }
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
