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
#include "spi.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lcd.h"
#include "rtc_time.h"
#include "keys.h"
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
/* ↓↓↓ 诊断用：给"调度器启动前"这段打表（看门狗已在计时，这段必须短）↓↓↓ */
volatile uint32_t g_scan_ms  = 0;   /* 总线体检(i2c_selftest) 耗时 ms */
volatile uint32_t g_init_ms  = 0;   /* 显示初始化(lcd_init) 耗时 ms */
volatile uint32_t g_boot_ms  = 0;   /* main 从复位到"进调度器前"的总耗时 ms */
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
  MX_SPI1_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  /* MX_IWDG_Init(); ← 故意不在这里调用：看门狗一通电就开始数秒、中途没法暂停，
     而下面的启动体检最坏要跑好几秒，"开早了"的结果就是上电即复位的无限重启。
     已挪到本函数末尾 USER CODE BEGIN 2 里，见那里的说明。
     ⚠ 用 CubeMX 重新生成代码会把这一行加回来，届时记得再删掉一次。 */
  /* USER CODE BEGIN 2 */
  /* ---- 三个按键：PB5 / PB6 的 EXTI 配置 -------------------------------
   * 放在 USER CODE 区而不是 gpio.c：那边是 CubeMX 生成区，重新生成会丢。
   * PA4 已经在 MX_GPIO_Init() 里配好了，这里只补新增的两个。
   * ------------------------------------------------------------------ */
  keys_init();

  /* ---- 备份域：跨复位保留的自留地 ------------------------------------
   * ① 三个前提缺一不可，少一个就是"静默失败"（写不进去还不报错）
   * ② DR1 = 复位计数；DR10 = 签名；DR3/DR4 留给"死亡档案"（谁死的+停滞秒数）
   * ③ 备份寄存器实际只有低 16 位可用（头文件虽声明成 uint32_t）
   * ------------------------------------------------------------------ */
  __HAL_RCC_PWR_CLK_ENABLE();          /* 前提 1：PWR 时钟 */
  __HAL_RCC_BKP_CLK_ENABLE();          /* 前提 2：BKP 时钟 */
  HAL_PWR_EnableBkUpAccess();          /* 前提 3：解除备份域写保护（DBP 位） */

  /* ---- 时间子系统：★ 必须放在所有 BKP 读写之前 -------------------------
   * 原因：rtc_time_init() 在"备份域已经丢了、需要重新配置 RTC"时会执行
   * BDRST（备份域复位），而 BKP->DR1~DR10 就住在那个域里 —— 放后面会把
   * 复位计数、死亡档案、boot loop 计数统统抹掉。
   * 反过来放这里就没事：它只在真断电（那些数据本来也已经丢了）或者 RTC
   * 时钟源发生变化时才复位；软复位（看门狗复位是常态）时备份域还活着、
   * RTC 也还在走，它什么都不动。
   * ------------------------------------------------------------------ */
  rtc_time_init();

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
   * g_i2c_addr2，本项目预期扫到 0x68 = MPU6050）。放在调度器启动前是白送的
   * —— 此刻没有任务，扫描最坏阻塞约 1 秒也不影响任何心跳。*/
  i2c_selftest();
  g_scan_ms = HAL_GetTick();           /* 诊断：进调度器前已耗时（体检含在内） */

  g_boot_ms = HAL_GetTick();           /* 诊断：main 到"进调度器前"的总耗时 */

  /* ---- 看门狗：等"系统准备好被监督"这一刻才武装 -------------------------
   * ① HAL_IWDG_Init() 一执行，IWDG 立刻开始倒数，装完就算、中途无法暂停；
   * ② 第一次喂狗要等 wdg 任务跑完 osDelay(1000)，所以留给
   *    "复位 → 调度器起来 + 1 s"的预算只有约 3 秒（IWDG 超时约 4 s）；
   * ③ 上面的启动体检最坏能吃掉好几秒（I2C 卡死时每个地址要白等 25 ms 的
   *    固定 BUSY 超时，112 个地址就是 2.8 s），一超就是"上电即复位"；
   * ④ 而且那次复位发生在调度器启动之前，wdg 任务永远跑不到，
   *    g_boot_loop_guard 那套 boot loop 防护也救不回来 —— 直接死循环。
   * 所以：启动逻辑全部跑完，再打开看门狗。
   * -------------------------------------------------------------------- */
  /* ---- 屏幕上电自检：黑 → 白 → 红 → 绿 → 蓝，各 0.25 秒 ----------------
   * 留着这段不是为了好看，它是个"不用调试器就能判断屏还活着"的探针：
   * 显示链路任何一环断了（背光、接触不良、SPI、初始化序列），上电一眼可见。
   * 而这套东西的 8 根杜邦线确实接触不良过一次，代价是查了小半天。
   *
   * 顺序也不是随便排的，"黑"放第一个是判读需要：
   *   ST7789 上电未经初始化时屏本身就是白的（GRAM 内容不定 + IPS 面板）；
   *   初始化完了但一个像素都没写进去、GRAM 全 0x0000 再被 INVON 反显，
   *   同样也是白的 —— 所以"看到白色"区分不了这两种情况，什么也证明不了。
   *   只有真的刷黑了，才说明 命令 → 窗口 → 像素 这条通路是通的。
   *
   * 放在 MX_IWDG_Init() 之前：这段阻塞正好落在"看门狗还没武装"的窗口里。
   * 别让看门狗和长阻塞共存于同一时间窗（09-26 的教训）。
   *
   * 发送次数 = 9(lcd_init) + 5 × 245(每次刷屏) = 1234，对照 g_spi_tx_ok 用。
   * -------------------------------------------------------------------- */
  uint32_t t_lcd = HAL_GetTick();
  lcd_init();
  g_init_ms = HAL_GetTick() - t_lcd;   /* 诊断：显示初始化耗时（含约 380 ms 的各种等待） */

  lcd_clear(LCD_BLACK); HAL_Delay(250);
  lcd_clear(LCD_WHITE); HAL_Delay(250);
  lcd_clear(LCD_RED);   HAL_Delay(250);
  lcd_clear(LCD_GREEN); HAL_Delay(250);
  lcd_clear(LCD_BLUE);  HAL_Delay(250);
  lcd_clear(LCD_BLACK);                /* 收在黑底：接下来 ui 任务直接在它上面画 */

  MX_IWDG_Init();
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();


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
