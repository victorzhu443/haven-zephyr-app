/* Host-test fake for <zephyr/drivers/i2c.h>.
 *
 * Real behaviour, not a no-op: every i2c_write_dt() is decoded (4-byte
 * big-endian register address + payload) into haven_fake_i2c_log[] so tests
 * can assert exactly which ADAU1860 registers adau1860_control.c touched and
 * with what bytes. i2c_write_read_dt() answers from a tiny register model so
 * the bring-up sequence's STATUS2 polls succeed (see haven_fake_i2c_reg8()).
 */
#ifndef FAKE_ZEPHYR_DRIVERS_I2C_H_
#define FAKE_ZEPHYR_DRIVERS_I2C_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>

struct i2c_dt_spec {
	const struct device *bus;
	uint16_t addr;
};

extern const struct device haven_fake_i2c_bus_dev;
#define I2C_DT_SPEC_GET(node) { .bus = &haven_fake_i2c_bus_dev, .addr = 0x64 }

#define HAVEN_FAKE_I2C_LOG_MAX 512
#define HAVEN_FAKE_I2C_MAX_PAYLOAD 64

struct haven_fake_i2c_xfer {
	uint32_t reg;
	uint8_t data[HAVEN_FAKE_I2C_MAX_PAYLOAD];
	size_t len;
};

static struct haven_fake_i2c_xfer haven_fake_i2c_log[HAVEN_FAKE_I2C_LOG_MAX];
static size_t haven_fake_i2c_log_count;
static int haven_fake_i2c_fail_writes; /* nonzero: every write returns -EIO */

static inline void haven_fake_i2c_reset(void)
{
	haven_fake_i2c_log_count = 0;
	haven_fake_i2c_fail_writes = 0;
}

/* Register model for reads: STATUS2 reports power-up complete + input ASRC
 * locked + FM ready so init's and the tone path's polls pass; ID registers
 * return a recognisable pattern; anything else reads 0.
 */
static inline uint8_t haven_fake_i2c_reg8(uint32_t reg)
{
	switch (reg) {
	case 0x4000C402u: /* STATUS2 */
		return (1 << 7) | (1 << 2) | (1 << 1);
	case 0x4000C404u: /* EQ_STATUS: clear done */
		return 1;
	case 0x4000C000u: /* VENDOR_ID */
		return 0x41;
	case 0x4000C001u:
		return 0x18;
	case 0x4000C002u:
		return 0x60;
	default:
		return 0;
	}
}

static inline uint32_t haven_fake_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline int i2c_write_dt(const struct i2c_dt_spec *spec, const uint8_t *buf, uint32_t len)
{
	(void)spec;
	if (haven_fake_i2c_fail_writes) {
		return -5; /* -EIO */
	}
	if (len < 4 || len - 4 > HAVEN_FAKE_I2C_MAX_PAYLOAD ||
	    haven_fake_i2c_log_count >= HAVEN_FAKE_I2C_LOG_MAX) {
		return -22; /* -EINVAL */
	}
	struct haven_fake_i2c_xfer *x = &haven_fake_i2c_log[haven_fake_i2c_log_count++];

	x->reg = haven_fake_be32(buf);
	x->len = len - 4;
	memcpy(x->data, buf + 4, x->len);
	return 0;
}

static inline int i2c_write_read_dt(const struct i2c_dt_spec *spec, const void *write_buf,
				    size_t num_write, void *read_buf, size_t num_read)
{
	(void)spec;
	if (num_write != 4) {
		return -22;
	}
	uint32_t reg = haven_fake_be32((const uint8_t *)write_buf);
	uint8_t *out = (uint8_t *)read_buf;

	for (size_t i = 0; i < num_read; i++) {
		out[i] = haven_fake_i2c_reg8(reg + (uint32_t)i);
	}
	return 0;
}

/* Test helpers: find the last write to `reg`, or count writes to it. */
static inline const struct haven_fake_i2c_xfer *haven_fake_i2c_last_write(uint32_t reg)
{
	for (size_t i = haven_fake_i2c_log_count; i > 0; i--) {
		if (haven_fake_i2c_log[i - 1].reg == reg) {
			return &haven_fake_i2c_log[i - 1];
		}
	}
	return NULL;
}

static inline size_t haven_fake_i2c_count_writes(uint32_t reg)
{
	size_t n = 0;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		if (haven_fake_i2c_log[i].reg == reg) {
			n++;
		}
	}
	return n;
}

#endif /* FAKE_ZEPHYR_DRIVERS_I2C_H_ */
