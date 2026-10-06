/*
 * rtc_time.c
 *
 * 时间子系统实现
 *
 * ── 三件事 ────────────────────────────────────────────────────────────
 *   ① 选时钟源：上电先试 LSE（外部 32.768 kHz 晶振），带 300 ms 超时；
 *      试不通就回退 LSI。选到哪个会写进 g_rtc_src，INFO 页直接显示。
 *   ② 保住软复位后时间不跳：看门狗复位是常态，那种情况下备份域还活着、
 *      RTC 秒数还是对的，**绝不能用编译时刻把它冲掉**。
 *   ③ 日历换算：F103 的 RTC 只是个 32 位秒计数器，没有 BCD 日历，
 *      年月日得自己算。
 *
 * ── 为什么不用 HAL 的 RTC 驱动 ────────────────────────────────────────
 *   两个原因。一是 HAL_RCC_OscConfig 等 LSE 起振用的是 LSE_TIMEOUT_VALUE
 *   （5 秒），板上没晶振的话每次上电都要干等 5 秒；自己写就能压到 300 ms。
 *   二是 HAL 的 RTC 初始化会顺手把计数器清零，而我们要在"备份域还活着"
 *   的时候保留原值。直接操作 BDCR / CRL / PRL / CNT 这几个寄存器反而更短、
 *   更可控。
 * -------------------------------------------------------------------- */

#include "rtc_time.h"
#include "main.h"

/* 等 LSE 起振多久算失败。晶振正常的起振时间在几百毫秒量级，
 * 300 ms 是个折中：够真晶振起来，又不至于没晶振时干等太久。 */
#define RTC_LSE_PROBE_MS    300u
#define RTC_LSI_PROBE_MS    100u
/* 等 RSF / RTOFF 这类"寄存器同步"标志：它们都在几个 RTCCLK 周期内完成，
 * 50 ms 已经是极大的余量。给这么短不是抠，是要让"硬件没反应"尽快暴露成
 * 超时而不是死等。 */
#define RTC_SYNC_MS          50u

#define RTC_PRESC_LSE     32767u    /* 32768 Hz ÷ (32767+1) = 1 Hz */
#define RTC_PRESC_LSI     39999u    /* 约 40 kHz ÷ (39999+1) = 1 Hz */

/* 秒数低于这个值就认为 RTC 里的值不可信，重新置初值。
 * 取 2020-01-01：这是个手环，时间不可能比它更早。 */
#define RTC_EPOCH_MIN   1577836800u

volatile uint32_t g_rtc_src   = 0u;
volatile uint32_t g_rtc_epoch = 0u;
volatile uint32_t g_rtc_reset = 0u;
volatile uint32_t g_rtc_build = 0u;
volatile uint32_t g_rtc_retry = 0u;

/*
 * 带超时地等某个位置起。返回 1 = 等到了，0 = 超时。
 *
 * ★ 所有等硬件标志的地方都必须走这里。启动路径上没有任何东西兜底：
 *   MX_IWDG_Init() 排在 rtc_time_init() 之后，看门狗还没武装，
 *   一旦死等就是"上电卡死、进不了系统"，连拔电重启都救不回来。
 *
 * ★ 这里除了用 HAL_GetTick() 计时，还额外加了一个纯循环计数兜底 ——
 *   如果 tick 因为中断没开而根本不增，光看它会永远睡不着。
 */
static uint8_t rtc_wait_flag(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
	uint32_t t0   = HAL_GetTick();
	uint32_t spin = 0u;

	while ((*reg & mask) == 0u)
	{
		if ((HAL_GetTick() - t0) >= ms) return 0u;
		if (++spin > 10000000u)         return 0u;
	}
	return 1u;
}

/* ==================================================================
 * 日历换算（Howard Hinnant 的 days_from_civil / civil_from_days）
 *
 * 抄的时候一个符号都不能改：这套算法没有查表、没有分支，闰年规则被编进
 * era / yoe / doe 三步算术里，对了是"巧合不了的那种对"，错了也不会崩，
 * 只会让日期系统性地偏 —— 所以它被单独拿去和 Python 对拍验证过。
 * ================================================================== */

/* 公历 → 自 1970-01-01 起的天数 */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
	int32_t  era;
	uint32_t yoe, doy, doe;

	y -= (m <= 2u) ? 1 : 0;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (uint32_t)(y - era * 400);                         /* [0, 399] */
	doy = (153u * (m > 2u ? (m - 3u) : (m + 9u)) + 2u) / 5u + d - 1u;  /* [0, 365] */
	doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;          /* [0, 146096] */
	return era * 146097 + (int32_t)doe - 719468;
}

