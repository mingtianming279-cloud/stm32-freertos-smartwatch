/*
 * lcd.c
 *
 * 显示抽象层的 ST7789 实现
 *
 * 这一层的职责：把"字符""字符串""矩形"翻译成 ST7789 能吃的像素流。
 * 上层只认 lcd.h，所以哪天换屏了，改的是这个文件，不是任务代码。
 */

#include "lcd.h"
#include "st7789.h"
#include "font8x16.h"

/* 尺寸必须和硬件层一致。用编译期检查挡住"改了 st7789.h 忘了改 lcd.h"——
 * 这种事一旦漏掉，症状是画面右下角被悄悄切掉，不会报任何错。 */
#if (LCD_W != ST7789_W) || (LCD_H != ST7789_H)
#error "lcd.h 的 LCD_W/LCD_H 与 st7789.h 的 ST7789_W/ST7789_H 不一致"
#endif

/* 放大倍数上限。取 4 是因为：字符一行 8 像素，放大 4 倍就是一行的 64 字节，
 * 而整块字符缓冲（16 行 x 32 像素 x 2 字节）要 1024 字节 —— 我们不需要它。
 * 放大无非是"把这一行重复发 scale 次"，所以只需要一行就够。 */
#define MAX_SCALE  4u
static uint8_t s_row[FONT8X16_W * MAX_SCALE * 2u];

void lcd_init(void)
{
	st7789_init();
}

void lcd_clear(uint16_t color)
{
	st7789_fill_screen(color);
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	st7789_fill_rect(x, y, w, h, color);
}

/* 把字库里的一行（8 个 bit）铺成 scale 倍宽的一行像素，写进 s_row。
 * bit7 对应最左边的像素 —— 这个约定来自生成字库的脚本，换字库时别改反。 */
static void lcd_pack_row(uint8_t bits, uint16_t fg, uint16_t bg, uint8_t scale)
{
	uint8_t fgh = (uint8_t)(fg >> 8);
	uint8_t fgl = (uint8_t)(fg & 0xFFu);
	uint8_t bgh = (uint8_t)(bg >> 8);
	uint8_t bgl = (uint8_t)(bg & 0xFFu);
	uint16_t i = 0u;

	for (uint8_t b = 0u; b < FONT8X16_W; b++)
	{
		uint8_t on = (uint8_t)((bits >> (7u - b)) & 0x01u);
		uint8_t hi = on ? fgh : bgh;
		uint8_t lo = on ? fgl : bgl;

		for (uint8_t s = 0u; s < scale; s++)
		{
			s_row[i++] = hi;
			s_row[i++] = lo;
		}
	}
}

void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
	if (scale == 0u) return;
	if (scale > (uint8_t)MAX_SCALE) scale = (uint8_t)MAX_SCALE;

	/* 字库外的字符一律画成 '?'：宁可显示一个看得见的问号，
	 * 也不要静默丢弃 —— 静默丢弃会让人怀疑是绘制链路坏了。 */
	uint8_t uc = (uint8_t)c;
	if (uc < FONT8X16_FIRST || uc > FONT8X16_LAST) uc = (uint8_t)'?';

	const uint8_t *glyph = font8x16[uc - FONT8X16_FIRST];
	uint16_t cw = (uint16_t)(FONT8X16_W * scale);
	uint16_t ch = (uint16_t)(FONT8X16_H * scale);

	/* 没有 framebuffer，就没法"先画到画布上再裁剪"，所以只支持整块在屏内 */
	if (x >= LCD_W || y >= LCD_H) return;
	if ((uint32_t)x + cw > LCD_W) return;
	if ((uint32_t)y + ch > LCD_H) return;

	st7789_blit_begin(x, y, (uint16_t)(x + cw - 1u), (uint16_t)(y + ch - 1u));
	for (uint8_t r = 0u; r < FONT8X16_H; r++)
	{
		lcd_pack_row(glyph[r], fg, bg, scale);
		for (uint8_t s = 0u; s < scale; s++)   /* 垂直放大：同一行发 scale 次 */
		{
			st7789_blit_data(s_row, (uint16_t)(cw * 2u));
		}
	}
	st7789_blit_end();
}

void lcd_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
	if (scale == 0u) scale = 1u;
	if (scale > (uint8_t)MAX_SCALE) scale = (uint8_t)MAX_SCALE;

	uint16_t step = (uint16_t)(FONT8X16_W * scale);

	while (*s != '\0')
	{
		/* 先判断再画：字符不完整就不画，避免画出一半然后被剪掉 */
		if ((uint32_t)x + step > LCD_W) break;

		lcd_draw_char(x, y, *s, fg, bg, scale);
		x = (uint16_t)(x + step);
		s++;
	}
}

uint16_t lcd_str_width(const char *s, uint8_t scale)
{
	if (scale == 0u) scale = 1u;
	if (scale > (uint8_t)MAX_SCALE) scale = (uint8_t)MAX_SCALE;

	uint16_t n = 0u;
	while (*s++ != '\0') n++;
	return (uint16_t)(n * FONT8X16_W * scale);
}

uint8_t lcd_format_int(char *buf, int32_t v)
{
	char tmp[12];
	uint8_t n = 0u;
	uint8_t i = 0u;
	uint32_t u;

	if (v < 0)
	{
		buf[n++] = '-';
		/* 走 64 位再取反：直接写 -v 遇到 INT32_MIN 会溢出成负数 */
		u = (uint32_t)(-(int64_t)v);
	}
	else
	{
		u = (uint32_t)v;
	}

	/* 从低位往高位取，所以先倒着存进 tmp 再翻回来 */
	do
	{
		tmp[i++] = (char)('0' + (u % 10u));
		u /= 10u;
	} while (u != 0u);

	while (i > 0u)
	{
		buf[n++] = tmp[--i];
	}
	buf[n] = '\0';

	return n;
}