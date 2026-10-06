/*
 * app_events.h
 *
 *  Created on: 2026年9月10日
 *      Author: 86136
 */

#ifndef INC_APP_EVENTS_H_
#define INC_APP_EVENTS_H_

#include <stdint.h>
/* 事件码：故意只占 1 字节，和队列 Item Size = 1 严丝合缝 */
typedef uint8_t evt_t;

#define EVT_NONE        0x00u
#define EVT_TICK        0x01u   /* 周期节拍 */
#define EVT_KEY_PRESS   0x02u   /* 按键按下（旧：只有一颗键时的笼统事件，保留兼容） */
#define EVT_UI_REFRESH  0x03u   /* 请求刷新   (M3 用) */
#define EVT_BLE_RX      0x04u   /* 蓝牙收到数据 */
#define EVT_ALGO_DONE   0x05u   /* 算法算完 */

/* ── 三个按键各自的事件码 ───────────────────────────────────────────────
 * 为什么要分开而不是继续用笼统的 EVT_KEY_PRESS：ui 的按键语义是**随状态
 * 变化的**（同一个物理键在主页是"进入"、在菜单里是"上一个"、在细分页里
 * 是"返回"）。趁着还在中断/队列这一层就把"是哪个键"分清楚，
 * ui 那边就只需要按状态去查表，不用再回头猜。
 * -------------------------------------------------------------------- */
#define EVT_KEY_PREV    0x06u   /* 左键：上一个 */
#define EVT_KEY_NEXT    0x07u   /* 右键：下一个 */
#define EVT_KEY_OK      0x08u   /* 中键：确认 */

#endif /* INC_APP_EVENTS_H_ */
