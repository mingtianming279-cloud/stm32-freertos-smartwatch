/*
 * rtc_time.h
 *
 * 时间子系统 —— RTC 时钟源自动选择 + 日历换算
 *
 * 这里要解决的核心问题不是"怎么读 RTC"，而是**时间从哪来**：
 * F103 的 RTC 时钟源只能三选一（LSE / LSI / HSE÷128），而 Blue Pill 板上
 * 通常不焊 32.768 kHz 晶振。所以本模块上电时先去试 LSE，试不通自动回退
 * 到 LSI，并把实际用的是哪一个暴露出来（INFO 页会显示）。
 */

#ifndef INC_RTC_TIME_H_
#define INC_RTC_TIME_H_

#include <stdint.h>

/* 实际生效的 RTC 时钟源 */
typedef enum {
	RTC_SRC_NONE = 0,   /* 两个都不起振（理论上不该发生，LSI 一般都在） */
	RTC_SRC_LSE,        /* 外部 32.768 kHz 晶振：准 */
	RTC_SRC_LSI         /* 内部 RC：能走，但一天能差一两小时 */
} rtc_src_t;

/* 拆开的时间。year 是绝对年份（2026），month 是 1~12（不是 0~11） */
typedef struct {
	uint16_t year;
	uint8_t  month;
	uint8_t  day;
	uint8_t  hour;
	uint8_t  min;
	uint8_t  sec;
	uint8_t  wday;      /* 0 = 周日 */
} rtc_tm_t;

/* 初始化。在 main() 里 HAL_PWR_EnableBkUpAccess() 之后调用。
 * 返回实际用到的时钟源。 */
rtc_src_t rtc_time_init(void);

/* Unix 秒（本项目只当"本地时间"用，不做时区换算） */
uint32_t rtc_now(void);
void     rtc_set(uint32_t epoch);

void     rtc_to_tm(uint32_t epoch, rtc_tm_t *tm);
uint32_t rtc_from_tm(const rtc_tm_t *tm);

/* 编译时刻，用作 RTC 的初值（烧录那一刻时间就是对的） */
uint32_t rtc_build_epoch(void);

/* ---- 观测量 ---------------------------------------------------------- */
extern volatile uint32_t g_rtc_src;     /* 0=None 1=LSE 2=LSI */
extern volatile uint32_t g_rtc_epoch;   /* 最近一次读到的秒 */
extern volatile uint32_t g_rtc_reset;   /* 1 = 本次上电重设过时间（备份域真丢过） */
extern volatile uint32_t g_rtc_build;   /* 编译时刻算出来的初值（排查用） */
extern volatile uint32_t g_rtc_retry;   /* rtc_set 回读校验重试了几次 */

#endif /* INC_RTC_TIME_H_ */