/* 自 1970-01-01 起的天数 → 公历 */
static void civil_from_days(int32_t z, int32_t *py, uint32_t *pm, uint32_t *pd)
{
	int32_t  era;
	uint32_t doe, yoe, doy, mp, y;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (uint32_t)(z - era * 146097);                      /* [0, 146096] */
	yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;  /* [0, 399] */
	y   = yoe + (uint32_t)era * 400u;
	doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);        /* [0, 365] */
	mp  = (5u * doy + 2u) / 153u;                            /* [0, 11] */
	*pd = doy - (153u * mp + 2u) / 5u + 1u;                  /* [1, 31] */
	*pm = (mp < 10u) ? (mp + 3u) : (mp - 9u);                /* [1, 12] */
	*py = (int32_t)y + ((*pm <= 2u) ? 1 : 0);
}

void rtc_to_tm(uint32_t epoch, rtc_tm_t *tm)
{
	uint32_t days = epoch / 86400u;
	uint32_t sod  = epoch % 86400u;       /* second of day */
	int32_t  y;
	uint32_t m, d;

	civil_from_days((int32_t)days, &y, &m, &d);

	tm->year  = (uint16_t)y;
	tm->month = (uint8_t)m;
	tm->day   = (uint8_t)d;
	tm->hour  = (uint8_t)(sod / 3600u);
	tm->min   = (uint8_t)((sod / 60u) % 60u);
	tm->sec   = (uint8_t)(sod % 60u);
	/* 1970-01-01 是周四，所以 +4 再取模 */
	tm->wday  = (uint8_t)((days + 4u) % 7u);
}

uint32_t rtc_from_tm(const rtc_tm_t *tm)
{
	int32_t days = days_from_civil((int32_t)tm->year, tm->month, tm->day);

	return (uint32_t)days * 86400u
	     + (uint32_t)tm->hour * 3600u
	     + (uint32_t)tm->min * 60u
	     + (uint32_t)tm->sec;
}

/* ==================================================================
 * 编译时刻 —— 烧录那一刻就是"现在"，于是每次上电时间自然是对的
 * ================================================================== */

