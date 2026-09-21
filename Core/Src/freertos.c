/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "iwdg.h"
#include "app_events.h"
#include "mpu6050.h"
#include "ssd1306.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct {
	const char *name; /* ── 配置 ── 任务名 */
	volatile uint32_t *hb; /* ── 配置 ── 监控谁 */
	uint32_t limit; /* ── 配置 ── 连续停滞几秒判定卡死 */
	uint32_t last; /* ── 状态 ── 上一秒快照 */
	uint32_t stall_sec; /* ── 状态 ── 连续停滞秒数 */
} hb_mon_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 按键有效电平：PA4 配的是“上升沿 + 无上下拉”，即按下被外部拉到高电平 */
/* 如果板子实际是“按下接地”，改成 GPIO_PIN_RESET，并把 EXTI 触发沿改成下降沿 */
#define KEY_ACTIVE_LEVEL   GPIO_PIN_SET
#define KEY_SCAN_MS        20u    /* 按下后先等这么久再采样 = 消抖窗口 */
#define ALGO_PERIOD_MS     20u    /* algo 节拍，以后挂采样/滤波 */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* ---- 心跳：每个任务各一个，调试器里挂 Live Expressions 看谁死了 ---- */
volatile uint32_t g_hb_algo = 0;
volatile uint32_t g_hb_key = 0;
volatile uint32_t g_hb_ui = 0;
volatile uint32_t g_hb_ble = 0;
volatile uint32_t g_hb_wdg = 0;

/* ---- 队列收发统计：g_evt_sent 应当 ≈ g_evt_recv ---- */
volatile uint32_t g_evt_sent = 0; /* 成功放进队列的次数   */
volatile uint32_t g_evt_recv = 0; /* 成功取出的次数       */
volatile uint32_t g_evt_putfail = 0; /* 队列满、发失败的次数 */
volatile uint32_t g_evt_unknown = 0; /* 收到不认识的事件码   */
volatile uint32_t g_evt_err = 0; /* 其它错误码           */
volatile uint32_t g_evt_timeout = 0; /* M2 遗留：algo 改成非阻塞 drain 后就没用了，M3 重写完可删 */

/* ---- M3 按键链路 4 级流水，卡在哪一级一眼就看出来 ---- */
volatile uint32_t g_key_irq = 0; /* ① EXTI 中断次数（含抖动） */
volatile uint32_t g_key_bounce = 0; /* ② 消抖吃掉的毛刺 / 抖完自己弹回 */
volatile uint32_t g_key_evt = 0; /* ③ 确认成立并投进 algo_q 的按键 */
volatile uint32_t g_algo_key = 0; /* ④ algo 处理掉的按键事件 */
volatile uint8_t  g_mpu_who = 0;   /* 芯片自报身份，期望 0x68 */
volatile uint32_t g_mpu_err = 0;   /* 非 HAL_OK 的次数，期望 0*/
volatile uint32_t g_mpu_st  = 0;   /* 最近一次的 HAL */

 /* ---- ui 侧 ---- */
volatile uint32_t g_ui_evt = 0; /* ui 收到的事件数 */
volatile uint32_t g_ui_refresh = 0; /* ui 真正刷了一下的次数（M3 用 LED 顶替屏幕） */
volatile uint32_t g_ui_timeout = 0; /* ui 超时醒来的次数（正常） */
volatile uint32_t g_ui_put = 0; /* key 成功投进 ui_q 的次数 */
volatile uint32_t g_ui_putfail = 0; /* ui_q 满、投失败的次数   */
volatile uint32_t g_dead_secs = 0; /* 有过"被判死任务"的秒数 */
#ifdef DEBUG
volatile uint32_t g_force_dead = 0; /* 调试钩子：置 1 假装有任务卡死（仅 Debug 构建存在） */
#endif
volatile uint32_t g_wdg_degraded = 0; /* 带病喂狗秒数：dead > 0 但因 guard 被豁免的次数 */
volatile int16_t g_ax = 0;
volatile int16_t g_ay = 0;
volatile int16_t g_az = 0;
volatile int16_t g_axf = 0;
volatile int16_t g_ayf = 0;
volatile int16_t g_azf = 0;
hb_mon_t g_mon[] = {
  { .name = "algo", .hb = &g_hb_algo, .limit = 2 },
  { .name = "ui",   .hb = &g_hb_ui,   .limit = 2 },
  { .name = "ble",  .hb = &g_hb_ble,  .limit = 3 },
};

