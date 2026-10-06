/*
 * st7789.c
 *
 * ST7789 (SPI, 240x240 IPS) 底层驱动 —— 只管硬件这一层
 *
 * 职责边界（和上层 lcd.c 的分工）：
 *   本文件懂：SPI 事务、CS/DC/RST/背光、窗口寄存器、像素字节序。
 *   本文件不懂："字符""字符串""矩形"这些概念，那是 lcd.c 的事。
 *
 * 通信三件套：
 *   CS  片选（低有效）
 *   DC  0 = 命令 / 1 = 数据（命令和像素共用 MOSI，靠它分辨）
 *   像素按 RGB565 发，每像素 2 字节，高字节在前
 *
 * 20KB RAM 装不下整屏 RGB565（240x240x2 = 115200 字节），
 * 所以不做 framebuffer，走"设窗口 + 连续灌像素"的方式：
 * 不管画什么，最终都归到 blit_begin → blit_data × N → blit_end，
 * 全程只需要一个行缓冲 —— 这是整套设计里最省内存的一步。
 */

#include "st7789.h"
#include "spi.h"

/* ---- 引脚（对应 CubeMX 里的 PA0~PA3）---- */
#define ST_PORT   GPIOA
#define ST_CS     GPIO_PIN_0
#define ST_DC     GPIO_PIN_1
#define ST_RST    GPIO_PIN_2
#define ST_BLK    GPIO_PIN_3

/* ------------------------------------------------------------------
 * 取证计数器（现场表达式里按名字挂，把"固件发没发出去"和"屏有没有反应"分开）
 *
 *   开机五色测试时：g_spi_tx_ok = 1234，g_spi_tx_err = 0，g_spi_state = 1(READY)
 *   1234 的来历：init 共 9 次发送 + 5 次刷屏 × 245 次
 *                （黑/白/红/绿/蓝 = 5 次；每次 5 条窗口命令 + 240 行像素 = 245）
 *   如果 g_spi_tx_ok 远小于预期 → 程序没跑到，或卡在 HAL_SPI_Transmit
 *   如果 g_spi_tx_err 在涨      → HAL 认为发送失败（外设状态不对）
 *
 *   ⚠ 屏上开始画文字和图形之后，这个数就不再是固定值了。它的意义从
 *     "全都发成功了"变成"我发出去多少次"，只能用来对照程序走没走到，
 *     不能再当正确性判据 —— 画错位置、画到屏外，它一样是涨的。
 * ------------------------------------------------------------------ */
volatile uint32_t g_spi_tx_ok  = 0;
volatile uint32_t g_spi_tx_err = 0;
volatile uint32_t g_spi_state  = 0;   /* st7789_init() 之后的 hspi1.State 快照（READY=1） */

/* ------------------------------------------------------------------
 * 底层：CS / DC / 发送
 * ------------------------------------------------------------------ */

