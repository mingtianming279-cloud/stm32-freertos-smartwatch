/*
 * font8x16.h
 *
 * 8x16 ASCII 点阵字库（0x20 空格 ~ 0x7E '~'，共 95 个字符）—— 对外接口
 *
 * 【为什么只有这一套字号】
 *   8 像素宽的格子最多容纳 14px 的等宽字体，再大就会被裁边。所以不做多套
 *   字号，而是在 lcd 层提供整数倍放大（scale=2 就是 16x32），大字小字共用
 *   同一份字库 —— Flash 里只存一份，显示大小按需放。
 */

#ifndef INC_FONT8X16_H_
#define INC_FONT8X16_H_

#include <stdint.h>

#define FONT8X16_W      8u
#define FONT8X16_H      16u
#define FONT8X16_FIRST  0x20u
#define FONT8X16_LAST   0x7Eu
#define FONT8X16_COUNT  (FONT8X16_LAST - FONT8X16_FIRST + 1u)

/* 用法：font8x16[ch - FONT8X16_FIRST][行号]
 *       每个字符 16 字节，一字节 = 该行的 8 个像素，bit7 在最左。
 *       换算成 ST7789 的像素就是：bit 为 1 取前景色，否则取背景色。 */
extern const uint8_t font8x16[FONT8X16_COUNT][FONT8X16_H];

#endif /* INC_FONT8X16_H_ */