/* ---- UI 页面模型 ---------------------------------------------------
 * 页面数量由"有独立数据可展示"决定，不拍脑袋加。
 * PAGE_MAX 是哨兵：它自己不是页面，只用来取模 / 设边界。
 * ------------------------------------------------------------------ */

 typedef enum {
	 PAGE_HOME = 0,
	 PAGE_INFO,
	 PAGE_MAX
 } page_id_t;

volatile uint32_t g_ui_page    = 0;  /* 当前页面编号（镜像，供观测） */
volatile uint32_t g_ui_page_sw = 0;  /* 累计翻页次数（用来对照按键数） */

#define MON_COUNT (sizeof(g_mon) / sizeof(g_mon[0]))

volatile uint32_t g_stall_any = 0; /* 本秒有几个任务停滞（0 = 全健康） */

osSemaphoreId_t key_semHandle;
const osSemaphoreAttr_t key_sem_attributes = { .name = "key_sem" };
osMutexId_t i2c_mutexHandle;
const osMutexAttr_t i2c_mutex_attributes = { .name = "i2c_mutex" };

/* USER CODE END Variables */
/* Definitions for ble */
osThreadId_t bleHandle;
const osThreadAttr_t ble_attributes = {
  .name = "ble",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
/* Definitions for algo */
osThreadId_t algoHandle;
const osThreadAttr_t algo_attributes = {
  .name = "algo",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for key */
osThreadId_t keyHandle;
const osThreadAttr_t key_attributes = {
  .name = "key",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for ui */
osThreadId_t uiHandle;
const osThreadAttr_t ui_attributes = {
  .name = "ui",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for wdg */
osThreadId_t wdgHandle;
const osThreadAttr_t wdg_attributes = {
  .name = "wdg",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for algo_q */
osMessageQueueId_t algo_qHandle;
const osMessageQueueAttr_t algo_q_attributes = {
  .name = "algo_q"
};
/* Definitions for ui_q */
osMessageQueueId_t ui_qHandle;
const osMessageQueueAttr_t ui_q_attributes = {
  .name = "ui_q"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static void death_log_save(void);   /* 停喂前把"死因"写进备份寄存器 */
/* USER CODE END FunctionPrototypes */

void StartBleTask(void *argument);
void StartAlgoTask(void *argument);
void StartKeyTask(void *argument);
void StartUiTask(void *argument);
void StartWdgTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */

	 i2c_mutexHandle = osMutexNew(&i2c_mutex_attributes);

  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
	key_semHandle = osSemaphoreNew(1u, 0u, &key_sem_attributes); /* 二值信号量，初始无信号 */
	/* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
	/* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of algo_q */
  algo_qHandle = osMessageQueueNew (8, 1, &algo_q_attributes);

  /* creation of ui_q */
  ui_qHandle = osMessageQueueNew (16, 1, &ui_q_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
	/* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of ble */
  bleHandle = osThreadNew(StartBleTask, NULL, &ble_attributes);

  /* creation of algo */
  algoHandle = osThreadNew(StartAlgoTask, NULL, &algo_attributes);

  /* creation of key */
  keyHandle = osThreadNew(StartKeyTask, NULL, &key_attributes);

  /* creation of ui */
  uiHandle = osThreadNew(StartUiTask, NULL, &ui_attributes);

  /* creation of wdg */
  wdgHandle = osThreadNew(StartWdgTask, NULL, &wdg_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
	/* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
	/* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartBleTask */
/**
 * @brief  Function implementing the ble thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartBleTask */
void StartBleTask(void *argument)
{
  /* USER CODE BEGIN StartBleTask */
	/* Infinite loop */
	for (;;) {
		osDelay(500);
		g_hb_ble++;
	}
  /* USER CODE END StartBleTask */
}

/* USER CODE BEGIN Header_StartAlgoTask */
/**
 * @brief Function implementing the algo thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartAlgoTask */
void StartAlgoTask(void *argument)
{
  /* USER CODE BEGIN StartAlgoTask */

	evt_t rx = EVT_NONE; /* 收回来的 */
	 uint8_t who = 0;
	      HAL_StatusTypeDef st = mpu_who_am_i(&who);

	      g_mpu_st = st;
	      if (st == HAL_OK) { g_mpu_who = who; }
	      else              { g_mpu_err++;     }
	      if(g_mpu_st == HAL_OK) g_mpu_st = mpu_wakeup();
	/* Infinite loop */
	for (;;) {
		osDelay(20);
		g_hb_algo++;
		int16_t ax,ay,az;
		osMutexAcquire(i2c_mutexHandle, osWaitForever);
		HAL_StatusTypeDef r = mpu_read_accel(&ax, &ay, &az);
		osMutexRelease(i2c_mutexHandle);
		 if (r == HAL_OK)
		{
			g_ax = ax; g_ay = ay; g_az = az;
			g_axf += (g_ax - g_axf) / 8;
			g_ayf += (g_ay - g_ayf) / 8;
			g_azf += (g_az - g_azf) / 8;
		}else{
			 g_mpu_err++;
		}

		while(osMessageQueueGet(algo_qHandle,&rx,NULL,0u) == osOK){
			g_evt_recv++;
			switch (rx) /* 再按事件码分派 */
			{
			case EVT_TICK: /* M2 阶段：什么都不用做 */
				break;
				/* M3 会在这里加：case EVT_KEY_PRESS: ...  */
			default:
				g_evt_unknown++;
				break;
			}
		}
	}
  /* USER CODE END StartAlgoTask */
}

/* USER CODE BEGIN Header_StartKeyTask */
/**
 * @brief Function implementing the key thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartKeyTask */
void StartKeyTask(void *argument)
{
  /* USER CODE BEGIN StartKeyTask */
	evt_t to_ui = EVT_KEY_PRESS;
	/* Infinite loop */
	for (;;) {

		if (osSemaphoreAcquire(key_semHandle, osWaitForever) != osOK)
			continue;
		g_hb_key++;

		osDelay(KEY_SCAN_MS);
		while (osSemaphoreAcquire(key_semHandle, 0) == osOK) {
			g_key_bounce++;
		}

		if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) != GPIO_PIN_RESET) {
			g_key_evt++;
			if (osMessageQueuePut(ui_qHandle, &to_ui, 0u, 0u) == osOK) {
				g_ui_put++;
			} else {
				g_ui_putfail++;
			}

		} else {
			g_key_bounce++;
		}
	}
  /* USER CODE END StartKeyTask */
}

/* USER CODE BEGIN Header_StartUiTask */
/**
 * @brief Function implementing the ui thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartUiTask */
void StartUiTask(void *argument)
{
  /* USER CODE BEGIN StartUiTask */
	evt_t rx = EVT_NONE;
	/* Infinite loop */
	for (;;) {
		osStatus_t st = osMessageQueueGet(ui_qHandle, &rx, NULL, 200u);
		g_hb_ui++;

		if (st == osOK) {
			switch (rx) {
			case EVT_KEY_PRESS:

				g_ui_page = (g_ui_page + 1) % PAGE_MAX;

				g_ui_evt++;
				break;
			default:
				g_evt_unknown++; /* 收到不认识的事件码，记一笔 */
				break;
			}
		}
		if (osMutexAcquire(i2c_mutexHandle, osWaitForever) == osOK) {
			ssd1306_bar(0u, g_axf);     /* 第 0 行 → 第 1 页 */
			ssd1306_bar(1u, g_ayf);     /* 第 1 行 → 第 4 页 */
			ssd1306_bar(2u, g_azf);     /* 第 2 行 → 第 7 页 */
			osMutexRelease(i2c_mutexHandle);
		}

	}
  /* USER CODE END StartUiTask */
}

/* USER CODE BEGIN Header_StartWdgTask */
/**
 * @brief Function implementing the wdg thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartWdgTask */
void StartWdgTask(void *argument)
{
  /* USER CODE BEGIN StartWdgTask */

	/* Infinite loop */
	for (;;) {
		osDelay(1000);
		/* 活着计时：wdg 任务每转一圈 = 1 秒 */

		g_hb_wdg++;
		uint32_t stalled = 0;
		uint32_t dead = 0;

		for (uint32_t i = 0; i < MON_COUNT; i++) {
			if (g_mon[i].last == *(g_mon[i].hb)) {
				g_mon[i].stall_sec++;
				stalled++;

			} else {
				g_mon[i].last = *(g_mon[i].hb);
				g_mon[i].stall_sec = 0;

			}
			if (g_mon[i].stall_sec >= g_mon[i].limit) {
				dead++;
			}

		}
#ifdef DEBUG
		if (g_force_dead) {
			g_mon[0].stall_sec = g_mon[0].limit;   /* 伪造：让 1 号任务"看起来已判死" */
			dead++;                                /* 并让"有死件"成立 */
		}
#endif
		g_stall_any = stalled;
		if (dead > 0) {
			g_dead_secs++;
			death_log_save();                      /* 留证据：复位后还能复盘是谁害的 */
		}
		if (dead == 0 || g_boot_loop_guard) {
			HAL_IWDG_Refresh(&hiwdg);
			if (g_boot_loop_guard)
				g_wdg_degraded++;              /* 只数"带病喂狗"（正常喂狗不计数） */
		}
		if (g_alive_sec < HEALTHY_SEC)
				      g_alive_sec++;

				  if (g_alive_sec == HEALTHY_SEC
					  && dead == 0
					  && !g_boot_loop_healthy)      /* 活够了 —— 上报一次 */
				  {
				      BKP->DR2 = BKP_HEALTHY;   /* 盖上"够本"印章：跨复位保留，证明这次活够了 */
				      g_boot_loop_healthy  = 1; /* 镜像：本次启动已上报 */
				      g_boot_loop_guard    = 0; /* 活够了 → 撤销 boot loop 判定 */
				  }
	}

  /* USER CODE END StartWdgTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* ---- 死亡档案：停喂前把"谁死了 + 停滞几秒"写进备份寄存器 ----------
 * 复位后由 main() 读出（DR3/DR4 跨复位保留），于是你能回答：
 *     "algo 连续停滞 7 秒 → 看门狗复位系统"
 * 只记第一个判死的任务，避免多个任务同时卡死时互相覆盖。
 * 注意：DR3/DR4 实际只有 16 位，够用（任务数 < 65535，秒数 < 65535）
 * ------------------------------------------------------------------ */
static void death_log_save(void)
{
	for (uint32_t i = 0; i < MON_COUNT; i++)
	{
		if (g_mon[i].stall_sec >= g_mon[i].limit)
		{
			BKP->DR3 = (uint16_t)(i + 1u);              /* 第几个任务（1 起；0 = 无记录） */
			BKP->DR4 = (uint16_t)(g_mon[i].stall_sec);  /* 它连续停滞了多少秒 */
			break;
		}
	}
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
	if (GPIO_Pin == key_Pin) {
		g_key_irq++;
		osSemaphoreRelease(key_semHandle); /* ISR 里只做这一件事 */
	}
}
/* USER CODE END Application */

