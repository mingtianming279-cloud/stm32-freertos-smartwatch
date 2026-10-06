# -*- coding: utf-8 -*-
"""
对拍 step.c 的算法：构造典型波形，比较"加限速/冷却之前"和"之后"各计了多少步。

波形直接给出合加速度 mag（单位同固件：16384 = 1g），因为 step.c 的全部判断
都建立在 mag 上，三段式里的第①步（求模）不参与决策。

Python 的 >> 对负数同样是算术右移（向下取整），与 GCC 一致，所以可以逐行
照着 C 抄。
"""
import math
import random

G = 16384          # 1g
TH_HI = 2200
TH_LO = 900
MIN_MS = 300
BASE_SHIFT = 6
BASE_STEP_MAX = 24
SHOCK_TH = 15000
SHOCK_MS = 500
DC_SHIFT = 3
DC_MAX = 1500


class Pedometer:
    def __init__(self, base_limit, shock, dc_max=None):
        self.base_limit = base_limit   # 基线限速
        self.shock = shock             # 大冲击冷却
        self.dc_max = dc_max           # 持续运动检测
        self.steps = 0
        self.base = G
        self.armed = 0
        self.last_step = 0
        self.now = MIN_MS              # 与 step_reset() 一致
        self.shock_until = 0
        self.peak = 0
        self.reject = 0
        self.shock_cnt = 0
        self.min_base = G
        self.max_base = G
        self.dc = 0

    def update(self, mag, dt=20):
        self.now += dt
        d = (mag - self.base) >> BASE_SHIFT
        if self.base_limit:
            if d > BASE_STEP_MAX:
                d = BASE_STEP_MAX
            if d < -BASE_STEP_MAX:
                d = -BASE_STEP_MAX
        self.base += d
        dyn = mag - self.base

        self.min_base = min(self.min_base, self.base)
        self.max_base = max(self.max_base, self.base)
        if abs(dyn) > self.peak:
            self.peak = abs(dyn)

        # 持续运动检测（与 step.c 一致）：dyn 的慢速平均，走路时贴 0，
        # 一直朝一个方向移动时堆起来。
        self.dc += (dyn - self.dc) >> DC_SHIFT
        if self.dc_max is not None and self.dc > self.dc_max:
            self.armed = 1

        if not self.armed:
            if dyn > TH_HI:
                if self.shock and self.now < self.shock_until:
                    self.shock_cnt += 1
                elif self.now - self.last_step >= MIN_MS:
                    self.steps += 1
                    self.last_step = self.now
                    self.armed = 1
                else:
                    self.reject += 1
        else:
            if dyn < TH_LO:
                self.armed = 0

        # 冷却在判步之后设置（与 step.c 一致）：否则同一拍里冷却会把自己
        # 也关掉，连大冲击本身都计不上。
        if dyn > SHOCK_TH:
            self.shock_until = self.now + SHOCK_MS


def run(wave, base_limit, shock, dc_max=None):
    p = Pedometer(base_limit, shock, dc_max)
    for m in wave:
        p.update(m)
    return p


def wave_walk(dur_s=6.0, period_s=0.6, amp=4000):
    n = int(dur_s / 0.02)
    return [int(G + amp * math.sin(2 * math.pi * i * 0.02 / period_s)) for i in range(n)]


def wave_flick(dur_s=2.5):
    """单次甩手：加速峰 -> 减速谷 -> 衰减余波"""
    out = []
    for i in range(int(dur_s / 0.02)):
        t = i * 0.02
        if t < 0.15:
            mag = G + int(25000 * math.sin(math.pi * t / 0.15))
        elif t < 0.35:
            u = (t - 0.15) / 0.20
            mag = G - int(10000 * math.sin(math.pi * u))
        else:
            u = t - 0.35
            mag = G + int(3000 * math.exp(-u * 4.0) * math.sin(2 * math.pi * u / 0.45))
        out.append(max(0, mag))
    return out


