# -*- coding: utf-8 -*-
"""
把 Core/Src 下实际存在的源文件同步进 CubeIDE 的构建清单。

⚠ 关键：CubeIDE 用**两个地方**记录参与构建的文件，两个都要改
    Debug/Core/Src/subdir.mk   —— C_SRCS / OBJS / C_DEPS，$(OBJS) 用作依赖
    Debug/objects.list         —— ★ 链接命令真正读的那个 ★

  只改 subdir.mk 的话，新文件会被编译出来、却进不了链接，报一串
  undefined reference，而且看起来像"函数没写" —— 排查方向完全是错的。

平时这些文件由 Eclipse 在构建时自动重建；这个脚本是给命令行 make 用的
（不依赖 IDE 刷新）。两个配置都重建，因为 Release 那份曾经过时得比较厉害
—— 连 mpu6050.c / spi.c 都没列进去，靠增删条目补不齐，只能按事实重建。

只动源文件列表和 clean 规则，编译选项、包含路径一律不碰。
"""
import os
import re

# 脚本放在 <项目>/tools/ 下，项目根就是它的上一级
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRCDIR = os.path.join(ROOT, "Core", "Src")


def core_files():
    names = [f for f in os.listdir(SRCDIR) if f.endswith(".c")]
    names.sort()
    return names


def fix_objects_list(path, names):
    """重建 objects.list 里 Core/Src 那一段。

    这个文件容易被忽略：链接命令是 `arm-none-eabi-gcc -o smart_watch.elf
    @"objects.list"`，也就是说**真正决定链接哪些 .o 的是它，不是 subdir.mk
    里的 $(OBJS)**（$(OBJS) 只作为依赖用于触发重建）。
    所以新增源文件时两个地方都得改 —— 只改 subdir.mk 的话，新文件会被编译
    出来、却进不了链接，报一串 undefined reference。

    非 Core/Src 的条目（Drivers / Middlewares / Startup）一律原样保留。
    """
    with open(path, "r", encoding="utf-8", newline="") as f:
        raw = f.read()
    crlf = "\r\n" in raw
    lines = raw.replace("\r\n", "\n").split("\n")

    core_marker = "./Core/Src/"
    kept = [ln for ln in lines if ln.strip() and core_marker not in ln]
    new_core = ['"%s%s.o"' % (core_marker, n[:-2]) for n in names]

    # Core/Src 原本排在最前面，保持这个位置
    body = new_core + kept
    out = "\n".join(body) + "\n"
    if crlf:
        out = out.replace("\n", "\r\n")

    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    return len(body)


def make_list(var, prefix, names):
    """生成 `VAR += \\` 风格的列表；最后一条按 CubeIDE 的惯例以空格收尾。"""
    out = [var + " += \\"]
    for i, n in enumerate(names):
        tail = " \\" if i < len(names) - 1 else " "
        out.append(prefix + n + tail)
    return "\n".join(out) + "\n"


def fix(path, names):
    with open(path, "r", encoding="utf-8", newline="") as f:
        raw = f.read()

    before = len(raw)

    # 文件是 CRLF 的，而下面的正则按 LF 写。先在内存里统一成 LF 处理，
    # 写回时再还原 —— 否则 ^...$ 这类锚点会因为多出来的 \r 匹配不上。
    crlf = "\r\n" in raw
    s = raw.replace("\r\n", "\n") if crlf else raw

    for var, prefix, suffix in (
        ("C_SRCS", "../Core/Src/", ""),
        ("OBJS", "./Core/Src/", ".o"),
        ("C_DEPS", "./Core/Src/", ".d"),
    ):
        # names 形如 "lcd.c"。C_SRCS 保留扩展名；OBJS/C_DEPS 是把扩展名
        # **换掉**，不是接在后面 —— 直接拼会得到 "lcd.c.o"，于是 make 会去找
        # 并不存在的 lcd.c.c，报 "No rule to make target"。
        items = [n if suffix == "" else n[:-2] + suffix for n in names]
        block = make_list(var, prefix, items)
        pattern = r"(?s)^%s \+= \\\n.*?(?=\n\n)" % re.escape(var)
        s, cnt = re.subn(pattern, block.rstrip("\n"), s, count=1, flags=re.M)
        if cnt != 1:
            raise RuntimeError("没找到 %s 块：%s" % (var, path))

    # clean 规则：每个文件四个产物，顺序照 CubeIDE 的惯例 cyclo/d/o/su
    items = []
    for n in names:
        stem = n[:-2]
        for ext in ("cyclo", "d", "o", "su"):
            items.append("./Core/Src/%s.%s" % (stem, ext))
    clean = "\t-$(RM) " + " ".join(items)
    s, cnt = re.subn(r"(?m)^\t-\$\(RM\) .*$", lambda m: clean, s, count=1)
    if cnt != 1:
        raise RuntimeError("没找到 clean 规则：%s" % path)

    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(s.replace("\n", "\r\n") if crlf else s)

    return before, len(s)


if __name__ == "__main__":
    names = core_files()
    print("磁盘上的源文件 (%d):" % len(names))
    for n in names:
        print("   ", n)

    for cfg in ("Debug", "Release"):
        p = os.path.join(ROOT, cfg, "Core", "Src", "subdir.mk")
        if not os.path.exists(p):
            print("skip (not found):", p)
            continue
        a, b = fix(p, names)
        print("updated %s  (%d -> %d chars)" % (p, a, b))

        ol = os.path.join(ROOT, cfg, "objects.list")
        if os.path.exists(ol):
            cnt = fix_objects_list(ol, names)
            print("updated %s  (%d entries)" % (ol, cnt))
        else:
            print("skip (not found):", ol)