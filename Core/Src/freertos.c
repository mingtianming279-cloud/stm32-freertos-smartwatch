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
#include "i2c.h"
#include "lcd.h"
#include "step.h"
#include "rtc_time.h"
#include "keys.h"
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

/* ---- 屏幕布局（240x240）----------------------------------------------
 * 数字是照着 8x16 字库的格子算的，改之前先算一遍：
 *   一行 = 240/8 = 30 个字符，一屏 = 240/16 = 15 行。
 *   字号 scale=1 时一个字符占 8x16。
 * 三条柱状条纵向排开：36 / 100 / 164 起，各高 48，最低一条到 212，屏内。
 * -------------------------------------------------------------------- */
#define UI_TITLE_X     6u
#define UI_TITLE_Y     4u
#define UI_RULE_Y      26u      /* 标题下的分隔线 */

#define UI_BAR_X       22u      /* 柱状条左端 */
#define UI_BAR_W       140u     /* 条宽；中心线在正中，半宽 = 70 */
#define UI_BAR_H       48u      /* 条高 */
#define UI_BAR_Y0      36u      /* 第一条的顶边 */
#define UI_BAR_STEP    64u      /* 相邻两条的距离（48 高 + 16 空隙） */

#define UI_LABEL_X     4u       /* "X"/"Y"/"Z" 标签 */
#define UI_VALUE_X     172u     /* 数值区起点 */
#define UI_VALUE_W     64u      /* 数值区宽度（8 个字符），用来擦掉上次的残留 */

#define UI_INFO_X      6u       /* INFO 页标签列 */
#define UI_INFO_NUM_X  120u     /* INFO 页数值列 */
#define UI_INFO_NUM_W  88u      /* INFO 页数值区宽度（11 个字符），同样是拿来擦残留的 */
#define UI_INFO_Y0     40u
#define UI_INFO_STEP   22u
#define UI_INFO_LINES  9u       /* INFO 页行数。差分刷新要按行记住上一次的值 */

/* STEP 页：步数用大字号居中，下面三行是调阀值用的观测量 */
#define UI_STEP_NUM_Y  76u      /* 步数大字顶边 */
#define UI_STEP_NUM_H  48u      /* = 16 × UI_STEP_SCALE */
#define UI_STEP_SCALE   3u      /* 3 倍放大 → 24x48 一个字符，5 位数正好 120 px 宽 */
#define UI_STEP_DBG_Y  124u     /* 调试量起始行 */
#define UI_STEP_LINES   5u

/* ui_kv_line 的差分记账是 INFO / STEP 两页共用一个数组，用下标区间分开 */
#define UI_KV_LINES    (UI_INFO_LINES + UI_STEP_LINES)
#define UI_KV_STEP     UI_INFO_LINES    /* STEP 页的起始下标（跟着 INFO 行数走，别写死） */

/* CLOCK 页：时间用大字号，日期在下面 */
#define UI_CLK_TIME_Y      58u      /* HH:MM:SS 顶边 */
#define UI_CLK_TIME_SCALE   3u      /* 3 倍 → 24x48；8 个字符正好 192 px 宽 */
#define UI_CLK_DATE_Y     130u
#define UI_CLK_DATE_SCALE   2u      /* 2 倍 → 16x32；10 个字符 = 160 px 宽 */
#define UI_CLK_WDAY_Y     184u

/* ---- 菜单卡片 ---------------------------------------------------------
 * 一个细分页面 = 一张卡片。三张竖排，选中的用青色边框 + 白字，
 * 没选中的压暗。48 高 + 8 间隔，三张占 44~204，屏内。 */
#define UI_CARD_X       20u
#define UI_CARD_W      200u
#define UI_CARD_H       48u
#define UI_CARD_Y0      44u
#define UI_CARD_STEP    56u
#define UI_CARD_BORDER   2u

#define UI_TEXT_DY     16u      /* 条内标签/数值的纵向偏移：条高 48、字高 16，正好居中 */

/* 数值文字的死区（LSB）。不加它的话，静止时加速度那 ±1 的抖动会让文字
 * 每帧重画一次，看上去就是数字在原地抖 —— 比不刷新更难受。 */
#define UI_VALUE_DEADBAND  16

/* ui 循环节拍。50 ms 是差分刷新之后才敢用的：一帧的 SPI 传输量从约 70 KB
 * 降到几百字节，才轮得到聊"跟不跟手"。之前 200 ms 一帧光刷屏就占掉一半时间。 */
