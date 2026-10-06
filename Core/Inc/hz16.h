/*
 * hz16.h
 *
 * 按需汉字点阵字库（16x16）—— 对外接口
 *
 * ★ 自动生成，不要手改 ★
 *   生成脚本：tools/gen_hz.py
 *
 * 为什么只有这几个字：整 GB2312 是 6763 字 x 32 字节 = 216 KB，而本芯片
 * 一共 64 KB Flash、已用掉 43 KB。所以这里只放界面真正用到的字，加字就改
 * 脚本里的 HANZI 再跑一遍。
 *
 * 为什么存 UTF-8 原文而不是用枚举：调用处可以写成
 *     lcd_draw_hz_str(x, y, "步数", fg, bg, 2);
 * 这样人一眼能看懂，不用去翻 HZ_BU / HZ_SHU 这些枚举名。代价是查找要从
 * 字符串比对，但那是一条十几项的线性表，比维护一套枚举省心得多。
 */

#ifndef INC_HZ16_H_
#define INC_HZ16_H_

#include <stdint.h>

#define HZ16_W        16u
#define HZ16_H        16u
#define HZ16_BYTES    32u      /* 16 行 x 2 字节 */

typedef struct {
	const char *utf8;             /* 该字的 UTF-8 原文（3 字节 + 结束符） */
	const uint8_t bitmap[HZ16_BYTES];
} hz16_glyph_t;

extern const hz16_glyph_t hz16_table[];
extern const uint16_t     hz16_count;

/* 按 UTF-8 字符查点阵；查不到返回 0（调用方画成空白） */
const hz16_glyph_t *hz16_lookup(const char *utf8);

#endif /* INC_HZ16_H_ */
