/*
 * keys.h
 *
 * 三个按键 —— 独立成模块，而不是塞进 gpio.c
 *
 * 为什么不直接改 gpio.c / main.h：那两个是 CubeMX 的生成区，下次重新生成
 * 就会被覆盖掉。所以引脚定义放在这里、初始化也在 USER CODE 之后自己调，
 * 改 .ioc 的时候不会丢。
 *
 * 三个键的物理排布就是左-中-右：
 *     PB5（左 / 上一个）    PA4（中 / 确认）    PB6（右 / 下一个）
 *
 * 电平约定与原来的 PA4 保持一致：**按下接高电平**（上升沿 + 内部下拉）。
 * 如果你的按键是"按下接地"，把 keys.c 里的 IT_RISING 换成 IT_FALLING、
 * PULLDOWN 换成 PULLUP 即可，别的地方都不用动。
 */

#ifndef INC_KEYS_H_
#define INC_KEYS_H_

#include <stdint.h>

/* ---- 引脚（公开，因为 stm32f1xx_it.c 里要用）------------------------- */
#define KEY_PREV_PORT   GPIOB
#define KEY_PREV_PIN    GPIO_PIN_5      /* 上一个 */
/* ★ 下一个键原本用 PB6，实机证实那个引脚不工作（板子局部硬件问题，
 *   PB6/PB7 相邻、又都是 I2C1 的默认脚位）。
 *   换到 PA8：完全空闲，而且落在 EXTI8 —— 仍属于 EXTI9_5 那个中断向量，
 *   所以 stm32f1xx_it.c 里的处理函数一行都不用改。 */
#define KEY_NEXT_PORT   GPIOA
#define KEY_NEXT_PIN    GPIO_PIN_8      /* 下一个 */
#define KEY_OK_PORT     GPIOA
#define KEY_OK_PIN      GPIO_PIN_4      /* 确认（沿用原来那个按键） */

typedef enum {
	KEY_PREV = 0,
	KEY_OK,
	KEY_NEXT,
	KEY_NONE            /* 哨兵：不是按键，只用来表示"没有" */
} key_id_t;

/* 配置 PB5 / PB6（PA4 已经在 MX_GPIO_Init 里配好了）。
 * 在 main() 的 USER CODE 区、MX_GPIO_Init() 之后调用。 */
void keys_init(void);

/* 给 HAL_GPIO_EXTI_Callback 调用：记下是哪个键，并让 key 任务醒过来 */
void keys_isr(uint16_t gpio_pin);

/* 取走"最近一次按下的键"，取完清空。原子操作，ISR 打断不了。 */
key_id_t keys_take(void);

/* 读某个键此刻的电平：1 = 按着，0 = 松着。消抖要用。 */
uint8_t keys_level(key_id_t k);

/* ---- 观测：每个键各被按了多少次（EXTI 计数，含抖动）---------------- */
extern volatile uint32_t g_key_prev_irq;
extern volatile uint32_t g_key_ok_irq;
extern volatile uint32_t g_key_next_irq;

#endif /* INC_KEYS_H_ */