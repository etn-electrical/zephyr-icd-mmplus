/*
 * Copyright (c) 2022 ITE Corporation. All Rights Reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ite_enhance_i2c

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <errno.h>
#include <soc.h>
#include <soc_dt.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_ite_enhance, CONFIG_I2C_LOG_LEVEL);

#include "i2c-priv.h"

/* Start smbus session from idle state */
#define I2C_MSG_START BIT(5)

#define I2C_LINE_SCL_HIGH BIT(0)
#define I2C_LINE_SDA_HIGH BIT(1)
#define I2C_LINE_IDLE (I2C_LINE_SCL_HIGH | I2C_LINE_SDA_HIGH)

struct i2c_enhance_config {
	void (*irq_config_func)(void);
	uint32_t bitrate;
	uint8_t *base;
	uint8_t i2c_irq_base;
	uint8_t port;
	/* SCL GPIO cells */
	struct gpio_dt_spec scl_gpios;
	/* SDA GPIO cells */
	struct gpio_dt_spec sda_gpios;
	/* I2C alternate configuration */
	const struct pinctrl_dev_config *pcfg;
	uint8_t prescale_scl_low;
	uint32_t clock_gate_offset;
};

enum i2c_pin_fun {
	SCL = 0,
	SDA,
};

enum i2c_ch_status {
	I2C_CH_NORMAL = 0,
	I2C_CH_REPEAT_START,
	I2C_CH_WAIT_READ,
	I2C_CH_WAIT_NEXT_XFER,
};

struct i2c_enhance_data {
	enum i2c_ch_status i2ccs;
	struct i2c_msg *msgs;
	struct k_mutex mutex;
	struct k_sem device_sync_sem;
	/* Index into output data */
	size_t widx;
	/* Index into input data */
	size_t ridx;
	/* operation freq of i2c */
	uint32_t bus_freq;
	/* Error code, if any */
	uint32_t err;
	/* address of device */
	uint16_t addr_16bit;
	/* wait for stop bit interrupt */
	uint8_t stop;
};

enum enhanced_i2c_transfer_direct {
	TX_DIRECT,
	RX_DIRECT,
};

enum enhanced_i2c_ctl {
	/* Hardware reset */
	E_HW_RST = 0x01,
	/* Stop */
	E_STOP = 0x02,
	/* Start & Repeat start */
	E_START = 0x04,
	/* Acknowledge */
	E_ACK = 0x08,
	/* State reset */
	E_STS_RST = 0x10,
	/* Mode select */
	E_MODE_SEL = 0x20,
	/* I2C interrupt enable */
	E_INT_EN = 0x40,
	/* 0 : Standard mode , 1 : Receive mode */
	E_RX_MODE = 0x80,
	/* State reset and hardware reset */
	E_STS_AND_HW_RST = (E_STS_RST | E_HW_RST),
	/* Generate start condition and transmit slave address */
	E_START_ID = (E_INT_EN | E_MODE_SEL | E_ACK | E_START | E_HW_RST),
	/* Generate stop condition */
	E_FINISH = (E_INT_EN | E_MODE_SEL | E_ACK | E_STOP | E_HW_RST),
};

enum enhanced_i2c_host_status {
	/* ACK receive */
	E_HOSTA_ACK = 0x01,
	/* Interrupt pending */
	E_HOSTA_INTP = 0x02,
	/* Read/Write */
	E_HOSTA_RW = 0x04,
	/* Time out error */
	E_HOSTA_TMOE = 0x08,
	/* Arbitration lost */
	E_HOSTA_ARB = 0x10,
	/* Bus busy */
	E_HOSTA_BB = 0x20,
	/* Address match */
	E_HOSTA_AM = 0x40,
	/* Byte done status */
	E_HOSTA_BDS = 0x80,
	/* time out or lost arbitration */
	E_HOSTA_ANY_ERROR = (E_HOSTA_TMOE | E_HOSTA_ARB),
	/* Byte transfer done and ACK receive */
	E_HOSTA_BDS_AND_ACK = (E_HOSTA_BDS | E_HOSTA_ACK),
};

enum i2c_reset_cause {
	I2C_RC_NO_IDLE_FOR_START = 1,
	I2C_RC_TIMEOUT,
};

