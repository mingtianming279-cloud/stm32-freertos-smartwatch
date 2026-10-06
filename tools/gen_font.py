# -*- coding: utf-8 -*-
"""
生成 8x16 ASCII 点阵字库（C 源文件）

格式约定：每个字符 16 字节，一字节一行、每行 8 像素，MSB 在最左。
这个格式（PC BIOS 经典字形格式）的好处是逐行可直接转成 ST7789 的一行像素。
"""
from PIL import Image, ImageDraw, ImageFont
import os

W, H = 8, 16
FIRST, LAST = 0x20, 0x7E
FONT_PATH = r"C:\Windows\Fonts\consola.ttf"
FONT_SIZE = 14

_font = ImageFont.truetype(FONT_PATH, FONT_SIZE)


def render(ch):
    """把单个字符渲染成 16 字节的点阵（每字节一行，bit7 在最左）"""
    img = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(img)
    l, t, r, b = d.textbbox((0, 0), ch, font=_font)
    # 水平垂直都居中：让字形落在 8x16 格子的正中间，而不是贴左上角
    x = (W - (r - l)) // 2 - l
    y = (H - (b - t)) // 2 - t
    d.text((x, y), ch, font=_font, fill=1)

    rows = []
    for yy in range(H):
        byte = 0
        for xx in range(W):
            if img.getpixel((xx, yy)):
                byte |= 1 << (7 - xx)
        rows.append(byte)
    return rows


# 脚本放在 <项目>/tools/ 下，输出到项目里的 Core/ 目录。
# 用相对定位而不是写死绝对路径，换台机器、换个目录都能跑。
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_C = os.path.join(ROOT, "Core", "Src", "font8x16.c")
OUT_H = os.path.join(ROOT, "Core", "Inc", "font8x16.h")


def glyph_text(rows):
    return ["".join("#" if r & (1 << (7 - i)) else "." for i in range(W)) for r in rows]


def preview():
    for ch in " 018AMZ.-abc":
        print("--- %r ---" % ch)
        for line in glyph_text(render(ch)):
            print(line)


HEADER = """/*
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
"""

SRC_HEAD = """/*
 * font8x16.c
 *
 * 8x16 ASCII 点阵字库 —— ★ 自动生成，不要手改这里 ★
 *
 * 生成脚本：tools/gen_font.py（用 PIL 把 Consolas 14px 渲染成点阵）
 * 要调字形（换字体、换字号、改间距）就改脚本再跑一次；
 * 直接在这里改十六进制字节是维护不了的，下面每个字符都附了点阵图，
 * 对着图看比数字节快得多 —— 这也是为什么把图一起生成出来。
 *
 * 格式：每字符 16 字节，一字节 = 一行 8 像素，bit7 在最左。
 *       索引 = (ch - 0x20) * 16 + 行号。
 *
 * 数据量：95 x 16 = 1520 字节，全在 Flash 里，不占那 20KB RAM。
 */

#include "font8x16.h"

const uint8_t font8x16[FONT8X16_COUNT][FONT8X16_H] = {
"""


def generate():
    lines = [SRC_HEAD]
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        rows = render(ch)
        printable = ch if ch != " " else "space"
        lines.append("  /* 0x%02X '%s'\n" % (code, printable))
        for line in glyph_text(rows):
            lines.append("   * %s\n" % line)
        lines.append("   */\n")
        lines.append("  { " + ", ".join("0x%02X" % b for b in rows) + " },\n")
    lines.append("};\n")

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(lines))

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)

    print("written:", OUT_C)
    print("written:", OUT_H)
    print("glyphs :", LAST - FIRST + 1, "->", (LAST - FIRST + 1) * H, "bytes")


if __name__ == "__main__":
    generate()