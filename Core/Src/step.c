/*
 * step.c
 *
 * 计步器 —— 纯算法实现
 *
 * ── 思路（经典三段式）──────────────────────────────────────────────────
 *   ① 取合加速度幅值 |a|。用合向量而不是单轴，因为手环戴在手腕上、
 *      朝向随时在变，单轴读数跟姿势强相关，换个戴法就不准了。
 *   ② 减掉"重力基线"，得到动态分量。基线用很慢的一阶低通跟随 |a|：
 *      静止时它收敛到 1g（16384），走路时它稳在中间值，于是 |a| 的波动
 *      就被剥出来了 —— 剩下的那个 dyn 才是"步"。
 *   ③ 过阈值 → 判一步。
 *
 * ── 为什么第 ③ 步要五道闸 ─────────────────────────────────────────────
 *   只用一个阈值，一次迈步的波形抖动会被数成好几步，这是计步器最典型的
 *   误计来源。所以：
 *     阈值      —— 决定"多大的波动才算一步"
 *     迟滞      —— 进入用高阈值、退出用低阈值，防止在阈值附近来回跳
 *     最小间隔   —— 再快也不可能超过跑步频率
 *     冲击冷却   —— 一次甩动/磕碰的余波不许被再数一遍（见 STEP_SHOCK_TH）
 *     持续运动检测 —— 一直朝一个方向移动不是步态（见 STEP_DC_MAX）
 *
 *   另外基线那一级还有一个"限速"，它治的是另一个方向的误计：大幅动作把
 *   基线拽走之后、基线慢慢爬回来的那段时间。详见 STEP_BASE_STEP_MAX。
 *
 * ── 怎么调参 ──────────────────────────────────────────────────────────
 *   正常走一段路，看 g_step_dyn_peak（或者 STEP 页面上的 PEAK）：
 *     阈值应该明显低于它，否则会漏计；
 *     同时明显高于静止时的噪声底（静止时 dyn 通常在 ±50 以内），否则会误计。
 *   典型情况：走路时 dyn 峰值在 3000~8000，那 2200 这个默认值是合适的。
 *   如果走一路只计到零头 → 阈值调低；如果静止都在涨 → 阈值调高。
 *
 * ── 已知的局限 ────────────────────────────────────────────────────────
 *   这个算法判的是"有没有一次足够大的冲击"，不是"有没有周期性"。所以
 *   故意甩手腕、或者把表摘下来晃，一样会计数。要根治得做周期性检测
 *   （比如窗内自相关或步频约束），那是下一个量级的工作量，当前不做。
 * ──────────────────────────────────────────────────────────────────── */

#include "step.h"

/* ── 参数：全部以 ±2g 量程（16384 LSB = 1g）为基准 ─────────────────── */
#define STEP_TH_HI          2200    /* 进入阈值 ≈ 0.13 g */
#define STEP_TH_LO           900    /* 退出阈值（迟滞下沿）≈ 0.055 g */
#define STEP_MIN_MS          300u   /* 最小步间隔 → 上限 200 步/分 */

/* 基线低通：2^6 = 64 个采样 ≈ 1.28 s 的时间常数 */
#define STEP_BASE_SHIFT        6u

/*
 * ★ 基线限速：每拍最多变化 ±24（约 0.0015 g）。
 *
 * 这一条是后加的，治的是一个很具体的误计：没有限速时，一次大幅动作会把
 * 基线一起拽高（mag 冲高期间低通在老老实实地跟随），动作结束、mag 回到
 * 1g 时 dyn 就变成一个大负数；而基线要花一秒多才爬回来，**这段时间里任何
 * 一点轻微晃动都会被放大成一步** —— 现象就是"运动结束后又冒出一两步"。
 *
 * 限速之后基线基本钉在 1g 附近，动完就恢复干净。副作用是基线收敛变慢，
 * 但基线本来就该一直是 1g（静止时合加速度恒为 1g，与姿态无关），
 * 所以收敛快慢不影响正确性。
 */
#define STEP_BASE_STEP_MAX    24