static int i2c_parsing_return_value(const struct device *dev)
{
	struct i2c_enhance_data *data = dev->data;

	if (!data->err) {
		return 0;
	}

	/* Connection timed out */
	if (data->err == ETIMEDOUT) {
		return -ETIMEDOUT;
	}

	/* The device does not respond ACK */
	if (data->err == E_HOSTA_ACK) {
		return -ENXIO;
	} else {
		return -EIO;
	}
}

static int i2c_get_line_levels(const struct device *dev)
{
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;
	int pin_sts = 0;

	if (IT8XXX2_I2C_TOS(base) & IT8XXX2_I2C_SCL_IN) {
		pin_sts |= I2C_LINE_SCL_HIGH;
	}

	if (IT8XXX2_I2C_TOS(base) & IT8XXX2_I2C_SDA_IN) {
		pin_sts |= I2C_LINE_SDA_HIGH;
	}

	return pin_sts;
}

static int i2c_is_busy(const struct device *dev)
{
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;

	return (IT8XXX2_I2C_STR(base) & E_HOSTA_BB);
}

static int i2c_bus_not_available(const struct device *dev)
{
	if (i2c_is_busy(dev) ||
		(i2c_get_line_levels(dev) != I2C_LINE_IDLE)) {
		return -EIO;
	}

	return 0;
}

static void i2c_reset(const struct device *dev)
{
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;

	/* State reset and hardware reset */
	IT8XXX2_I2C_CTR(base) = E_STS_AND_HW_RST;
}

/* Set clock frequency for i2c port D, E , or F */
static void i2c_enhanced_port_set_frequency(const struct device *dev,
					    int freq_hz)
{
	const struct i2c_enhance_config *config = dev->config;
	uint32_t clk_div, psr, pll_clock;
	uint8_t *base = config->base;

	pll_clock = chip_get_pll_freq();
	/*
	 * Let psr(Prescale) = IT8XXX2_I2C_PSR(p_ch)
	 * Then, 1 SCL cycle = 2 x (psr + 2) x SMBus clock cycle
	 * SMBus clock = pll_clock / clk_div
	 * SMBus clock cycle = 1 / SMBus clock
	 * 1 SCL cycle = 1 / freq
	 * 1 / freq = 2 x (psr + 2) x (1 / (pll_clock / clk_div))
	 * psr = ((pll_clock / clk_div) x (1 / freq) x (1 / 2)) - 2
	 */
	if (freq_hz) {
		/* Get SMBus clock divide value */
		clk_div = (IT8XXX2_ECPM_SCDCR2 & 0x0F) + 1U;
		/* Calculate PSR value */
		psr = (pll_clock / (clk_div * (2U * freq_hz))) - 2U;
		/* Set psr value under 0xFD */
		if (psr > 0xFD) {
			psr = 0xFD;
		}

		/* Adjust SCL low period prescale */
		psr += config->prescale_scl_low;

		/* Set I2C Speed */
		IT8XXX2_I2C_PSR(base) = psr & 0xFF;
		IT8XXX2_I2C_HSPR(base) = psr & 0xFF;
	}

}

static int i2c_enhance_configure(const struct device *dev,
				 uint32_t dev_config_raw)
{
	const struct i2c_enhance_config *config = dev->config;
	struct i2c_enhance_data *const data = dev->data;

	if (!(I2C_MODE_MASTER & dev_config_raw)) {
		return -EINVAL;
	}

	if (I2C_ADDR_10_BITS & dev_config_raw) {
		return -EINVAL;
	}

	data->bus_freq = I2C_SPEED_GET(dev_config_raw);

	i2c_enhanced_port_set_frequency(dev, config->bitrate);

	return 0;
}

static int i2c_enhance_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct i2c_enhance_data *const data = dev->data;
	uint32_t speed;

	if (!data->bus_freq) {
		LOG_ERR("The bus frequency is not initially configured.");
		return -EIO;
	}

	switch (data->bus_freq) {
	case I2C_SPEED_DT:
	case I2C_SPEED_STANDARD:
	case I2C_SPEED_FAST:
	case I2C_SPEED_FAST_PLUS:
		speed = I2C_SPEED_SET(data->bus_freq);
		break;
	default:
		return -ERANGE;
	}

	*dev_config = (I2C_MODE_MASTER | speed);

	return 0;
}