#define UI_PERIOD_MS   50u
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
volatile uint32_t g_ui_refresh = 0; /* 预留：ui 真正画完一屏的次数（目前还没有人给它赋值） */
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
	 PAGE_CLOCK = 0,     /* 时间 —— ★ 主页 */
	 PAGE_STEP,          /* 步数 */
	 PAGE_ACCEL,         /* 三轴柱状条（原来的 HOME） */
	 PAGE_INFO,          /* 系统状态 */
	 PAGE_MAX
 } page_id_t;

 /* ── UI 三个状态 ────────────────────────────────────────────────────────
  * 组长说过的那套导航：
  *   主页(时间)  --任意键-->  菜单(卡片)  --左右选、中键进-->  细分页
  *   细分页里三个键换语义：左=返回上一步、中=确认、右=回到主页
  * ------------------------------------------------------------------- */
 typedef enum {
	 UI_MODE_MAIN = 0,   /* 主页：时间 */
	 UI_MODE_MENU,       /* 菜单：卡片选择 */
	 UI_MODE_DETAIL      /* 细分页 */
 } ui_mode_t;

 /* 菜单里能选哪几页。时间不在其中 —— 它本身就是主页。 */
 typedef enum {
	 MENU_STEP = 0,
	 MENU_ACCEL,
	 MENU_INFO,
	 MENU_MAX
 } menu_id_t;

 static const uint8_t s_menu_pages[MENU_MAX] = { PAGE_STEP, PAGE_ACCEL, PAGE_INFO };
 static const char *const s_menu_names[MENU_MAX] = { "STEPS", "ACCEL", "INFO" };

