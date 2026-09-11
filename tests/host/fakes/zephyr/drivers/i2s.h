/* Host-test fake for <zephyr/drivers/i2s.h>.
 *
 * Records the configuration and every block handed to i2s_write() (the
 * last block's samples are kept so tests can analyse the actual waveform
 * tone_gen.c produces) and counts triggers. Blocks are returned to their
 * mem_slab immediately, standing in for the driver freeing them after DMA.
 */
#ifndef FAKE_ZEPHYR_DRIVERS_I2S_H_
#define FAKE_ZEPHYR_DRIVERS_I2S_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

enum i2s_dir {
	I2S_DIR_RX,
	I2S_DIR_TX,
	I2S_DIR_BOTH,
};

enum i2s_trigger_cmd {
	I2S_TRIGGER_START,
	I2S_TRIGGER_STOP,
	I2S_TRIGGER_DRAIN,
	I2S_TRIGGER_DROP,
	I2S_TRIGGER_PREPARE,
};

typedef uint8_t i2s_fmt_t;
typedef uint8_t i2s_opt_t;

#define I2S_FMT_DATA_FORMAT_I2S      (0 << 0)
#define I2S_OPT_BIT_CLK_MASTER       (0 << 0)
#define I2S_OPT_FRAME_CLK_MASTER     (0 << 1)
#define I2S_OPT_BIT_CLK_SLAVE        (1 << 0)
#define I2S_OPT_FRAME_CLK_SLAVE      (1 << 1)

struct i2s_config {
	uint8_t word_size;
	uint8_t channels;
	i2s_fmt_t format;
	i2s_opt_t options;
	uint32_t frame_clk_freq;
	struct k_mem_slab *mem_slab;
	size_t block_size;
	int32_t timeout;
};

static const struct device haven_fake_device_i2s0 = { .name = "fake-i2s0" };

static struct i2s_config haven_fake_i2s_cfg;
static int haven_fake_i2s_configure_calls;
static int haven_fake_i2s_write_calls;
static int haven_fake_i2s_trigger_calls[5];
static int haven_fake_i2s_fail_writes; /* nonzero: i2s_write returns -EIO */

#define HAVEN_FAKE_I2S_MAX_BLOCK_BYTES 2048
static uint8_t haven_fake_i2s_last_block[HAVEN_FAKE_I2S_MAX_BLOCK_BYTES];
static size_t haven_fake_i2s_last_block_len;

/* Optional capture of every sample written, for multi-block analysis. */
#define HAVEN_FAKE_I2S_CAPTURE_MAX 65536
static int16_t haven_fake_i2s_capture[HAVEN_FAKE_I2S_CAPTURE_MAX];
static size_t haven_fake_i2s_capture_len;

static inline void haven_fake_i2s_reset(void)
{
	memset(&haven_fake_i2s_cfg, 0, sizeof(haven_fake_i2s_cfg));
	haven_fake_i2s_configure_calls = 0;
	haven_fake_i2s_write_calls = 0;
	memset(haven_fake_i2s_trigger_calls, 0, sizeof(haven_fake_i2s_trigger_calls));
	haven_fake_i2s_fail_writes = 0;
	haven_fake_i2s_last_block_len = 0;
	haven_fake_i2s_capture_len = 0;
}

static inline int i2s_configure(const struct device *dev, enum i2s_dir dir,
				const struct i2s_config *cfg)
{
	(void)dev;
	(void)dir;
	haven_fake_i2s_cfg = *cfg;
	haven_fake_i2s_configure_calls++;
	return 0;
}

static inline int i2s_write(const struct device *dev, void *mem_block, size_t size)
{
	(void)dev;
	if (haven_fake_i2s_fail_writes) {
		return -5; /* -EIO */
	}
	haven_fake_i2s_write_calls++;
	if (size <= HAVEN_FAKE_I2S_MAX_BLOCK_BYTES) {
		memcpy(haven_fake_i2s_last_block, mem_block, size);
		haven_fake_i2s_last_block_len = size;
	}
	size_t n = size / 2;

	if (haven_fake_i2s_capture_len + n <= HAVEN_FAKE_I2S_CAPTURE_MAX) {
		memcpy(&haven_fake_i2s_capture[haven_fake_i2s_capture_len], mem_block, size);
		haven_fake_i2s_capture_len += n;
	}
	if (haven_fake_i2s_cfg.mem_slab) {
		k_mem_slab_free(haven_fake_i2s_cfg.mem_slab, mem_block);
	}
	return 0;
}

static inline int i2s_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	(void)dev;
	(void)dir;
	haven_fake_i2s_trigger_calls[cmd]++;
	return 0;
}

#endif /* FAKE_ZEPHYR_DRIVERS_I2S_H_ */