static int enhanced_i2c_error(const struct device *dev)
{
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;
	uint32_t i2c_str = IT8XXX2_I2C_STR(base);

	if (i2c_str & E_HOSTA_ANY_ERROR) {
		data->err = i2c_str & E_HOSTA_ANY_ERROR;
	/* device does not respond ACK */
	} else if ((i2c_str & E_HOSTA_BDS_AND_ACK) == E_HOSTA_BDS) {
		if (IT8XXX2_I2C_CTR(base) & E_ACK)
			data->err = E_HOSTA_ACK;
	}

	return data->err;
}

static void enhanced_i2c_start(const struct device *dev)
{
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;

	/* reset i2c port */
	i2c_reset(dev);
	/* Set i2c frequency */
	i2c_enhanced_port_set_frequency(dev, config->bitrate);
	/*
	 * Set time out register.
	 * I2C D/E/F clock/data low timeout.
	 */
	IT8XXX2_I2C_TOR(base) = I2C_CLK_LOW_TIMEOUT;
	/* bit1: Enable enhanced i2c module */
	IT8XXX2_I2C_CTR1(base) = IT8XXX2_I2C_MDL_EN;
}

static void i2c_pio_trans_data(const struct device *dev,
			       enum enhanced_i2c_transfer_direct direct,
			       uint16_t trans_data, int first_byte)
{
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;
	uint32_t nack = 0;

	if (first_byte) {
		/* First byte must be slave address. */
		IT8XXX2_I2C_DTR(base) = trans_data |
					(direct == RX_DIRECT ? BIT(0) : 0);
		/* start or repeat start signal. */
		IT8XXX2_I2C_CTR(base) = E_START_ID;
	} else {
		if (direct == TX_DIRECT) {
			/* Transmit data */
			IT8XXX2_I2C_DTR(base) = (uint8_t)trans_data;
		} else {
			/*
			 * Receive data.
			 * Last byte should be NACK in the end of read cycle
			 */
			if (((data->ridx + 1) == data->msgs->len) &&
				(data->msgs->flags & I2C_MSG_STOP)) {
				nack = 1;
			}
		}
		/* Set hardware reset to start next transmission */
		IT8XXX2_I2C_CTR(base) = E_INT_EN | E_MODE_SEL |
				E_HW_RST | (nack ? 0 : E_ACK);
	}
}

static int enhanced_i2c_tran_read(const struct device *dev)
{
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;
	uint8_t in_data = 0;

	if (data->msgs->flags & I2C_MSG_START) {
		/* clear start flag */
		data->msgs->flags &= ~I2C_MSG_START;
		enhanced_i2c_start(dev);
		/* Direct read  */
		data->i2ccs = I2C_CH_WAIT_READ;
		/* Send ID */
		i2c_pio_trans_data(dev, RX_DIRECT, data->addr_16bit << 1, 1);
	} else {
		if (data->i2ccs) {
			if (data->i2ccs == I2C_CH_WAIT_READ) {
				data->i2ccs = I2C_CH_NORMAL;
				/* Receive data */
				i2c_pio_trans_data(dev, RX_DIRECT, in_data, 0);

			/* data->msgs->flags == I2C_MSG_RESTART */
			} else {
				/* Write to read */
				data->i2ccs = I2C_CH_WAIT_READ;
				/* Send ID */
				i2c_pio_trans_data(dev, RX_DIRECT,
					data->addr_16bit << 1, 1);
			}
			/* Turn on irq before next direct read */
			irq_enable(config->i2c_irq_base);
		} else {
			if (data->ridx < data->msgs->len) {
				/* read data */
				*(data->msgs->buf++) = IT8XXX2_I2C_DRR(base);
				data->ridx++;
				/* done */
				if (data->ridx == data->msgs->len) {
					data->msgs->len = 0;
					if (data->msgs->flags & I2C_MSG_STOP) {
						data->i2ccs = I2C_CH_NORMAL;
						IT8XXX2_I2C_CTR(base) = E_FINISH;
						/* wait for stop bit interrupt */
						data->stop = 1;
						return 1;
					}
					/* End the transaction */
					data->i2ccs = I2C_CH_WAIT_READ;
					return 0;
				}
				/* read next byte */
				i2c_pio_trans_data(dev, RX_DIRECT, in_data, 0);
			}
		}
	}
	return 1;
}