def wave_shift_shake(dur_s=8.0, dc_span=14000, dc_ramp=4.0, ac=6000, period=0.6):
    """持续单方向移动：斜线（DC）+ 手抖（AC）。这就是"步数一路涨"的波形。"""
    out = []
    for i in range(int(dur_s / 0.02)):
        t = i * 0.02
        dc = dc_span * min(1.0, t / dc_ramp)
        ac_v = ac * math.sin(2 * math.pi * t / period)
        out.append(int(max(0, min(32768, G + dc + ac_v))))
    return out


def wave_still(dur_s=4.0, noise=60):
    random.seed(7)
    return [G + random.randint(-noise, noise) for _ in range(int(dur_s / 0.02))]


def wave_double_flick(dur_s=2.5):
    """甩手折返：加速峰之后 300ms 又有一个减速峰 —— 这正是"多记一步"的波形"""
    out = []
    for i in range(int(dur_s / 0.02)):
        t = i * 0.02
        if t < 0.12:
            mag = G + int(20000 * math.sin(math.pi * t / 0.12))
        elif t < 0.30:
            mag = G
        elif t < 0.45:
            mag = G + int(12000 * math.sin(math.pi * (t - 0.30) / 0.15))
        else:
            mag = G
        out.append(max(0, mag))
    return out


def wave_dip_only(dur_s=3.5, small_amp=1500):
    """★ 单向：一段持续的低加速度（mag 明显低于 1g），把基线朝一个方向拉走，
    之后恢复期的轻微波动就被抬过了阈值 —— 这才是"运动结束后多记一步"。

    注意必须用**单向**的波形来测：如果用"加速峰 + 减速谷"那种对称波形，
    两边对基线的拉扯会互相抵消，净偏差接近零，什么也测不出来（试过，
    改前改后完全一样，白忙一场）。
    """
    out = []
    for i in range(int(dur_s / 0.02)):
        t = i * 0.02
        if 0.20 < t < 1.00:
            mag = G - 6000                      # 持续的低加速度
        elif 1.20 < t < 1.90:
            mag = G + int(small_amp * math.sin(math.pi * (t - 1.20) / 0.70))
        else:
            mag = G
        out.append(max(0, mag))
    return out


CASES = [
    ("walk 6s @0.6s/step  (expect ~10)", wave_walk(), 10),
    ("walk 6s @1.0s/step  (expect ~6)", wave_walk(6.0, 1.0, 3500), 6),
    ("single flick        (expect 1, NOT more)", wave_flick(), 1),
    ("double-peak flick   (expect 1, NOT 2)", wave_double_flick(), 1),
    ("one-way dip + small  (expect 0: A<thr)", wave_dip_only(), 0),
    ("one-way shift + shake (expect few)", wave_shift_shake(), 3),
    ("still 4s + noise    (expect 0)", wave_still(), 0),
]

print("=" * 72)
print("%-42s %8s %8s" % ("waveform", "before", "after"))
print("-" * 72)
for name, wave, expect in CASES:
    a = run(wave, base_limit=False, shock=False)
    b = run(wave, base_limit=True, shock=True, dc_max=DC_MAX)
    print("%-42s %8d %8d" % (name, a.steps, b.steps))
print("=" * 72)

print()
print("baseline drift (max deviation from 1g; smaller = steadier):")
for name, wave, expect in CASES[:6]:
    a = run(wave, base_limit=False, shock=False)
    b = run(wave, base_limit=True, shock=True)
    da = max(abs(a.max_base - G), abs(a.min_base - G))
    db = max(abs(b.max_base - G), abs(b.min_base - G))
    print("  %-40s before %6d   after %6d" % (name, da, db))

print()
print("sweep: one-way dip, then a small wiggle of amplitude A.")
print("A below 2200 should NEVER be counted -- only baseline drift can fake it.")
print("  %-12s %8s %8s" % ("A", "before", "after"))
for amp in range(400, 2401, 200):
    w = wave_dip_only(small_amp=amp)
    a = run(w, base_limit=False, shock=False)
    b = run(w, base_limit=True, shock=True)
    mark = "" if amp >= 2200 else "   <-- must be 1"
    print("  %-12d %8d %8d%s" % (amp, a.steps, b.steps, mark))