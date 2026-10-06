/*
 * keys.c
 *
 * 三个按键的实现
 *
 * ── 中断怎么分派 ──────────────────────────────────────────────────────
 *   PA4 在 EXTI4 线上，PB5/PB6 落在 EXTI9_5 这一条上（F1 上 EXTI5~9
 *   共用一个中断向量）。所以是两个 ISR，但它们都只做一件事：把"哪个键"
 *   记下来，然后让 key 任务醒过来。消抖、判定、分发全部留给任务 ——
 *   和原来 PA4 的处理方式一致，ISR 里不留耗时操作。
 *
 * ── 为什么"最近按键"要加临界区 ────────────────────────────────────────
 *   keys_take() 里是"读-清"两步。如果读到之后、清之前来了中断，那一次
 *   按键就被吞掉了。三个键共用一个 s_pending，窗口虽然很窄但确实存在，
 *   所以用关中断保护一下。这个函数只在任务里调用，很短。
 * -------------------------------------------------------------------- */

#include "keys.h"
#include "main.h"

volatile uint32_t g_key_prev_irq = 0u;
volatile uint32_t g_key_ok_irq   = 0u;
volatile uint32_t g_key_next_irq = 0u;

/* 最近一次按下的键；初值和"取空"之后都是 KEY_NONE */
static volatile uint8_t s_pending = (uint8_t)KEY_NONE;

void keys_init(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* PB5 / PB6：和 PA4 用同一套电平约定 —— 上升沿 + 内部下拉，
	 * 也就是"按下接高电平"。 */
	gpio.Pin   = KEY_PREV_PIN | KEY_NEXT_PIN;
	gpio.Mode  = GPIO_MODE_IT_RISING;
	gpio.Pull  = GPIO_PULLDOWN;
	HAL_GPIO_Init(GPIOB, &gpio);

	/* PB5/PB6 共用 EXTI9_5；优先级和 EXTI4 保持一致。
	 * ★ 必须低于 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY，
	 *   否则 ISR 里调 osSemaphoreRelease 会触发断言。 */
	HAL_NVIC_SetPriority(EXTI9_5_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

void keys_isr(uint16_t gpio_pin)
{
	if (gpio_pin == KEY_PREV_PIN)
	{
		g_key_prev_irq++;
		s_pending = (uint8_t)KEY_PREV;
	}
	else if (gpio_pin == KEY_NEXT_PIN)
	{
		g_key_next_irq++;
		s_pending = (uint8_t)KEY_NEXT;
	}
	else if (gpio_pin == KEY_OK_PIN)
	{
		g_key_ok_irq++;
		s_pending = (uint8_t)KEY_OK;
	}
	else
	{
		/* 不是我们认识的引脚，忽略 */
	}
}

key_id_t keys_take(void)
{
	key_id_t r;
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	r = (key_id_t)s_pending;
	s_pending = (uint8_t)KEY_NONE;
	if (primask == 0u)
	{
		__enable_irq();
	}

	return r;
}

uint8_t keys_level(key_id_t k)
{
	switch (k)
	{
	case KEY_PREV:
		return (HAL_GPIO_ReadPin(KEY_PREV_PORT, KEY_PREV_PIN) == GPIO_PIN_SET) ? 1u : 0u;
	case KEY_NEXT:
		return (HAL_GPIO_ReadPin(KEY_NEXT_PORT, KEY_NEXT_PIN) == GPIO_PIN_SET) ? 1u : 0u;
	case KEY_OK:
		return (HAL_GPIO_ReadPin(KEY_OK_PORT, KEY_OK_PIN) == GPIO_PIN_SET) ? 1u : 0u;
	default:
		break;
	}
	return 0u;
}