static int enhanced_i2c_tran_write(const struct device *dev)
{
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;
	uint8_t out_data;

	if (data->msgs->flags & I2C_MSG_START) {
		/* Clear start bit */
		data->msgs->flags &= ~I2C_MSG_START;
		enhanced_i2c_start(dev);
		/* Send ID */
		i2c_pio_trans_data(dev, TX_DIRECT, data->addr_16bit << 1, 1);
	} else {
		/* Host has completed the transmission of a byte */
		if (data->widx < data->msgs->len) {
			out_data = *(data->msgs->buf++);
			data->widx++;

			/* Send Byte */
			i2c_pio_trans_data(dev, TX_DIRECT, out_data, 0);
			if (data->i2ccs == I2C_CH_WAIT_NEXT_XFER) {
				data->i2ccs = I2C_CH_NORMAL;
				irq_enable(config->i2c_irq_base);
			}
		} else {
			/* done */
			data->msgs->len = 0;
			if (data->msgs->flags & I2C_MSG_STOP) {
				IT8XXX2_I2C_CTR(base) = E_FINISH;
				/* wait for stop bit interrupt */
				data->stop = 1;
			} else {
				/* Direct write with direct read */
				data->i2ccs = I2C_CH_WAIT_NEXT_XFER;
				return 0;
			}
		}
	}
	return 1;
}

static int i2c_transaction(const struct device *dev)
{
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;

	/* no error */
	if (!(enhanced_i2c_error(dev))) {
		if (!data->stop) {
			/* i2c read */
			if (data->msgs->flags & I2C_MSG_READ) {
				return enhanced_i2c_tran_read(dev);
			/* i2c write */
			} else {
				return enhanced_i2c_tran_write(dev);
			}
		}
	}
	/* reset i2c port */
	i2c_reset(dev);
	IT8XXX2_I2C_CTR1(base) = 0;

	data->stop = 0;
	/* done doing work */
	return 0;
}

static int i2c_enhance_transfer(const struct device *dev, struct i2c_msg *msgs,
				uint8_t num_msgs, uint16_t addr)
{
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;
	int res;

	/* Lock mutex of i2c controller */
	k_mutex_lock(&data->mutex, K_FOREVER);
	/*
	 * If the transaction of write to read is divided into two
	 * transfers, the repeat start transfer uses this flag to
	 * exclude checking bus busy.
	 */
	if (data->i2ccs == I2C_CH_NORMAL) {
		/* Make sure we're in a good state to start */
		if (i2c_bus_not_available(dev)) {
			/* Recovery I2C bus */
			i2c_recover_bus(dev);
			/*
			 * After resetting I2C bus, if I2C bus is not available
			 * (No external pull-up), drop the transaction.
			 */
			if (i2c_bus_not_available(dev)) {
				/* Unlock mutex of i2c controller */
				k_mutex_unlock(&data->mutex);
				return -EIO;
			}
		}

		msgs->flags |= I2C_MSG_START;
	}

	for (int i = 0; i < num_msgs; i++) {

		data->widx = 0;
		data->ridx = 0;
		data->err = 0;
		data->msgs = &(msgs[i]);
		data->addr_16bit = addr;

		if (msgs->flags & I2C_MSG_START) {
			data->i2ccs = I2C_CH_NORMAL;
			/* enable i2c interrupt */
			irq_enable(config->i2c_irq_base);
		}
		/* Start transaction */
		i2c_transaction(dev);
		/* Wait for the transfer to complete */
		/* TODO: the timeout should be adjustable */
		res = k_sem_take(&data->device_sync_sem, K_MSEC(100));
		/*
		 * The irq will be enabled at the condition of start or
		 * repeat start of I2C. If timeout occurs without being
		 * wake up during suspend(ex: interrupt is not fired),
		 * the irq should be disabled immediately.
		 */
		irq_disable(config->i2c_irq_base);
		/*
		 * The transaction is dropped on any error(timeout, NACK, fail,
		 * bus error, device error).
		 */
		if (data->err) {
			break;
		}

		if (res != 0) {
			data->err = ETIMEDOUT;
			/* reset i2c port */
			i2c_reset(dev);
			LOG_ERR("I2C ch%d:0x%X reset cause %d",
				config->port, data->addr_16bit, I2C_RC_TIMEOUT);
			/* If this message is sent fail, drop the transaction. */
			break;
		}
	}

	/* reset i2c channel status */
	if (data->err || (msgs->flags & I2C_MSG_STOP)) {
		data->i2ccs = I2C_CH_NORMAL;
	}
	/* Unlock mutex of i2c controller */
	k_mutex_unlock(&data->mutex);

	return i2c_parsing_return_value(dev);
}