/*
 * ★ 大冲击冷却：dyn 高到这个程度，基本可以断定不是一步、而是一次甩动
 *   或者磕碰。它的后半段（减速 + 回弹）紧跟着还会产生第二个峰，那才是
 *   "一次动作被记成两步"的主要来源。
 *
 * 这里刻意不去争论"这一下到底算不算一步"：跑步时峰值也可能很高，一律
 * 不算会大量漏计。所以策略是——照常计它一步，但**把它之后的一小段时间
 * 闭掉**，不让紧随的余波再被数成步。代价是极快的连续步态里可能丢一两步，
 * 而那比"甩一下手多记一步"更容易接受。
 */
#define STEP_SHOCK_TH      15000    /* ≈ 0.92 g */
#define STEP_SHOCK_MS        500u

/*
 * ★ 持续运动检测（"一直朝一个方向移动"时步数一路涨，就是靠这个治的）
 *
 * 那种场景的波形是"斜线 + 振荡"：DC 分量让基线一直追不上，AC 分量（手抖）
 * 让 dyn 在高位反复穿越阈值 —— 每穿一次就复位、再触发一次，于是步数一路涨，
 * 直到那一路加速度饱和、基线终于追上才停。
 *
 * 判据取自步态和它的本质差别：**走路的 mag 在 1g 上下对称波动，dyn 的直流
 * 分量接近 0；而持续单方向运动会让 mag 持续偏向一侧，dyn 就带上了一个直流
 * 分量**。所以只要盯住 dyn 的慢速平均就够了。
 *
 * 时间常数取 2^3 = 8 拍（160 ms）：够快，运动起来一两拍就能识别；又不至于
 * 被单次冲击带偏。实测这个档位下走路一步不少（10 步 → 10 步）。
 */
#define STEP_DC_SHIFT         3
#define STEP_DC_MAX        1500     /* ≈ 0.09 g 的持续偏置 */

/* ── 调参观测量 ─────────────────────────────────────────────────────── */
volatile int32_t  g_step_mag      = 0;
volatile int32_t  g_step_dyn      = 0;
volatile int32_t  g_step_base     = 0;
volatile int32_t  g_step_dyn_peak = 0;
volatile uint32_t g_step_reject   = 0u;
volatile uint32_t g_step_shock    = 0u;
volatile int32_t  g_step_dc       = 0;
volatile uint32_t g_step_hold     = 0u;

/* ── 内部状态 ───────────────────────────────────────────────────────── */
static uint32_t s_steps;
static int32_t  s_base;          /* 重力基线 */
static uint8_t  s_armed;         /* 1 = 已计过一步，在等 dyn 落回去 */
static uint32_t s_last_step_ms;
static uint32_t s_now_ms;
static uint32_t s_shock_until;   /* 大冲击冷却的截止时刻 */
static int32_t  s_dc;            /* dyn 的直流分量估计 */

/*
 * 整数平方根。
 *
 * 为什么不用 sqrtf：F103 没有 FPU，sqrtf 是软浮点实现，会往固件里拉进
 * 浮点库。而这个项目的原则是"能用整数就别动浮点"（lcd_format_int 也是
 * 同一个理由）。20 ms 才跑一次，整数 Newton 迭代的开销完全无所谓。
 *
 * 初始猜测取 v 的"半位长"（即 2^(bits/2)），它已经和真值同量级，
 * 所以迭代 2~3 次就收敛 —— 比从 v/2 起步快一个数量级。
 */
static uint32_t isqrt32(uint32_t v)
{
	uint32_t x, y, bits, t;

	if (v == 0u) return 0u;

	bits = 0u;
	t = v;
	while (t != 0u) { t >>= 1; bits++; }
	x = 1u << ((bits + 1u) >> 1u);

	for (;;)                        /* Newton: x ← (x + v/x) / 2 */
	{
		y = (x + (v / x)) >> 1u;
		if (y >= x) break;
		x = y;
	}
	return x;
}

void step_reset(void)
{
	s_steps        = 0u;
	s_base         = 16384;         /* 静止时合加速度就是 1g */
	s_armed        = 0u;
	s_last_step_ms = 0u;
	/* 从 STEP_MIN_MS 起步，让第一步不受最小间隔限制 */
	s_now_ms       = STEP_MIN_MS;
	s_shock_until  = 0u;

	g_step_mag      = 0;
	g_step_dyn      = 0;
	g_step_base     = s_base;
	g_step_dyn_peak = 0;
	g_step_reject   = 0u;
	g_step_shock    = 0u;
	g_step_dc       = 0;
	g_step_hold     = 0u;

	s_dc   = 0;
}

