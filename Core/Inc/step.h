/*
 * step.h
 *
 * 计步器 —— 对外接口
 *
 * 刻意做成"无状态输入输出"的纯算法：不碰 I2C、不碰任务、不读全局变量。
 * 这样调参的时候可以把一段真实录下来的加速度数据在 PC 上回放，
 * 比"改一行阈值、烧一次、走两步"快一个数量级 —— 而计步的参数几乎
 * 肯定是要反复调的。
 *
 * 单位约定：输入是 MPU6050 的原始读数，±2g 量程下 16384 LSB = 1g。
 */

#ifndef INC_STEP_H_
#define INC_STEP_H_

#include <stdint.h>

/* 清零：步数、重力基线、状态机、峰值统计全部复位 */
void step_reset(void);

/* 喂一个采样点。
 * dt_ms = 距上一次调用的毫秒数（algo 任务固定 20 ms）。
 * 之所以要传时间而不是写死，是为了 PC 回放时能用别的采样率。 */
void step_update(int16_t ax, int16_t ay, int16_t az, uint16_t dt_ms);

/* 累计步数 */
uint32_t step_count(void);

/* ---- 调参用的观测量（现场表达式里挂这几个，或者看 STEP 页面）--------
 * 调阈值时的正确做法是：正常走一段路，盯 g_step_dyn_peak ——
 * 阈值应该明显低于峰值、但明显高于静止时的噪声底。 */
extern volatile int32_t  g_step_mag;       /* 本次合加速度幅值（1g ≈ 16384） */
extern volatile int32_t  g_step_dyn;       /* 本次动态分量 = mag - 重力基线 */
extern volatile int32_t  g_step_base;      /* 当前重力基线，静止时会收敛到 16384 */
extern volatile int32_t  g_step_dyn_peak;  /* 动态分量的历史峰值（调阈值看这个） */
extern volatile uint32_t g_step_reject;    /* 被最小步间隔挡掉的次数 */
extern volatile uint32_t g_step_shock;     /* 被"大冲击冷却"挡掉的次数 */
extern volatile int32_t  g_step_dc;        /* dyn 的直流分量估计（判"持续运动"用它） */
extern volatile uint32_t g_step_hold;      /* 被"持续运动"按住、暂停判步的拍数 */

#endif /* INC_STEP_H_ */