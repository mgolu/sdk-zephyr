/* ST Microelectronics STMEMS hal i/f
 *
 * Copyright (c) 2021 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * zephyrproject-rtos/modules/hal/st/sensor/stmemsc/
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_STMEMSC_STMEMSC_H_
#define ZEPHYR_DRIVERS_SENSOR_STMEMSC_STMEMSC_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i3c.h>
#include <zephyr/drivers/spi.h>

static inline void stmemsc_mdelay(uint32_t millisec)
{
	k_msleep(millisec);
}

#ifdef CONFIG_I2C
int stmemsc_i2c_read(const struct i2c_dt_spec *stmemsc,
		     uint8_t reg_addr, uint8_t *value, uint8_t len);
int stmemsc_i2c_write(const struct i2c_dt_spec *stmemsc,
		      uint8_t reg_addr, uint8_t *value, uint8_t len);
#endif

#ifdef CONFIG_I3C
int stmemsc_i3c_read(void *stmemsc,
		     uint8_t reg_addr, uint8_t *value, uint8_t len);
int stmemsc_i3c_write(void *stmemsc,
		      uint8_t reg_addr, uint8_t *value, uint8_t len);
#endif

#ifdef CONFIG_SPI
int stmemsc_spi_read(const struct spi_dt_spec *stmemsc,
		     uint8_t reg_addr, uint8_t *value, uint8_t len);
int stmemsc_spi_write(const struct spi_dt_spec *stmemsc,
		      uint8_t reg_addr, uint8_t *value, uint8_t len);
#endif
#endif /* ZEPHYR_DRIVERS_SENSOR_STMEMSC_STMEMSC_H_ */