static void i2c_enhance_isr(void *arg)
{
	struct device *dev = (struct device *)arg;
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;

	/* If done doing work, wake up the task waiting for the transfer */
	if (!i2c_transaction(dev)) {
		k_sem_give(&data->device_sync_sem);
		irq_disable(config->i2c_irq_base);
	}
}

static int i2c_enhance_init(const struct device *dev)
{
	struct i2c_enhance_data *data = dev->data;
	const struct i2c_enhance_config *config = dev->config;
	uint8_t *base = config->base;
	uint32_t bitrate_cfg;
	int error, status;

	/* Initialize mutex and semaphore */
	k_mutex_init(&data->mutex);
	k_sem_init(&data->device_sync_sem, 0, K_SEM_MAX_LIMIT);

	/* Enable clock to specified peripheral */
	volatile uint8_t *reg = (volatile uint8_t *)
		(IT8XXX2_ECPM_BASE + (config->clock_gate_offset >> 8));
	uint8_t reg_mask = config->clock_gate_offset & 0xff;
	*reg &= ~reg_mask;

	/* Enable I2C function */
	/* Software reset */
	IT8XXX2_I2C_DHTR(base) |= IT8XXX2_I2C_SOFT_RST;
	IT8XXX2_I2C_DHTR(base) &= ~IT8XXX2_I2C_SOFT_RST;
	/* reset i2c port */
	i2c_reset(dev);
	/* bit1, Module enable */
	IT8XXX2_I2C_CTR1(base) = 0;

	/* Set clock frequency for I2C ports */
	if (config->bitrate == I2C_BITRATE_STANDARD ||
		config->bitrate == I2C_BITRATE_FAST ||
		config->bitrate == I2C_BITRATE_FAST_PLUS) {
		bitrate_cfg = i2c_map_dt_bitrate(config->bitrate);
	} else {
		/* Device tree specified speed */
		bitrate_cfg = I2C_SPEED_DT << I2C_SPEED_SHIFT;
	}

	error = i2c_enhance_configure(dev, I2C_MODE_MASTER | bitrate_cfg);
	data->i2ccs = I2C_CH_NORMAL;

	if (error) {
		LOG_ERR("i2c: failure initializing");
		return error;
	}

	/* Set the pin to I2C alternate function. */
	status = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (status < 0) {
		LOG_ERR("Failed to configure I2C pins");
		return status;
	}


	return 0;
}

