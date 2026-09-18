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
#define EVT_KEY_PRESS   0x02u   /* 按键按下   (M3 用) */
#define EVT_UI_REFRESH  0x03u   /* 请求刷新   (M3 用) */
#define EVT_BLE_RX      0x04u   /* 蓝牙收到数据 */
#define EVT_ALGO_DONE   0x05u   /* 算法算完 */

#endif /* INC_APP_EVENTS_H_ */
