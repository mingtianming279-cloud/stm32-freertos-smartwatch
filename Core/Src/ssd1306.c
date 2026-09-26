/*
 * ssd1306.c
 *
 * SSD1306 (I2C) 驱动实现
 *
 * 阶段 1（当前）：总线自检 —— 先证明"能通话"，再谈"说什么"。
 *   驱动开发第一课：用一个干净的"通/不通"判据，把接线、供电、上拉、
 *   时钟这些故障源一次性收敛掉，别和初始化序列的错误混在一起排查。
 */

#include "ssd1306.h"
#include "i2c.h"
#include <string.h>

volatile uint32_t g_i2c_found      = 0;
volatile uint32_t g_i2c_addr1      = 0;
volatile uint32_t g_i2c_addr2      = 0;
volatile uint32_t g_ssd1306_online = 0;

/* ------------------------------------------------------------------
 * 总线体检：扫 0x08~0x77，把应答的地址写进 out[]，返回应答个数
 *
 * 两个坑：
 *   ① HAL 的地址参数是 8 位格式（bit0 = 读写位）→ 7 位地址必须 << 1，
 *      否则 0x3C 会被当成"发给 0x1E"，永远扫不到东西。
 *   ② 扫描范围避开保留地址：0x00~0x07（广播/保留）与 0x78~0x7F。
 * ------------------------------------------------------------------ */
/* 单个地址的探测超时（ms）。正常器件几十 µs 就 ACK，2 ms 已有几十倍余量。
 * 之所以抠得这么死：这是"启动期"代码，每个地址多等 1 ms，112 个地址就多等
 * 112 ms，而这段时间看门狗正在数秒。 */
#define I2C_PROBE_TIMEOUT_MS   2u

static uint8_t i2c_scan(uint8_t *out, uint8_t max_n)
{
	uint8_t n = 0;
	uint32_t busy_cnt = 0u;

	for (uint16_t a = 0x08u; a < 0x78u; a++)
	{
		HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1),
		                                             1u, I2C_PROBE_TIMEOUT_MS);
		if (st == HAL_BUSY)
		{
			/* BUSY 又锁死了：先恢复总线，再把这个地址重问一次。
			 * 这里千万不能直接 break —— 那样 g_i2c_found 会变成"一个都没扫到"
			 * 的假 0，把"总线卡死"误报成"总线上没有器件"（我就被这个坑过一次）。 */
			if (i2c1_recover_bus() == 0u)
			{
				break;              /* 真救不回来，别再耗了 */
			}
			if (++busy_cnt > 4u)
			{
				/* 每个地址都锁一次的话，112 × (25 ms + 恢复) 要 3 秒，
				 * 白白把启动拖长。救几次还不行就认输。 */
				break;
			}
			st = HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1),
			                           1u, I2C_PROBE_TIMEOUT_MS);
		}
		if (st == HAL_OK)
		{
			if (n < max_n) out[n] = (uint8_t)a;
			n++;
		}
	}
	return n;
}

void ssd1306_selftest(void)
{
	uint8_t found[8];

	/* 体检之前先把总线救活：MX_I2C1_Init() 里已经试过一次，但那是复位后
	 * 几毫秒，从机可能还没上电稳。到这一刻（启动约几十毫秒）再试一次非常便宜
	 * —— BUSY 没置位就直接返回。不救的话下面 112 个地址会逐个白等 25 ms。 */
	i2c1_recover_bus();

	g_i2c_found = i2c_scan(found, 8u);
	if (g_i2c_found > 0u) g_i2c_addr1 = found[0];
	if (g_i2c_found > 1u) g_i2c_addr2 = found[1];

	/* SSD1306 单点探测：发地址帧，等 ACK */
	/* 这次故意用又宽又多次的探测（20 ms × 2 次），不走上面那个 2 ms：
	 * 它的结果 g_ssd1306_online 是一次性的、会被锁存下来，直接决定
	 * main() 到底调不调 ssd1306_init()。误判成"不在线"的代价是屏永远黑，
	 * 而多花的这几十毫秒在启动期完全不值一提。 */
	g_ssd1306_online = (HAL_I2C_IsDeviceReady(&hi2c1,
	                        (uint16_t)(SSD1306_I2C_ADDR7 << 1), 2u, 20u) == HAL_OK) ? 1u : 0u;
}
/* ---- 底层：发一条命令 ------------------------------------------- */
static void ssd1306_cmd(uint8_t c)
{
	/* 借 HAL_I2C_Mem_Write：MemAddress 正好当"控制字节"用。
	 * 提示：命令的控制字节是 ____（16 进制，别忘 u 后缀） */
	HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(SSD1306_I2C_ADDR7 << 1),
			0x00u, I2C_MEMADD_SIZE_8BIT, &c, 1u, 50u);
}

