#include "mpu6050.h"
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
	 HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1, MPU_ADDR_8BIT, MPU_REG_ACCEL_XOUT_H,I2C_MEMADD_SIZE_8BIT,b,6U,100U);

	 if(st != HAL_OK) return st;

	 *ax = (int16_t)((b[0] << 8) | b[1]);
	 *ay = (int16_t)((b[2] << 8) | b[3]);
	 *az = (int16_t)((b[4] << 8) | b[5]);
	 return HAL_OK;

 }
