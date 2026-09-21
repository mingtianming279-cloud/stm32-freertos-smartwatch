#ifndef INC_MPU6050_H_
    #define INC_MPU6050_H_

    #include <stdint.h>
    #include "i2c.h"

    /* 7 位从机地址：AD0 接地 = 0x68 */
    #define MPU_ADDR_7BIT     0x68u
    /* HAL 惯例：DevAddress 传"7 位左移 1 位"的 8 位形式 */
    #define MPU_ADDR_8BIT     (MPU_ADDR_7BIT << 1)

    /* WHO_AM_I：芯片自报身份 */
    #define MPU_REG_WHO_AM_I  0x75u
    #define MPU_WHO_AM_I_VAL  0x68u
	#define  MPU_REG_ACCEL_XOUT_H 0x3Bu
	#define MPU_REG_PWR_MGMT_1  0x6Bu


    /* 读芯片身份：HAL_OK = 总线通了；who 里是它自报的家门 */

    HAL_StatusTypeDef mpu_who_am_i(uint8_t *who);
    HAL_StatusTypeDef mpu_wakeup(void);
    HAL_StatusTypeDef mpu_read_accel(int16_t *ax, int16_t *ay, int16_t *az);
    #endif /* INC_MPU6050_H_ */
