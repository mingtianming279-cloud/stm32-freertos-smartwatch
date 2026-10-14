#include "mpu6050.h"

/* ── DMA 版读取（2026-10-14）────────────────────────────────────────
 * 搬运走 DMA1_Channel7（配置手写在 i2c.c 的 HAL_I2C_MspInit），
 * 完成收尾靠 I2C1 事件中断（stm32f1xx_it.c 的 I2C1_EV_IRQHandler）。
 * mpu_rx_done 必须是全局变量：HAL_I2C_MemRxCpltCallback 在中断上下文
 * 里执行，看不见函数内的局部变量。 */
volatile uint8_t mpu_rx_done = 0;

/* DMA 收完 6 个字节后，HAL 在中断里调这里 */
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c->Instance == I2C1) mpu_rx_done = 1;
}

 HAL_StatusTypeDef mpu_who_am_i(uint8_t *who)
    {
	 return HAL_I2C_Mem_Read(&hi2c1, MPU_ADDR_8BIT, MPU_REG_WHO_AM_I,I2C_MEMADD_SIZE_8BIT, who, 1u, 100u);

    }


 HAL_StatusTypeDef mpu_wakeup(void)
     {
         uint8_t zero = 0x00u;
         return HAL_I2C_Mem_Write(&hi2c1, MPU_ADDR_8BIT,MPU_REG_PWR_MGMT_1,I2C_MEMADD_SIZE_8BIT, &zero, 1u, 100u);			//在mpu6050的电源寄存器置0
     }

 HAL_StatusTypeDef mpu_read_accel(int16_t *ax,int16_t *ay,int16_t *az)
 {
	 uint8_t b [6];
	 uint32_t t0;
	 HAL_StatusTypeDef st;

	 mpu_rx_done = 0;
	 st = HAL_I2C_Mem_Read_DMA(&hi2c1, MPU_ADDR_8BIT, MPU_REG_ACCEL_XOUT_H, I2C_MEMADD_SIZE_8BIT, b, 6U);
	 if(st != HAL_OK) return st;

	 /* DMA 调用立即返回，此刻数据还没到：等完成标志，100 ms 超时兜底 */
	 t0 = HAL_GetTick();
	 while (!mpu_rx_done)
	 {
		 if (HAL_GetTick() - t0 > 100U) return HAL_TIMEOUT;
	 }

	 *ax = (int16_t)((b[0] << 8) | b[1]);
	 *ay = (int16_t)((b[2] << 8) | b[3]);
	 *az = (int16_t)((b[4] << 8) | b[5]);
	 return HAL_OK;

 }
