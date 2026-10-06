/*
 * lcd.h
 *
 * 显示抽象层 —— 任务代码只认这一个文件
 *
 * 设计约定（这条约定决定了以后换屏要改几个地方）：
 *   上层（ui 任务、main）只 include lcd.h，不碰 SPI / GPIO / 命令序列 / 窗口。
 *   换屏（不再用 ST7789、换回 OLED、或者换别的彩屏）只改 lcd.c 里的实现，
 *   这一份头文件的所有调用方一行都不用动。
 *
 * 当前实现：ST7789 240x240 IPS，RGB565。
 *
 * ⚠ 关于"没有 framebuffer"这件事实：整屏 240x240x2 = 115200 字节，
 *   而 F103C8T6 一共只有 20KB RAM，装不下。所以这里所有绘制函数都是
 *   "直接往屏上写"，没有离屏画布。由此带来两个可见的约束：
 *     ① 画上去就擦不掉，要改内容必须用背景色重画覆盖（文字带 bg 参数就是这个用途）；
 *     ② 没有"先画好再整体切换"，所以刷新时屏上会出现短暂的中间状态。
 */

#ifndef INC_LCD_H_
#define INC_LCD_H_

#include <stdint.h>

#define LCD_W   240u
#define LCD_H   240u

/* RGB565: R5 G6 B5 —— 每像素 2 字节 */
#define LCD_RGB(r, g, b)  ((uint16_t)(((r) & 0xF8u) << 8) | \
                           (uint16_t)(((g) & 0xFCu) << 3) | \
                           (uint16_t)((b) >> 3))

/* 常用纯色 */
#define LCD_BLACK   0x0000u
#define LCD_WHITE   0xFFFFu
#define LCD_RED     0xF800u
#define LCD_GREEN   0x07E0u
#define LCD_BLUE    0x001Fu
#define LCD_YELLOW  0xFFE0u
#define LCD_CYAN    0x07FFu
#define LCD_GREY    0x8410u

/* 深灰：柱状条等"底色"，比纯黑柔和，能看出块的边界 */
#define LCD_DARK    0x18E3u

void lcd_init(void);
void lcd_clear(uint16_t color);
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* ---- 字符绘制 --------------------------------------------------------
 * scale 是整数倍放大：1 = 8x16 原始大小，2 = 16x32。
 * 放大靠"每个像素铺成 scale×scale 的方块"，不做插值 ——
 * 点阵字体放大后带方块感是预期效果，不是缺陷。
 * bg 不是背景透明，而是"把字符格子填成这个色"，否则旧内容会残留在字形周围。
 *
 * ⚠ 约束：字符必须完整落在屏内，部分出屏的字符会被整个跳过（不画半截）。
 *   这是"没有 framebuffer"的直接后果 —— 没有地方先拼好再裁剪。
 *   调用方负责把坐标算在范围内，lcd_draw_str 会在超出行宽时自动停下。
 * -------------------------------------------------------------------- */
void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void lcd_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/* 一个字符串画出来有多宽（像素），用于居中排版 */
uint16_t lcd_str_width(const char *s, uint8_t scale);

/* ---- 数字转字符串 ----------------------------------------------------
 * 故意不用 sprintf：带 %d 会让链接器把 newlib 的格式化部分拉进来，
 * 在 F103 上要多掏好几 KB Flash，而我们只要转个整数。
 * buf 至少 12 字节（"-2147483648" 加结束符）。
 * 返回写入的字符数（不含结束符）。 */
uint8_t lcd_format_int(char *buf, int32_t v);

#endif /* INC_LCD_H_ */