/* CS 低有效：on=1 表示"选中" */
static void st_cs(uint8_t on)
{
	HAL_GPIO_WritePin(ST_PORT, ST_CS, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/* DC：0=命令，1=数据 */
static void st_dc(uint8_t data)
{
	HAL_GPIO_WritePin(ST_PORT, ST_DC, data ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 发送 + 记账（所有发送都走这里，方便统计到底发出去了多少） */
static void st_tx(const uint8_t *buf, uint16_t n)
{
	if (HAL_SPI_Transmit(&hspi1, (uint8_t *)buf, n, 100u) == HAL_OK)
	{
		g_spi_tx_ok++;
	}
	else
	{
		g_spi_tx_err++;
	}
}

/* 发一条命令：命令态 → 选中 → 发 1 字节 → 放开 */
static void st_cmd(uint8_t c)
{
	st_dc(0);
	st_cs(1);
	st_tx(&c, 1u);
	st_cs(0);
}

/* 发一串数据（参数或像素） */
static void st_data(const uint8_t *buf, uint16_t n)
{
	st_dc(1);
	st_cs(1);
	st_tx(buf, n);
	st_cs(0);
}

/* 一条命令带一个参数 */
static void st_cmd1(uint8_t cmd, uint8_t arg)
{
	st_cmd(cmd);
	st_data(&arg, 1u);
}

/* 设置写入窗口：列 x0..x1，行 y0..y1 */
static void st_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	uint8_t b[4];

	st_cmd(0x2Au);                      /* CASET：列地址 */
	b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0;
	b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
	st_data(b, 4u);

	st_cmd(0x2Bu);                      /* RASET：行地址 */
	b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0;
	b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
	st_data(b, 4u);

	st_cmd(0x2Cu);                      /* RAMWR：之后来的都是像素数据 */
}

/* 一行像素缓冲：240 像素 × 2 字节 = 480 字节。
 * 必须是文件级，不能放局部 —— 任务栈没有这么大（ui 栈才 1KB）。 */
static uint8_t s_line[ST7789_W * 2u];

/* 把行缓冲的前 w 个像素填成同一颜色，供 fill_rect 复用 */
static void st_line_fill(uint16_t w, uint16_t color)
{
	uint8_t hi = (uint8_t)(color >> 8);
	uint8_t lo = (uint8_t)(color & 0xFFu);

	for (uint16_t i = 0u; i < w; i++)
	{
		s_line[i * 2u]      = hi;
		s_line[i * 2u + 1u] = lo;
	}
}

/* ------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------ */

void st7789_init(void)
{
	HAL_GPIO_WritePin(ST_PORT, ST_BLK, GPIO_PIN_SET);   /* 背光亮 */

	/* 硬复位 */
	HAL_GPIO_WritePin(ST_PORT, ST_RST, GPIO_PIN_RESET);
	HAL_Delay(20);
	HAL_GPIO_WritePin(ST_PORT, ST_RST, GPIO_PIN_SET);
	HAL_Delay(120);

	st_cmd(0x01);          /* SWRESET 软复位 */
	HAL_Delay(120);
	st_cmd(0x11);          /* SLPOUT 退出睡眠 */
	HAL_Delay(120);
	st_cmd1(0x3A, 0x55);   /* COLMOD：16bit/像素 = RGB565 */
	st_cmd1(0x36, 0x00);   /* MADCTL：显示方向（0x00 = 默认方向） */
	st_cmd(0x21);          /* INVON：ST7789 IPS 面板需要反显，否则颜色是负片 */
	st_cmd(0x13);          /* NORON：正常显示模式 */
	st_cmd(0x29);          /* DISPON：开显示 */

	g_spi_state = (uint32_t)hspi1.State;   /* 取证：此刻 SPI 外设状态 */
}

void st7789_blit_begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	st_set_window(x0, y0, x1, y1);
	st_dc(1);              /* 接下来是像素 */
	st_cs(1);              /* 整个像素流期间 CS 保持选中，不逐行断开 */
}

void st7789_blit_data(const uint8_t *buf, uint16_t n)
{
	if (n == 0u) return;
	st_tx(buf, n);
}

void st7789_blit_end(void)
{
	st_cs(0);
}

void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	/* 越界先裁掉。原因：ST7789 收到超出面板范围的窗口时，表现是
	 * "什么都不显示"而不是"显示一部分"—— 它是按窗口整体丢弃的。
	 * 所以宁可在这里裁，也不要让它带着越界坐标进去。 */
	if (w == 0u || h == 0u) return;
	if (x >= ST7789_W || y >= ST7789_H) return;
	if ((uint32_t)x + w > ST7789_W) w = (uint16_t)(ST7789_W - x);
	if ((uint32_t)y + h > ST7789_H) h = (uint16_t)(ST7789_H - y);

	st_line_fill(w, color);

	st7789_blit_begin(x, y, (uint16_t)(x + w - 1u), (uint16_t)(y + h - 1u));
	for (uint16_t r = 0u; r < h; r++)
	{
		st7789_blit_data(s_line, (uint16_t)(w * 2u));
	}
	st7789_blit_end();
}

void st7789_fill_screen(uint16_t color)
{
	st7789_fill_rect(0u, 0u, ST7789_W, ST7789_H, color);
}