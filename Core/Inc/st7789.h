/*
 * st7789.h
 *
 * ST7789 (SPI, 240x240 IPS) 底层驱动 —— 硬件层接口
 *
 * 这是"给 lcd.c 用的"，不是给任务代码用的。
 * 上层只 include lcd.h；换屏时改的是 lcd.c，不是任务代码。
 *
 * 分辨一个改动该放哪层：如果它提到 SPI、CS、DC、窗口、RGB565 字节序，
 * 那属于这一层；如果它提到"字符""矩形""居中"，那属于 lcd 层。
 */

#ifndef INC_ST7789_H_
#define INC_ST7789_H_

#include <stdint.h>

#define ST7789_W   240u
#define ST7789_H   240u

void st7789_init(void);

/* ---- 像素流三件套 ----------------------------------------------------
 * 画任何东西都是这个形状：begin 设定窗口 → data 推进若干次 → end 收尾。
 * 三步必须严格配对，中间不能插入别的 SPI 操作（命令、另一个 blit 都不行），
 * 否则 CS/DC 状态被搅乱，后面写进去的像素会整体错位 —— 症状是"花屏"，
 * 而且不报错，g_spi_tx_err 一个都不涨，很难查。
 * -------------------------------------------------------------------- */
void st7789_blit_begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void st7789_blit_data(const uint8_t *buf, uint16_t n);
void st7789_blit_end(void);

/* 便捷封装：整块填色（内部就是上面三件套跑一遍） */
void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void st7789_fill_screen(uint16_t color);

/* 取证变量（现场表达式里挂这几个）：
 * 只跑开机五色测试时正常值是 ok=1234 / err=0 / state=1。
 * 一旦屏上开始画文字和图形，这个数就不再是固定值了 —— 它的意义变成
 * "我发出去多少次"，只能用来对照"程序有没有往下跑"，不能当正确性判据。 */
extern volatile uint32_t g_spi_tx_ok;
extern volatile uint32_t g_spi_tx_err;
extern volatile uint32_t g_spi_state;

#endif /* INC_ST7789_H_ */