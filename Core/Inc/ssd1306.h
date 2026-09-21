/*
 * ssd1306.h
 *
 * SSD1306 (I2C, 128x64 单色) 驱动 —— 对外接口
 *
 * 分层约定：
 *   上层（UI 任务）只 include 这个文件，不碰 HAL / I2C 细节。
 *   将来换 ST7789，只需再提供一组同名函数的实现，UI 一行不改。
 */

#ifndef INC_SSD1306_H_
#define INC_SSD1306_H_

#include <stdint.h>

/* SSD1306 的 7 位 I2C 地址。模块背面若标注 0x3D，改这一行 */
#define SSD1306_I2C_ADDR7   0x3Cu

/* ---- 观测镜像（调试器读不了 HAL 内部状态，只能看这些）---- */
extern volatile uint32_t g_i2c_found;       /* 总线上应答的器件个数 */
extern volatile uint32_t g_i2c_addr1;       /* 第 1 个地址（期望 0x3C） */
extern volatile uint32_t g_i2c_addr2;       /* 第 2 个地址（期望 0x68） */
extern volatile uint32_t g_ssd1306_online;  /* 1 = SSD1306 在线 */
void ssd1306_init(void);
void ssd1306_fill(uint8_t color);
/* 开机自检：扫描总线 + 探测 SSD1306，结果写进上面的镜像变量 */
void ssd1306_selftest(void);
void ssd1306_bar(uint8_t row, int16_t v);

#endif /* INC_SSD1306_H_ */