/* ---- 初始化序列：照手册抄，99% 的"不亮"都出在这一段 ------------- */
static const uint8_t init_seq[] = {
	0xAE,              /* 关显示：初始化期间不让乱码露脸          */
	0x20, 0x02,        /* 内存寻址模式 = 页寻址                   */
	0xB0,              /* 起始页 = 0                              */
	0xC8,              /* COM 扫描方向 = 反向                    */
	0x00, 0x10,        /* 列地址低 4 位 / 高 4 位 = 0             */
	0x40,              /* 显示起始行 = 0                          */
	0x81, 0x7F,        /* 对比度                                  */
	0xA1,              /* 段重映射：左右翻转                      */
	0xA6,              /* 正常显示（不是反色）                    */
	0xA8, 0x3F,        /* 复用率 = 64 行                          */
	0xD3, 0x00,        /* 显示垂直偏移 = 0                        */
	0xD5, 0x80,        /* 时钟分频 / 振荡频率                     */
	0xD9, 0xF1,        /* 预充电周期                              */
	0xDA, 0x12,        /* COM 引脚硬件配置                        */
	0xDB, 0x40,        /* VCOMH 电平                              */
	0x8D, 0x14,        /* ★ 电荷泵使能 —— 漏了屏永远不亮          */
	0xAF,              /* 开显示                                  */
};

void ssd1306_init(void)
{
	HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(SSD1306_I2C_ADDR7 << 1),
	                  0x00u, I2C_MEMADD_SIZE_8BIT,
	                  (uint8_t *)init_seq, sizeof(init_seq), 200u);
}


/* ---- 整屏填充：color = 0xFF 全白，0x00 全黑 ---------------------- */
void ssd1306_fill(uint8_t color)
{
	uint8_t buf[129];

	/* 提示：这次后面跟的是"数据"，控制字节是 ____ */
	buf[0] = 0x40u;
	for (uint8_t i = 1u; i < 129u; i++) buf[i] = color;

	for (uint8_t page = 0u; page < 8u; page++)
	{
		ssd1306_cmd((uint8_t)(0xB0u | page));  /* 选中第 page 页   */
		ssd1306_cmd(0x00u);                    /* 列地址低 4 位 = 0 */
		ssd1306_cmd(0x10u);                    /* 列地址高 4 位 = 0 */
		HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)(SSD1306_I2C_ADDR7 << 1),
		                        buf, 129u, 100u);
	}
}
void ssd1306_bar(uint8_t row,int16_t v)
{
	uint8_t buf[129];
	uint8_t page = (uint8_t)(1u + row * 3u);   /* 第 0/1/2 行 → 第
	  1/4/7 页 */

	buf[0] = 0x40u;
	for (uint8_t i = 1u; i < 129u; i++) buf[i] = 0x00u;   /*整页先清黑 */

	buf[1u + 64u] = 0x01u;                     /* 中点刻度：1像素高，代表 0 */

	int32_t x = 64 + (int32_t)v * 60 / 16000;  /* 满量程映射到 ±60 列*/
	if (x < 1)   x = 1;
	if (x > 126) x = 126;
	buf[1u + x] = 0xFFu;                       /* 指针：整页 8 像素高*/

	ssd1306_cmd((uint8_t)(0xB0u | page));
	ssd1306_cmd(0x00u);
	ssd1306_cmd(0x10u);
	HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)(SSD1306_I2C_ADDR7 << 1), buf, 129u, 100u);


}