static int i2c_enhance_recover_bus(const struct device *dev)
{
	const struct i2c_enhance_config *config = dev->config;
	int i, status;

	/* Set SCL of I2C as GPIO pin */
	gpio_pin_configure_dt(&config->scl_gpios, GPIO_OUTPUT);
	/* Set SDA of I2C as GPIO pin */
	gpio_pin_configure_dt(&config->sda_gpios, GPIO_OUTPUT);

	/*
	 * In I2C recovery bus, 1ms sleep interval for bitbanging i2c
	 * is mainly to ensure that gpio has enough time to go from
	 * low to high or high to low.
	 */
	/* Pull SCL and SDA pin to high */
	gpio_pin_set_dt(&config->scl_gpios, 1);
	gpio_pin_set_dt(&config->sda_gpios, 1);
	k_msleep(1);

	/* Start condition */
	gpio_pin_set_dt(&config->sda_gpios, 0);
	k_msleep(1);
	gpio_pin_set_dt(&config->scl_gpios, 0);
	k_msleep(1);

	/* 9 cycles of SCL with SDA held high */
	for (i = 0; i < 9; i++) {
		/* SDA */
		gpio_pin_set_dt(&config->sda_gpios, 1);
		/* SCL */
		gpio_pin_set_dt(&config->scl_gpios, 1);
		k_msleep(1);
		/* SCL */
		gpio_pin_set_dt(&config->scl_gpios, 0);
		k_msleep(1);
	}
	/* SDA */
	gpio_pin_set_dt(&config->sda_gpios, 0);
	k_msleep(1);

	/* Stop condition */
	gpio_pin_set_dt(&config->scl_gpios, 1);
	k_msleep(1);
	gpio_pin_set_dt(&config->sda_gpios, 1);
	k_msleep(1);

	/* Set GPIO back to I2C alternate function */
	status = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (status < 0) {
		LOG_ERR("Failed to configure I2C pins");
		return status;
	}

	/* reset i2c port */
	i2c_reset(dev);
	LOG_ERR("I2C ch%d reset cause %d", config->port,
		I2C_RC_NO_IDLE_FOR_START);

	return 0;
}

static const struct i2c_driver_api i2c_enhance_driver_api = {
	.configure = i2c_enhance_configure,
	.get_config = i2c_enhance_get_config,
	.transfer = i2c_enhance_transfer,
	.recover_bus = i2c_enhance_recover_bus,
};

#define I2C_ITE_ENHANCE_INIT(inst)                                              \
	PINCTRL_DT_INST_DEFINE(inst);                                           \
	BUILD_ASSERT((DT_INST_PROP(inst, clock_frequency) ==                    \
		     50000) ||                                                  \
		     (DT_INST_PROP(inst, clock_frequency) ==                    \
		     I2C_BITRATE_STANDARD) ||                                   \
		     (DT_INST_PROP(inst, clock_frequency) ==                    \
		     I2C_BITRATE_FAST) ||                                       \
		     (DT_INST_PROP(inst, clock_frequency) ==                    \
		     I2C_BITRATE_FAST_PLUS), "Not support I2C bit rate value"); \
	static void i2c_enhance_config_func_##inst(void);                       \
										\
	static const struct i2c_enhance_config i2c_enhance_cfg_##inst = {       \
		.base = (uint8_t *)(DT_INST_REG_ADDR(inst)),                    \
		.irq_config_func = i2c_enhance_config_func_##inst,              \
		.bitrate = DT_INST_PROP(inst, clock_frequency),                 \
		.i2c_irq_base = DT_INST_IRQN(inst),                             \
		.port = DT_INST_PROP(inst, port_num),                           \
		.scl_gpios = GPIO_DT_SPEC_INST_GET(inst, scl_gpios),            \
		.sda_gpios = GPIO_DT_SPEC_INST_GET(inst, sda_gpios),            \
		.prescale_scl_low = DT_INST_PROP_OR(inst, prescale_scl_low, 0), \
		.clock_gate_offset = DT_INST_PROP(inst, clock_gate_offset),     \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                   \
	};                                                                      \
										\
	static struct i2c_enhance_data i2c_enhance_data_##inst;                 \
										\
	I2C_DEVICE_DT_INST_DEFINE(inst, i2c_enhance_init,                       \
				  NULL,                                         \
				  &i2c_enhance_data_##inst,                     \
				  &i2c_enhance_cfg_##inst,                      \
				  POST_KERNEL,                                  \
				  CONFIG_KERNEL_INIT_PRIORITY_DEVICE,           \
				  &i2c_enhance_driver_api);                     \
										\
	static void i2c_enhance_config_func_##inst(void)                        \
	{                                                                       \
		IRQ_CONNECT(DT_INST_IRQN(inst),                                 \
			0,                                                      \
			i2c_enhance_isr,                                        \
			DEVICE_DT_INST_GET(inst), 0);                           \
	}

DT_INST_FOREACH_STATUS_OKAY(I2C_ITE_ENHANCE_INIT)
