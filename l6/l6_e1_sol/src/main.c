/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <nrfx_twim.h>
#include <hal/nrf_twim.h>

#define I2C_NODE DT_NODELABEL(mysensor)

int main(void)
{
	static const struct i2c_dt_spec dev_i2c = I2C_DT_SPEC_GET(I2C_NODE);

	if (!device_is_ready(dev_i2c.bus)) {
		printk("I2C bus %s is not ready!\n", dev_i2c.bus->name);
		return -1;
	}

	printk("TWIM CLK test: sending 1 byte in loop...\n");

	/* Read current FREQUENCY register before configure */
	NRF_TWIM_Type *twim = NRF_TWIM21;
	printk("Before i2c_configure, FREQUENCY = 0x%08X\n", (unsigned int)twim->FREQUENCY);

	/* Explicitly set I2C speed to 1000k */
	int ret = i2c_configure(dev_i2c.bus,
				    I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST_PLUS));
	if (ret != 0) {
		printk("i2c_configure failed: %d\n", ret);
		return -1;
	}
	printk("After i2c_configure, FREQUENCY = 0x%08X\n", (unsigned int)twim->FREQUENCY);

	uint8_t data = 0xAA;

	while (1) {
		(void)i2c_write_dt(&dev_i2c, &data, 1);
	}

	return 0;
}