volatile uint32_t g_ui_page    = 0;  /* 当前页面编号（镜像，供观测） */
volatile uint32_t g_ui_page_sw = 0;  /* 累计切换次数 */
volatile uint32_t g_ui_mode    = 0;  /* UI_MODE_MAIN / MENU / DETAIL */
volatile uint32_t g_ui_menu_sel = 0; /* 菜单里选中的第几项 */
volatile uint32_t g_ui_detail  = 0;  /* 细分页对应的菜单下标 */

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
  /* ★ 从 256 加到 320。原因：ui 现在的调用链一路要走到
   * lcd_draw_str → lcd_draw_char → st7789_fill_rect → st_set_window →
   * HAL_SPI_Transmit，用 -fstack-usage 量出来光链子本身就约 300 字节，
   * 还要加上 HAL 内部开销和中断压栈。256 字（1KB）没溢出，但余量只剩一倍多，
   * 而 configCHECK_FOR_STACK_OVERFLOW 没开 —— 栈溢出在 F103 上是静默踩内存，
   * 表现为莫名其妙的死机，不值得为省 256 字节冒这个险。
   * heap 总共 10KB、只用了约 4.4KB，加这 256 字节绰绰有余。
   * ⚠ CubeMX 重新生成代码会把它改回 256，届时记得再改一次。 */
  .stack_size = 320 * 4,
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
static void ui_invalidate(void);
static void ui_render(void);
static void ui_handle_key(evt_t ev);
static void ui_bar_update(uint8_t idx, uint16_t y, char label, int16_t v);
static void ui_kv_line(uint8_t idx, uint16_t y, const char *label, uint32_t value);
static void ui_step_page(void);
static void ui_info_page(void);
static void ui_clock_page(void);
static void ui_accel_page(void);
static void ui_menu_page(void);
static const char *ui_title_of(uint8_t page);
static void ui_page_body(uint8_t page);
static void ui_two(char *p, uint8_t v);
static void ui_rtc_line(uint16_t y);
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

	step_reset();                 /* 上电从 0 步开始，同时把重力基线置成 1g */

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

			/* ★ 计步吃的是原始值 ax/ay/az，不是上面那三个低通值。
			 * 低通（系数 1/8，截止约 3 Hz）的本意是滤掉抖动、让柱状条看起来
			 * 稳一点，而步态的波动恰好就落在这个频段里 —— 拿 g_axf 去计步，
			 * 等于先把要测的东西滤掉了。这是整个接入里最容易接错的一根线。 */
			step_update(ax, ay, az, (uint16_t)ALGO_PERIOD_MS);
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
	/* Infinite loop */
	for (;;) {
		evt_t to_ui;
		key_id_t k;

		if (osSemaphoreAcquire(key_semHandle, osWaitForever) != osOK)
			continue;
		g_hb_key++;

		/* 消抖：先等过抖动窗口，再回头确认那个键还按着 */
		osDelay(KEY_SCAN_MS);

		/* 把窗口期内到达的其它中断吃掉 —— 否则它们会在下一轮立刻
		 * 触发一次"假按键" */
		while (osSemaphoreAcquire(key_semHandle, 0) == osOK) {
			g_key_bounce++;
		}

		k = keys_take();
		if (k == KEY_NONE) {
			g_key_bounce++;
			continue;
		}

		/* 抖完已经松开了 = 毛刺，丢掉 */
		if (keys_level(k) == 0u) {
			g_key_bounce++;
			continue;
		}

		switch (k) {
		case KEY_PREV: to_ui = EVT_KEY_PREV; break;
		case KEY_NEXT: to_ui = EVT_KEY_NEXT; break;
		case KEY_OK:   to_ui = EVT_KEY_OK;   break;
		default:       continue;
		}

		g_key_evt++;
		if (osMessageQueuePut(ui_qHandle, &to_ui, 0u, 0u) == osOK) {
			g_ui_put++;
		} else {
			g_ui_putfail++;
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
	uint8_t view_prev = 0xFFu;   /* 上一帧的界面标识（状态+页面）；0xFF = 还没画过 */

	ui_invalidate();             /* 开机第一帧必须走完整绘制 */
	/* Infinite loop */
	for (;;) {
		osStatus_t st = osMessageQueueGet(ui_qHandle, &rx, NULL, UI_PERIOD_MS);
		g_hb_ui++;

		if (st == osOK) {
			switch (rx) {
			/* 三个物理键。它们各自是什么意思，完全由 ui_handle_key
			 * 按当前状态决定 —— 队列这一层只负责把"是哪个键"送上来 */
			case EVT_KEY_PREV:
			case EVT_KEY_NEXT:
			case EVT_KEY_OK:
				g_ui_evt++;
				ui_handle_key(rx);
				break;
			/* 旧的笼统事件：当成"确认"处理，保留兼容 */
			case EVT_KEY_PRESS:
				g_ui_evt++;
				ui_handle_key(EVT_KEY_OK);
				break;
			default:
				g_evt_unknown++; /* 收到不认识的事件码，记一笔 */
				break;
			}
		}

		/* 界面标识 = 状态 + 页面。任何一项变了都要清一次内容区：
		 * 本屏没有 framebuffer，上一屏画过的东西不会自己消失。
		 * 只清内容区、不碰标题那一行以上，能省一点是一点。 */
		{
			uint8_t view = (uint8_t)(((uint8_t)g_ui_mode << 4) | (uint8_t)(g_ui_page & 0x0Fu));
			if (view != view_prev) {
				lcd_fill_rect(0u, UI_RULE_Y, LCD_W, (uint16_t)(LCD_H - UI_RULE_Y), LCD_BLACK);
				view_prev = view;
				ui_invalidate();
			}
		}

		/* 屏幕走 SPI，不再碰 I2C，所以不需要 i2c_mutex 了。
		 * 注意：以后如果有第二个任务也要画屏，得给 SPI 配一把锁 ——
		 * 现在只有 ui 一个任务在写屏，所以先不加。 */
		ui_render();
		g_ui_refresh++;   /* 这个计数器一直是预留，现在真的有人给它赋值了 */
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

/* ---- UI 状态：差分刷新 ------------------------------------------------
 * 屏幕没有 framebuffer，往上面写的每一个字节都是立刻可见的。所以"每帧
 * 无脑重绘整块"的代价不只是慢 —— 柱状条区域每帧被填一次底色再重画，
 * 肉眼看到的就是周期性频闪。这个坑是在实机上跑出来之后才补的。
 *
 * 做法：把上一次画出来的形状记住，这一帧只写"和上次不一样"的那一段。
 * 结果是静止时屏幕完全不动，动的时候也只有变化的那几列在更新。
 *
 * ⚠ 所有状态都必须在换页时作废（ui_invalidate）。否则新页会拿着上一页
 *   的记录去做差分，该画的没画，屏幕上就是一片残缺。
 * -------------------------------------------------------------------- */
typedef struct {
	int16_t len;    /* 上次画出来的条长（带符号：负 = 往左），单位像素 */
	int16_t text;   /* 上次显示出来的数值，用来判断要不要重画文字 */
	uint8_t valid;  /* 0 = 这一条还没画过，静态部分要完整画一遍 */
} bar_state_t;

static bar_state_t s_bar[3];
static uint32_t s_info_prev[UI_KV_LINES];     /* 键值行的上一次值（INFO 与 STEP 两页共用） */
static uint32_t s_step_shown = 0xFFFFFFFFu;   /* 上次显示的步数；初值取一个不可能的值以强制首帧重画 */
static uint32_t s_rtc_shown  = 0xFFFFFFFFu;   /* 上次显示的 RTC 时钟源 */
static uint32_t s_menu_sel_shown = 0xFFFFFFFFu; /* 菜单上次高亮的是第几项 */
static uint8_t s_page_valid = 0u;             /* 0 = 当前页的静态部分还没画 */

static void ui_invalidate(void)
{
	for (uint8_t i = 0u; i < (uint8_t)(sizeof(s_bar) / sizeof(s_bar[0])); i++)
	{
		s_bar[i].valid = 0u;
	}
	s_page_valid = 0u;
	s_step_shown = 0xFFFFFFFFu;
	s_rtc_shown  = 0xFFFFFFFFu;
	s_menu_sel_shown = 0xFFFFFFFFu;
}

static void ui_bar_update(uint8_t idx, uint16_t y, char label, int16_t v)
{
	char buf[12];
	bar_state_t *st = &s_bar[idx];
	int32_t half = (int32_t)(UI_BAR_W / 2u);   /* 70：半宽 */
	int32_t cx   = (int32_t)UI_BAR_X + half;   /* 92：中心线位置 */
	uint16_t ty  = (uint16_t)(y + UI_TEXT_DY);
	int32_t len, old, old_lo, old_hi, new_lo, new_hi;
	uint8_t redraw_text = 0u;

	/* 满量程 ±32768 映射到半边宽度。先夹住再画：滤波后的加速度偶尔会
	 * 超出量程，不夹的话条会画到框外面去，而越界窗口 ST7789 是整块丢弃的，
	 * 症状是"这一条突然整个不见了"。 */
	len = ((int32_t)v * half) / 32768;
	if (len >  half) len =  half;
	if (len < -half) len = -half;

	if (st->valid == 0u)
	{
		/* 首次进这一页：静态部分一次性画好，之后再也不碰它们 */
		lcd_draw_char(UI_LABEL_X, ty, label, LCD_WHITE, LCD_BLACK, 1u);
		lcd_fill_rect(UI_BAR_X, y, UI_BAR_W, UI_BAR_H, LCD_DARK);
		lcd_fill_rect((uint16_t)cx, y, 1u, UI_BAR_H, LCD_GREY);
		st->len = 0;
		st->valid = 1u;
		redraw_text = 1u;
	}
	else
	{
		int32_t d = (int32_t)v - (int32_t)st->text;
		if ((d > UI_VALUE_DEADBAND) || (d < -UI_VALUE_DEADBAND)) redraw_text = 1u;
	}

	/* ---- 条：只写变化的那一段 ----
	 * 条是从中心线往两边长的，所以把旧形状和新形状各截成 [左端, 0) 与
	 * [0, 右端) 两段分别比。永远只有三种情况：旧有新无（擦回底色）、
	 * 新有旧无（补画）、两边都有（不动）。一帧通常只写几列像素。 */
	if (st->len != (int16_t)len)
	{
		old    = st->len;
		old_lo = (old < 0) ? old : 0;
		old_hi = (old > 0) ? old : 0;
		new_lo = (len < 0) ? len : 0;
		new_hi = (len > 0) ? len : 0;

		if (old_lo < new_lo)
		{
			lcd_fill_rect((uint16_t)(cx + old_lo), (uint16_t)(y + 2u),
			              (uint16_t)(new_lo - old_lo), (uint16_t)(UI_BAR_H - 4u), LCD_DARK);
		}
		if (old_hi > new_hi)
		{
			lcd_fill_rect((uint16_t)(cx + new_hi), (uint16_t)(y + 2u),
			              (uint16_t)(old_hi - new_hi), (uint16_t)(UI_BAR_H - 4u), LCD_DARK);
		}

		/* 条往正方向起步时，上面那次擦除的区间正好从中心线开始，会把中心线
		 * 一起抹掉。补一笔最省事 —— 就 1x48 像素，比判断边界便宜得多。 */
		lcd_fill_rect((uint16_t)cx, y, 1u, UI_BAR_H, LCD_GREY);

		if (new_lo < old_lo)
		{
			lcd_fill_rect((uint16_t)(cx + new_lo), (uint16_t)(y + 2u),
			              (uint16_t)(old_lo - new_lo), (uint16_t)(UI_BAR_H - 4u), LCD_RED);
		}
		if (new_hi > old_hi)
		{
			lcd_fill_rect((uint16_t)(cx + old_hi), (uint16_t)(y + 2u),
			              (uint16_t)(new_hi - old_hi), (uint16_t)(UI_BAR_H - 4u), LCD_GREEN);
		}

		st->len = (int16_t)len;
	}

	if (redraw_text)
	{
		/* 大小先擦一块固定宽度再写：位数变少时（-12345 → -99）后面会留残影。
		 * 没有 framebuffer，屏幕不会自己刷新，残影得自己负责。 */
		lcd_fill_rect(UI_VALUE_X, ty, UI_VALUE_W, 16u, LCD_BLACK);
		lcd_format_int(buf, (int32_t)v);
		lcd_draw_str(UI_VALUE_X, ty, buf, LCD_WHITE, LCD_BLACK, 1u);
		st->text = v;
	}
}

/* 一行"标签 + 数值"。INFO 和 STEP 两页共用，靠 idx 区分各自的记账位置。
 * 标签只在换页后的第一帧画 —— 它永远不变，每帧重画只会让屏幕闪。 */
static void ui_kv_line(uint8_t idx, uint16_t y, const char *label, uint32_t value)
{
	char buf[12];

	if (s_page_valid == 0u)
	{
		lcd_draw_str(UI_INFO_X, y, label, LCD_GREY, LCD_BLACK, 1u);
	}
	else if (s_info_prev[idx] == value)
	{
		return;   /* 这一行的数没变，一个字都不用写 */
	}

	lcd_fill_rect(UI_INFO_NUM_X, y, UI_INFO_NUM_W, 16u, LCD_BLACK);
	lcd_format_int(buf, (int32_t)value);
	lcd_draw_str(UI_INFO_NUM_X, y, buf, LCD_WHITE, LCD_BLACK, 1u);
	s_info_prev[idx] = value;
}

static void ui_step_page(void)
{
	char buf[12];
	uint32_t n = step_count();
	uint16_t w, x;
	int32_t  d = g_step_dyn;

	if (d < 0) d = -d;   /* 显示幅度，符号没有意义 */

	/* 步数大字：先整块擦掉再居中画。位数一变（99 → 100）起点就跟着变，
	 * 不整块擦的话前一位会留下残影。整块是 240x48，但只在步数真的变了
	 * 的那一帧才动它 —— 走路时也就一两赫兹。 */
	if (s_page_valid == 0u || s_step_shown != n)
	{
		lcd_fill_rect(0u, UI_STEP_NUM_Y, LCD_W, UI_STEP_NUM_H, LCD_BLACK);
		lcd_format_int(buf, (int32_t)n);
		w = lcd_str_width(buf, UI_STEP_SCALE);
		x = (uint16_t)((LCD_W - w) / 2u);
		lcd_draw_str(x, UI_STEP_NUM_Y, buf, LCD_WHITE, LCD_BLACK, UI_STEP_SCALE);
		s_step_shown = n;
	}

	/* 下面三个是调阀值用的：正常走一段路，盯 PEAK —— 阈值要明显低于它，
	 * 又明显高于静止时的噪声底。REJ 在快速上涨说明阈值偏低。 */
	ui_kv_line((uint8_t)(UI_KV_STEP + 0u), (uint16_t)(UI_STEP_DBG_Y + 0u * UI_INFO_STEP), "DYN",   (uint32_t)d);
	ui_kv_line((uint8_t)(UI_KV_STEP + 1u), (uint16_t)(UI_STEP_DBG_Y + 1u * UI_INFO_STEP), "DC",    (uint32_t)((g_step_dc < 0) ? -g_step_dc : g_step_dc));
	ui_kv_line((uint8_t)(UI_KV_STEP + 2u), (uint16_t)(UI_STEP_DBG_Y + 2u * UI_INFO_STEP), "PEAK",  (uint32_t)g_step_dyn_peak);
	ui_kv_line((uint8_t)(UI_KV_STEP + 3u), (uint16_t)(UI_STEP_DBG_Y + 3u * UI_INFO_STEP), "REJ",   g_step_reject);
	ui_kv_line((uint8_t)(UI_KV_STEP + 4u), (uint16_t)(UI_STEP_DBG_Y + 4u * UI_INFO_STEP), "SHOCK", g_step_shock);
}

/* RTC 时钟源那一行。为什么不用 ui_kv_line 直接显示数字：
 * 这一行的全部意义就是"一眼看出板子上那颗 32.768 kHz 晶振在不在"，
 * 而 0/1/2 得翻文档才知道含义 —— 不如直接写名字，再用颜色标出可信度。 */
static void ui_rtc_line(uint16_t y)
{
	static const char *const src_name[3] = { "NONE", "LSE", "LSI" };
	uint32_t v = (g_rtc_src < 3u) ? g_rtc_src : 0u;
	uint16_t color;

	if (s_page_valid == 0u)
	{
		lcd_draw_str(UI_INFO_X, y, "RTC SRC", LCD_GREY, LCD_BLACK, 1u);
	}
	else if (s_rtc_shown == v)
	{
		return;                 /* 没变，一个字都不用写 */
	}

	/* LSE 绿（准）／LSI 黄（能走但会漂）／NONE 红（两个都没起来，时间不可信） */
	color = (v == 1u) ? LCD_GREEN : ((v == 2u) ? LCD_YELLOW : LCD_RED);

	lcd_fill_rect(UI_INFO_NUM_X, y, UI_INFO_NUM_W, 16u, LCD_BLACK);
	lcd_draw_str(UI_INFO_NUM_X, y, src_name[v], color, LCD_BLACK, 1u);
	s_rtc_shown = v;
}

static void ui_info_page(void)
{
	/* INFO 页不放传感器数据，放"系统还在不在正常运转"的那几个数。
	 * 这些数以前只能接调试器挂现场表达式才看得到，现在按键翻一页就有
	 * —— 对一块要戴在手上的表来说，这才是它该有的观测方式。 */
	uint16_t y = UI_INFO_Y0;
	uint8_t  i = 0u;

	ui_kv_line(i++, y, "HB ALGO", g_hb_algo);   y = (uint16_t)(y + UI_INFO_STEP);
	ui_kv_line(i++, y, "HB UI",   g_hb_ui);     y = (uint16_t)(y + UI_INFO_STEP);
	ui_kv_line(i++, y, "HB WDG",  g_hb_wdg);    y = (uint16_t)(y + UI_INFO_STEP);
	ui_kv_line(i++, y, "STALL",   g_stall_any); y = (uint16_t)(y + UI_INFO_STEP);
	ui_kv_line(i++, y, "DEAD",    g_dead_secs); y = (uint16_t)(y + UI_INFO_STEP);
	ui_kv_line(i++, y, "I2C FND", g_i2c_found); y = (uint16_t)(y + UI_INFO_STEP);
	ui_rtc_line(y);
	y = (uint16_t)(y + UI_INFO_STEP);
	ui_kv_line(i++, y, "RTC SET", g_rtc_build); y = (uint16_t)(y + UI_INFO_STEP);
	ui_kv_line(i++, y, "RTC NOW", g_rtc_epoch);
}

/* 两位十进制。不用 sprintf —— 那会把 newlib 整个格式化库拉进固件，
 * 为一个两位数不值得（lcd_format_int 当初拒绝 sprintf 也是这个理由）。 */
static void ui_two(char *p, uint8_t v)
{
	p[0] = (char)('0' + ((v / 10u) % 10u));
	p[1] = (char)('0' + (v % 10u));
}

static void ui_clock_page(void)
{
	static const char *const wday_name[7] = {
		"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
	};
	char     buf[12];
	rtc_tm_t tm;
	uint16_t w, x;

	rtc_to_tm(rtc_now(), &tm);

	/* ── 时间 HH:MM:SS ───────────────────────────────────────────────
	 * 这一行走的是"整行覆盖重画"而不是差分：秒每秒都在变，而且宽度
	 * 固定 8 个字符、不会出现位数变化留下的残影，所以直接盖上去就行 ——
	 * 覆盖式重画在同一位置不会闪，比"先擦后画"还干净。 */
	ui_two(&buf[0], tm.hour);
	buf[2] = ':';
	ui_two(&buf[3], tm.min);
	buf[5] = ':';
	ui_two(&buf[6], tm.sec);
	buf[8] = '\0';

	w = lcd_str_width(buf, UI_CLK_TIME_SCALE);
	x = (uint16_t)((LCD_W - w) / 2u);
	lcd_draw_str(x, UI_CLK_TIME_Y, buf, LCD_WHITE, LCD_BLACK, UI_CLK_TIME_SCALE);

	/* ── 日期 YYYY-MM-DD ─────────────────────────────────────────── */
	buf[0] = (char)('0' + ((tm.year / 1000u) % 10u));
	buf[1] = (char)('0' + ((tm.year / 100u) % 10u));
	buf[2] = (char)('0' + ((tm.year / 10u) % 10u));
	buf[3] = (char)('0' + (tm.year % 10u));
	buf[4] = '-';
	ui_two(&buf[5], tm.month);
	buf[7] = '-';
	ui_two(&buf[8], tm.day);
	buf[10] = '\0';

	w = lcd_str_width(buf, UI_CLK_DATE_SCALE);
	x = (uint16_t)((LCD_W - w) / 2u);
	lcd_draw_str(x, UI_CLK_DATE_Y, buf, LCD_GREY, LCD_BLACK, UI_CLK_DATE_SCALE);

	/* ── 星期 ─────────────────────────────────────────────────────── */
	lcd_draw_str(UI_TITLE_X, UI_CLK_WDAY_Y, wday_name[tm.wday % 7u],
	             LCD_CYAN, LCD_BLACK, 1u);
}

/* 三条加速度柱状条。它原来是"主页"，现在降级成菜单里的一项 ——
 * 本质上这是个调试视图，不该占着主页的位置。 */
static void ui_accel_page(void)
{
	ui_bar_update(0u, (uint16_t)(UI_BAR_Y0 + 0u * UI_BAR_STEP), 'X', g_axf);
	ui_bar_update(1u, (uint16_t)(UI_BAR_Y0 + 1u * UI_BAR_STEP), 'Y', g_ayf);
	ui_bar_update(2u, (uint16_t)(UI_BAR_Y0 + 2u * UI_BAR_STEP), 'Z', g_azf);
}

static const char *ui_title_of(uint8_t page)
{
	switch (page)
	{
	case PAGE_STEP:  return "STEPS";
	case PAGE_ACCEL: return "ACCEL";
	case PAGE_INFO:  return "INFO";
	default:         return "TIME";     /* PAGE_CLOCK */
	}
}

/* 画具体页面的内容（标题由 ui_render 统一处理） */
static void ui_page_body(uint8_t page)
{
	switch (page)
	{
	case PAGE_STEP:  ui_step_page();  break;
	case PAGE_ACCEL: ui_accel_page(); break;
	case PAGE_INFO:  ui_info_page();  break;
	default:         ui_clock_page(); break;
	}
}

/* 一张菜单卡片。选中的：青色边框 + 白字；没选中的：暗边框 + 灰字。
 * 边框是靠"整块填边框色、再把内部挖成背景色"画出来的 —— 比画四条边省事，
 * 而且圆角没有、方块感正好和这个字库搭。 */
static void ui_card(uint16_t y, const char *label, uint8_t selected)
{
	uint16_t border = (selected != 0u) ? LCD_CYAN : LCD_DARK;
	uint16_t fg     = (selected != 0u) ? LCD_WHITE : LCD_GREY;
	uint16_t w, x;

	lcd_fill_rect(UI_CARD_X, y, UI_CARD_W, UI_CARD_H, border);
	lcd_fill_rect((uint16_t)(UI_CARD_X + UI_CARD_BORDER),
	              (uint16_t)(y + UI_CARD_BORDER),
	              (uint16_t)(UI_CARD_W - 2u * UI_CARD_BORDER),
	              (uint16_t)(UI_CARD_H - 2u * UI_CARD_BORDER), LCD_BLACK);

	w = lcd_str_width(label, 2u);
	x = (uint16_t)(UI_CARD_X + (UI_CARD_W - w) / 2u);
	lcd_draw_str(x, (uint16_t)(y + 8u), label, fg, LCD_BLACK, 2u);
}

static void ui_menu_page(void)
{
	uint8_t sel = (uint8_t)(g_ui_menu_sel % MENU_MAX);
	uint8_t i;

	if (s_page_valid == 0u)
	{
		/* 刚进菜单：标题 + 三张卡片全画 */
		lcd_draw_str(UI_TITLE_X, UI_TITLE_Y, "MENU", LCD_CYAN, LCD_BLACK, 1u);
		lcd_fill_rect(0u, UI_RULE_Y, LCD_W, 1u, LCD_GREY);
		for (i = 0u; i < (uint8_t)MENU_MAX; i++)
		{
			ui_card((uint16_t)(UI_CARD_Y0 + i * UI_CARD_STEP),
			        s_menu_names[i], (uint8_t)(i == sel));
		}
		s_menu_sel_shown = sel;
		return;
	}

	if (s_menu_sel_shown == sel)
	{
		return;                      /* 选择没动，一个字都不用写 */
	}

	/* 只有"原来选中的"和"现在选中的"这两张要重画 ——
	 * 每按一次就把三张全刷一遍是没必要的 */
	if (s_menu_sel_shown < (uint32_t)MENU_MAX)
	{
		ui_card((uint16_t)(UI_CARD_Y0 + s_menu_sel_shown * UI_CARD_STEP),
		        s_menu_names[s_menu_sel_shown], 0u);
	}
	ui_card((uint16_t)(UI_CARD_Y0 + sel * UI_CARD_STEP), s_menu_names[sel], 1u);
	s_menu_sel_shown = sel;
}

/* 细分页里按"确认"要做什么。目前只有步数页有实质动作（清零），
 * 别的页先留空 —— 等页面本身定下来再填。 */
static void ui_detail_action(uint8_t menu_idx)
{
	if (menu_idx == (uint8_t)MENU_STEP)
	{
		step_reset();
		g_step_dyn_peak = 0;         /* 连峰值一起清，否则调参时看着别扭 */
	}
}

/*
 * 一次按键 = 一次状态迁移。语义完全由**当前状态**决定：
 *
 *   主页   ：任意键      → 进菜单
 *   菜单   ：左 / 右     → 上下选
 *             中          → 进入选中的页面
 *   细分页 ：左          → 返回上一步（回菜单）
 *             中          → 确认（页面自己的动作）
 *             右          → 回到主页
 *
 * 这就是你要的那套"三个键三种语义随页面切换"。物理键位始终是左-中-右，
 * 用户不用记"现在这个键是什么" —— 位置对了就行。
 */
static void ui_handle_key(evt_t ev)
{
	switch (g_ui_mode)
	{
	case (uint32_t)UI_MODE_MAIN:
		g_ui_mode     = (uint32_t)UI_MODE_MENU;
		g_ui_menu_sel = 0u;
		g_ui_page_sw++;
		break;

	case (uint32_t)UI_MODE_MENU:
		if (ev == EVT_KEY_PREV)
		{
			g_ui_menu_sel = (g_ui_menu_sel + (uint32_t)MENU_MAX - 1u) % (uint32_t)MENU_MAX;
		}
		else if (ev == EVT_KEY_NEXT)
		{
			g_ui_menu_sel = (g_ui_menu_sel + 1u) % (uint32_t)MENU_MAX;
		}
		else if (ev == EVT_KEY_OK)
		{
			g_ui_detail = g_ui_menu_sel;
			g_ui_mode   = (uint32_t)UI_MODE_DETAIL;
			g_ui_page   = s_menu_pages[g_ui_detail % (uint32_t)MENU_MAX];
		}
		else { /* 不认识的键 */ }
		g_ui_page_sw++;
		break;

	case (uint32_t)UI_MODE_DETAIL:
		if (ev == EVT_KEY_PREV)              /* 返回上一步 */
		{
			g_ui_mode = (uint32_t)UI_MODE_MENU;
		}
		else if (ev == EVT_KEY_NEXT)         /* 回到主页 */
		{
			g_ui_mode = (uint32_t)UI_MODE_MAIN;
			g_ui_page = (uint32_t)PAGE_CLOCK;
		}
		else if (ev == EVT_KEY_OK)           /* 确认 */
		{
			ui_detail_action((uint8_t)(g_ui_detail % (uint32_t)MENU_MAX));
		}
		else { /* 忽略 */ }
		g_ui_page_sw++;
		break;

	default:
		g_ui_mode = (uint32_t)UI_MODE_MAIN;  /* 状态坏了就回主页，别卡住 */
		break;
	}

	/* 界面变了，差分记账必须全部作废 —— 否则新界面会拿着旧记录去比，
	 * 该画的不画，屏幕上就是一片残缺。 */
	ui_invalidate();
}

static void ui_render(void)
{
	uint8_t page;

	/* 菜单是独立的一屏，标题也归它自己画 */
	if (g_ui_mode == (uint32_t)UI_MODE_MENU)
	{
		ui_menu_page();
		s_page_valid = 1u;
		return;
	}

	page = (g_ui_mode == (uint32_t)UI_MODE_MAIN)
	     ? (uint8_t)PAGE_CLOCK
	     : (uint8_t)s_menu_pages[g_ui_detail % (uint32_t)MENU_MAX];

	if (s_page_valid == 0u)
	{
		/* 换页后的第一帧才需要画标题和分隔线。它们之后一直不变，
		 * 每帧重画既浪费、又会让屏幕顶部跟着闪。 */
		lcd_draw_str(UI_TITLE_X, UI_TITLE_Y, ui_title_of(page), LCD_CYAN, LCD_BLACK, 1u);
		lcd_fill_rect(0u, UI_RULE_Y, LCD_W, 1u, LCD_GREY);
	}

	ui_page_body(page);
	s_page_valid = 1u;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
	/* 三个键走同一条路：先让 keys 模块记下"是哪个键"，再放一次信号量。
	 * 消抖、判定、分发全都留给 key 任务 —— ISR 里只做最轻的两件事，
	 * 和原来只有一颗键时的处理方式一致。 */
	if ((GPIO_Pin == KEY_PREV_PIN) || (GPIO_Pin == KEY_NEXT_PIN) || (GPIO_Pin == KEY_OK_PIN)) {
		keys_isr(GPIO_Pin);
		g_key_irq++;
		osSemaphoreRelease(key_semHandle);
	}
}
/* USER CODE END Application */