uint32_t rtc_build_epoch(void)
{
	static const char mon3[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *d = __DATE__;      /* "Oct  6 2026" / "Oct 16 2026" */
	const char *t = __TIME__;      /* "11:49:01" */
	rtc_tm_t tm;
	uint32_t i;
	uint32_t mon = 1u;

	/* 月份名字三个字母对一遍 */
	for (i = 0u; i < 12u; i++)
	{
		if (d[0] == mon3[i * 3u] && d[1] == mon3[i * 3u + 1u] && d[2] == mon3[i * 3u + 2u])
		{
			mon = i + 1u;
			break;
		}
	}

	/* 日期是一位还是两位，取决于 d[4] 是不是空格 ——
	 * "Oct  6 2026" 的第 4 位是空格，而 "Oct 16 2026" 是 '1'。
	 * 年份固定在第 7~10 位，两种格式都一样。 */
	tm.day = (uint8_t)((d[4] == ' ')
	                  ? (d[5] - '0')
	                  : ((d[4] - '0') * 10 + (d[5] - '0')));

	tm.year  = (uint16_t)((d[7] - '0') * 1000 + (d[8] - '0') * 100
	                    + (d[9] - '0') * 10 + (d[10] - '0'));
	tm.month = (uint8_t)mon;
	tm.hour  = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
	tm.min   = (uint8_t)((t[3] - '0') * 10 + (t[4] - '0'));
	tm.sec   = (uint8_t)((t[6] - '0') * 10 + (t[7] - '0'));
	tm.wday  = 0u;

	return rtc_from_tm(&tm);
}

/* ==================================================================
 * RTC 寄存器操作（F1 的老式 RTC：PRL 预分频 + CNT 秒计数）
 * ================================================================== */

void rtc_set(uint32_t epoch)
{
	uint8_t  retry;
	uint32_t rb;

	/*
	 * 按 RM0008 的配置流程来，一步都不能省：
	 *   ① 轮询 RTOFF = 1（上一个操作已完成）
	 *   ② 置 CNF = 1 进配置模式
	 *   ③ 写 CNTH / CNTL
	 *   ④ 清 CNF = 0
	 *   ⑤ 再轮询 RTOFF = 1
	 *
	 * ★ 第 ① 步最初被漏掉了。它不是可有可无的礼节：RTC 的写是异步的
	 *   （要等 RTCCLK 同步），"写完 PRL"和"可以配置下一组寄存器"是两回事。
	 *
	 * ⚠ 另外注意 CRL 在 CMSIS 里是 __IO uint32_t（32 位），所以掩码也要用
	 *   32 位的 —— 用 (uint16_t)~RTC_CRL_CNF 会只保留低 16 位。
	 */
	for (retry = 0u; retry < 3u; retry++)
	{
		while ((RTC->CRL & RTC_CRL_RTOFF) == 0u) { }   /* ① */

		RTC->CRL |= RTC_CRL_CNF;                       /* ② */
		RTC->CNTH = (uint32_t)(epoch >> 16);           /* ③ */
		RTC->CNTL = (uint32_t)(epoch & 0xFFFFu);
		RTC->CRL &= ~RTC_CRL_CNF;                      /* ④ */

		while ((RTC->CRL & RTC_CRL_RTOFF) == 0u) { }   /* ⑤ */

		/* 回读校验。写没写进去读一遍就知道 —— 而不是等用户在屏幕上
		 * 看见 1970-01-01 才发现。这个校验要是当初就有，这次的
		 * "时间退回 1970" 在第一次上电时就能自己暴露出来。 */
		rb = ((uint32_t)RTC->CNTH << 16) | (uint32_t)RTC->CNTL;
		if (rb == epoch)
		{
			break;
		}
	}
	g_rtc_retry = retry;

	g_rtc_epoch = epoch;
}

uint32_t rtc_now(void)
{
	uint32_t a, b;

	/* 读 CNT 的时候可能正好碰上低位进位，两次读不一致就重来。
	 * 这不是防御性代码，是必需的：CNTH 和 CNTL 是两个寄存器，
	 * 中间隔着一个进位就会读出一个错得离谱的值（比如从 0x0000FFFF
	 * 跳到 0x00010000 时可能读到 0x00000000）。 */
	do
	{
		a = ((uint32_t)RTC->CNTH << 16) | (uint32_t)RTC->CNTL;
		b = ((uint32_t)RTC->CNTH << 16) | (uint32_t)RTC->CNTL;
	} while (a != b);

	g_rtc_epoch = a;
	return a;
}

/* ==================================================================
 * 对外：时钟源探测 + 初始化
 * ================================================================== */

rtc_src_t rtc_time_init(void)
{
	rtc_src_t src;
	uint32_t  presc;
	uint32_t  want_sel;
	uint32_t  cur_sel;
	uint32_t  cur;
	uint8_t   running;
	uint8_t   ok;

	/* 先把编译时刻算出来记账，不管后面走哪条路都能看到它 ——
	 * 上一版把它放在"重新配置"分支里，结果走提前返回时它一直是 0，
	 * 屏幕上那个 RTC SET = 0 把人往"初值算错了"的方向带偏了。 */
	g_rtc_build = rtc_build_epoch();

	/* 备份域写保护：main() 里已经开过，这里再确认一次，
	 * 免得本函数被别人单独调用时踩坑 */
	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_RCC_BKP_CLK_ENABLE();
	HAL_PWR_EnableBkUpAccess();

	/* ── ① 先挑一个能起振的时钟源 ──────────────────────────────── */
	RCC->BDCR |= RCC_BDCR_LSEON;
	ok = rtc_wait_flag(&RCC->BDCR, RCC_BDCR_LSERDY, RTC_LSE_PROBE_MS);

	if (ok != 0u)
	{
		src      = RTC_SRC_LSE;
		presc    = RTC_PRESC_LSE;
		want_sel = RCC_BDCR_RTCSEL_0;      /* 01 = LSE */
	}
	else
	{
		/* 起不来就把 LSE 关掉，别让它一直耗着 ——
		 * 板上根本没焊晶振时，这个驱动电路是空载振荡。 */
		RCC->BDCR &= ~RCC_BDCR_LSEON;

		RCC->CSR |= RCC_CSR_LSION;
		if (rtc_wait_flag(&RCC->CSR, RCC_CSR_LSIRDY, RTC_LSI_PROBE_MS) == 0u)
		{
			g_rtc_src = (uint32_t)RTC_SRC_NONE;
			return RTC_SRC_NONE;   /* 两个都没有：不阻塞启动，只是没时间 */
		}

		src      = RTC_SRC_LSI;
		presc    = RTC_PRESC_LSI;
		want_sel = RCC_BDCR_RTCSEL_1;      /* 10 = LSI */
	}

	/* ── ② 已经在跑、而且值合理？那就别动它 ────────────────────────
	 * 软复位（看门狗复位是常态）时备份域没断电，RTC 秒数还是对的，
	 * 拿编译时刻去覆盖它会让每次复位时间都跳回烧录那一刻。
	 *
	 * ★ 但"在跑"不等于"是对的"：计数器也可能是个从 0 开始一路往上数的
	 *   秒表（当初 rtc_set 写失败过）。所以判据要看**值**，不只看状态位。 */
	cur_sel = RCC->BDCR & RCC_BDCR_RTCSEL;
	running = (((RCC->BDCR & RCC_BDCR_RTCEN) != 0u) && (cur_sel != 0u)) ? 1u : 0u;

	if ((running != 0u) && (cur_sel == want_sel))
	{
		cur = rtc_now();
		if (cur >= RTC_EPOCH_MIN)
		{
			g_rtc_reset = 0u;
			g_rtc_src   = (uint32_t)src;
			return src;
		}
	}

	/* ── ③ 整个备份域复位重来（真断过电，或者时钟源换了）─────────── */
	RCC->BDCR |= RCC_BDCR_BDRST;
	RCC->BDCR &= ~RCC_BDCR_BDRST;

	/*
	 * ★★★ 这是整个模块最坑的一步：BDRST 复位的是**整个备份域**，
	 * 而 LSE 振荡器就住在备份域里 —— 也就是说前面刚点起来的 LSEON，
	 * 被上面那一行 BDRST 顺手清掉了。
	 *
	 * 漏掉下面这段"重新打开"的后果极其隐蔽：RTCSEL 指向 LSE、RTCEN 也开着，
	 * 从寄存器上看一切正常，**但 RTCCLK 根本没在跑**。于是紧接着等 RSF
	 * 就成了死等（RSF 要 RTCCLK 才被硬件置位）。
	 *
	 * 而启动路径上没有任何兜底：MX_IWDG_Init() 排在 rtc_time_init() 之后，
	 * 看门狗还没武装。所以表现出来就是"上电卡死、进不了系统"，
	 * 连拔电重启都救不回来 —— 只能重新烧录。
	 */
	if (src == RTC_SRC_LSE)
	{
		RCC->BDCR |= RCC_BDCR_LSEON;
		if (rtc_wait_flag(&RCC->BDCR, RCC_BDCR_LSERDY, RTC_LSE_PROBE_MS) == 0u)
		{
			g_rtc_src = (uint32_t)RTC_SRC_NONE;
			return RTC_SRC_NONE;
		}
	}
	else
	{
		RCC->CSR |= RCC_CSR_LSION;
		if (rtc_wait_flag(&RCC->CSR, RCC_CSR_LSIRDY, RTC_LSI_PROBE_MS) == 0u)
		{
			g_rtc_src = (uint32_t)RTC_SRC_NONE;
			return RTC_SRC_NONE;
		}
	}

	RCC->BDCR &= ~RCC_BDCR_RTCSEL;
	RCC->BDCR |= want_sel;
	RCC->BDCR |= RCC_BDCR_RTCEN;

	/* 等 RTC 寄存器组同步（RSF 由硬件置位，这之前读数不可信） */
	RTC->CRL &= ~RTC_CRL_RSF;
	if (rtc_wait_flag(&RTC->CRL, RTC_CRL_RSF, RTC_SYNC_MS) == 0u)
	{
		g_rtc_src = (uint32_t)RTC_SRC_NONE;
		return RTC_SRC_NONE;       /* 同步不上就不玩 RTC，别拖住启动 */
	}

	/* 写预分频：同样先等 RTOFF 再进配置模式 */
	if (rtc_wait_flag(&RTC->CRL, RTC_CRL_RTOFF, RTC_SYNC_MS) == 0u)
	{
		g_rtc_src = (uint32_t)RTC_SRC_NONE;
		return RTC_SRC_NONE;
	}
	RTC->CRL |= RTC_CRL_CNF;
	RTC->PRLH = (uint32_t)((presc >> 16) & 0x000Fu);
	RTC->PRLL = (uint32_t)(presc & 0xFFFFu);
	RTC->CRL &= ~RTC_CRL_CNF;
	(void)rtc_wait_flag(&RTC->CRL, RTC_CRL_RTOFF, RTC_SYNC_MS);

	/* 初值取编译时刻：烧录那一刻"现在"就是对的 */
	rtc_set(g_rtc_build);

	g_rtc_reset = 1u;
	g_rtc_src   = (uint32_t)src;
	return src;
}
