# -*- coding: utf-8 -*-
"""
生成"按需汉字"点阵字库（16x16）。

为什么不整 GB2312：6763 个汉字 x 32 字节 = 216 KB，而 F103C8 总共才 64 KB
Flash、已经用掉 43 KB。所以只能挑真正用到的十几个字生成子集 —— 十几字
几百字节，随便放。

数据格式与 font8x16 保持一致的思路：每个字 32 字节（16 行 x 每行 2 字节），
bit15 在最左。这样在 lcd 层里可以复用同样的"逐行铺像素"逻辑。

★ 查找方式：字库里同时存 UTF-8 原文，运行时按字符串比对查找。
  好处是调用处可以写 lcd_draw_hz_str(x, y, "步数", ...) 这样的可读形式，
  而不用记一堆 HZ_BU / HZ_SHU 枚举名。
"""
from PIL import Image, ImageDraw, ImageFont
import os

W, H = 16, 16
BYTES_PER_GLYPH = 32        # 16 行 x 2 字节
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",     # 黑体，笔画均匀，小字号下最稳
    r"C:\Windows\Fonts\msyh.ttc",       # 微软雅黑
    r"C:\Windows\Fonts\simsun.ttc",     # 宋体
]
FONT_SIZE = 15

# 只放真正要用的字。加字就加到这里，重新跑脚本即可。
HANZI = "步数三轴系统时间返回确认主页清零秒星期日一二三四五六菜单再按"

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_C = os.path.join(ROOT, "Core", "Src", "hz16.c")
OUT_H = os.path.join(ROOT, "Core", "Inc", "hz16.h")


def load_font():
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return ImageFont.truetype(p, FONT_SIZE), p
    raise RuntimeError("找不到可用的中文字体")


def render(ch, font):
    """渲染成 32 字节：16 行，每行 2 字节，bit15 在最左"""
    img = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(img)
    l, t, r, b = d.textbbox((0, 0), ch, font=font)
    x = (W - (r - l)) // 2 - l
    y = (H - (b - t)) // 2 - t
    d.text((x, y), ch, font=font, fill=1)

    rows = []
    for yy in range(H):
        hi = 0
        lo = 0
        for xx in range(W):
            if img.getpixel((xx, yy)):
                if xx < 8:
                    hi |= 1 << (7 - xx)
                else:
                    lo |= 1 << (15 - xx)
        rows.append((hi, lo))
    return rows


def glyph_text(rows):
    out = []
    for (hi, lo) in rows:
        v = (hi << 8) | lo
        out.append("".join("#" if (v >> (15 - i)) & 1 else "." for i in range(W)))
    return out


HEADER = """/*
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
\tconst char *utf8;             /* 该字的 UTF-8 原文（3 字节 + 结束符） */
\tconst uint8_t bitmap[HZ16_BYTES];
} hz16_glyph_t;

extern const hz16_glyph_t hz16_table[];
extern const uint16_t     hz16_count;

/* 按 UTF-8 字符查点阵；查不到返回 0（调用方画成空白） */
const hz16_glyph_t *hz16_lookup(const char *utf8);

#endif /* INC_HZ16_H_ */
"""

SRC_HEAD = """/*
 * hz16.c
 *
 * 按需汉字点阵字库 —— ★ 自动生成，不要手改 ★
 *
 * 生成脚本：tools/gen_hz.py
 * 每个字 32 字节 = 16 行，每行 2 字节，bit15 在最左。
 * 下面每个字都附了点阵图，对照着看比数十六进制快得多。
 */

#include "hz16.h"

const hz16_glyph_t hz16_table[] = {
"""


def main():
    font, path = load_font()
    print("字体:", path, " 大小:", FONT_SIZE)
    print("汉字数:", len(HANZI), "->", len(HANZI) * BYTES_PER_GLYPH, "字节")

    lines = [SRC_HEAD]
    for ch in HANZI:
        rows = render(ch, font)
        lines.append("  { /* %s */\n" % ch)
        lines.append('    "%s",\n' % ch)
        lines.append("    {\n")
        # 每行两个字节，排成 8 行 4 个
        for i in range(0, H, 2):
            hi = rows[i][0]
            lo = rows[i][1]
            hi2 = rows[i + 1][0]
            lo2 = rows[i + 1][1]
            lines.append("      0x%02X, 0x%02X, 0x%02X, 0x%02X,\n" % (hi, lo, hi2, lo2))
        lines.append("    }\n")
        lines.append("    /*\n")
        for t in glyph_text(rows):
            lines.append("      %s\n" % t)
        lines.append("    */\n")
        lines.append("  },\n")
    lines.append("};\n\n")
    lines.append("const uint16_t hz16_count = %d;\n\n" % len(HANZI))
    lines.append("""const hz16_glyph_t *hz16_lookup(const char *utf8)
{
\tuint16_t i;
\tif ((utf8 == 0) || (utf8[0] == '\\0')) return 0;

\tfor (i = 0u; i < hz16_count; i++)
\t{
\t\tconst char *p = hz16_table[i].utf8;
\t\tif ((p[0] == utf8[0]) && (p[1] == utf8[1]) && (p[2] == utf8[2]))
\t\t{
\t\t\treturn &hz16_table[i];
\t\t}
\t}
\treturn 0;
}
""")

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(lines))
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)

    print("written:", OUT_C)
    print("written:", OUT_H)

    # 预览前几个字
    print()
    for ch in HANZI[:4]:
        print("--- %s ---" % ch)
        for t in glyph_text(render(ch, font)):
            print("  " + t)


if __name__ == "__main__":
    main()