void step_update(int16_t ax, int16_t ay, int16_t az, uint16_t dt_ms)
{
	uint32_t sq;
	int32_t  mag, dyn, adyn;

	s_now_ms += dt_ms;

	/* ① 合加速度幅值。先平方求和再开方，避免为开方单独做浮点。
	 *    平方和最大到 3 × 32768² ≈ 3.2e9，int32 装不下，所以用 uint32。 */
	sq = (uint32_t)((int32_t)ax * (int32_t)ax)
	   + (uint32_t)((int32_t)ay * (int32_t)ay)
	   + (uint32_t)((int32_t)az * (int32_t)az);
	mag = (int32_t)isqrt32(sq);

	/* ② 重力基线：一阶低通 + 限速（限速的理由见文件开头 STEP_BASE_STEP_MAX）。
	 *    用 >> 而不是 / 是因为除数是 2 的幂，移位快得多。代价是有一个
	 *    ±63 的死区（差值小于 64 时这一拍不更新），但走路时的动态分量
	 *    是几千量级，这点死区可以忽略。 */
	{
		int32_t d = (mag - s_base) >> STEP_BASE_SHIFT;
		if (d >  STEP_BASE_STEP_MAX) d =  STEP_BASE_STEP_MAX;
		if (d < -STEP_BASE_STEP_MAX) d = -STEP_BASE_STEP_MAX;
		s_base += d;
	}
	dyn = mag - s_base;

	g_step_mag  = mag;
	g_step_dyn  = dyn;
	g_step_base = s_base;

	/* 峰值取绝对值：负向的波动同样说明"在动"，看幅度比看符号有用 */
	adyn = (dyn < 0) ? -dyn : dyn;
	if (adyn > g_step_dyn_peak) g_step_dyn_peak = adyn;

	/* ③ 持续运动检测（见 STEP_DC_MAX 的说明）。
	 *    dyn 的慢速平均，反映的是"mag 有没有持续偏向一侧"：走路时它贴着 0，
	 *    持续单方向运动时它会堆起来。一旦判到，就把状态机**按住**而不是简单
	 *    跳过判步 —— 按住是为了让运动结束之后也不会立刻补一步：得等 dyn
	 *    真正回落到迟滞下沿以下，才重新开始判步。 */
	s_dc += (dyn - s_dc) >> STEP_DC_SHIFT;
	g_step_dc = s_dc;

	if (s_dc > STEP_DC_MAX)
	{
		s_armed = 1u;
		g_step_hold++;
	}

	/* ④ 四道闸 */
	if (s_armed == 0u)
	{
		if (dyn > STEP_TH_HI)
		{
			if (s_now_ms < s_shock_until)
			{
				/* 刚经历过大冲击，这多半是它的余波、不是新的一步 */
				g_step_shock++;
			}
			else if ((s_now_ms - s_last_step_ms) >= STEP_MIN_MS)
			{
				s_steps++;
				s_last_step_ms = s_now_ms;
				s_armed = 1u;
			}
			else
			{
				/* 挡掉的这次不丢弃、只记一笔：如果这个数在快速上涨，
				 * 说明阈值偏低或者采样里噪声太大。 */
				g_step_reject++;
			}
		}
	}
	else
	{
		/* 必须落回迟滞下沿以下才允许计下一步 */
		if (dyn < STEP_TH_LO)
		{
			s_armed = 0u;
		}
	}

	/* ④ 大冲击 → 进入冷却期。
	 *    ★ 顺序很关键：必须放在判步**之后**。
	 *    放前面的话，冷却会在同一拍里把自己一起关掉 —— 刚设成 now+500，
	 *    紧接着的判断 `now < shock_until` 就成立，于是连大冲击本身都计不上。
	 *    而跑步的峰值完全可能超过这个阈值，那就会整段整段地漏计。
	 *    放在后面，被拦住的才是纯粹的余波。 */
	if (dyn > STEP_SHOCK_TH)
	{
		s_shock_until = s_now_ms + STEP_SHOCK_MS;
	}
}

uint32_t step_count(void)
{
	return s_steps;
}