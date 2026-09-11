#include "adau1860_control.h"
#include "adau1860_regs.h"
#include "lark_fdsp_program.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* picolibc's math.h only exposes M_PI under a feature-test macro that
 * -std=c17 doesn't define. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(adau1860_control, LOG_LEVEL_INF);

#ifdef CONFIG_HAVEN_FDSP_BANK
#define FDSP_BANK CONFIG_HAVEN_FDSP_BANK
#else
#define FDSP_BANK LARK_FDSP_BANK_TRANSPARENCY
#endif

/* ── Devicetree bindings ─────────────────────────────────────────────────── */
#define ADAU1860_NODE DT_NODELABEL(adau1860)

static const struct i2c_dt_spec bus = I2C_DT_SPEC_GET(ADAU1860_NODE);
static const struct gpio_dt_spec enable_gpio = GPIO_DT_SPEC_GET(ADAU1860_NODE, enable_gpios);
#if DT_NODE_HAS_PROP(ADAU1860_NODE, supply_gpios)
static const struct gpio_dt_spec supply_gpio = GPIO_DT_SPEC_GET(ADAU1860_NODE, supply_gpios);
#define SUPPLY_DELAY_US DT_PROP(ADAU1860_NODE, supply_delay_us)
#endif

static bool initialised;

/* ── Control-port I/O ────────────────────────────────────────────────────────
 * 32-bit register address, big-endian, immediately followed by the payload in
 * ONE I2C write (upstream sends it as two Zephyr messages without a repeated
 * START, which the nRF TWIM driver only accepts with a concat buffer; a single
 * buffer avoids that devicetree dependency). Multi-word memory payloads
 * (program / parameter / safeload) go little-endian word by word -- that is
 * the byte order upstream puts on the wire by memcpy-ing uint32_t arrays on a
 * little-endian Cortex-M, and its firmware runs on this board.
 */
#define REG_ADDR_LEN 4
#define MAX_PAYLOAD  64

static int reg_write(uint32_t reg, const uint8_t *data, size_t len)
{
	uint8_t buf[REG_ADDR_LEN + MAX_PAYLOAD];

	if (len > MAX_PAYLOAD) {
		return -EINVAL;
	}
	sys_put_be32(reg, buf);
	memcpy(&buf[REG_ADDR_LEN], data, len);

	int err = i2c_write_dt(&bus, buf, REG_ADDR_LEN + len);

	if (err) {
		LOG_ERR("I2C write reg 0x%08x (%u B) failed: %d", reg, (unsigned int)len, err);
	}
	return err;
}

static int reg_write8(uint32_t reg, uint8_t val)
{
	return reg_write(reg, &val, 1);
}

static int reg_read(uint32_t reg, uint8_t *data, size_t len)
{
	uint8_t addr[REG_ADDR_LEN];

	sys_put_be32(reg, addr);

	int err = i2c_write_read_dt(&bus, addr, sizeof(addr), data, len);

	if (err) {
		LOG_ERR("I2C read reg 0x%08x (%u B) failed: %d", reg, (unsigned int)len, err);
	}
	return err;
}

static int mem_write_words(uint32_t addr, const uint32_t *words, size_t count)
{
	uint8_t buf[MAX_PAYLOAD];

	if (count * 4 > MAX_PAYLOAD) {
		return -EINVAL;
	}
	for (size_t i = 0; i < count; i++) {
		sys_put_le32(words[i], &buf[4 * i]);
	}
	return reg_write(addr, buf, count * 4);
}

/* Poll STATUS2 until every bit in `mask` is set. Upstream spins forever
 * here; a bounded wait turns "codec unpowered / wrong address / enable pin
 * not wired" into a clean error at boot instead of a hang.
 */
static int wait_status2(uint8_t mask, const char *what)
{
	int64_t deadline = k_uptime_get() + 200;

	for (;;) {
		uint8_t status2 = 0;
		int err = reg_read(ADAU1860_REG_STATUS2, &status2, 1);

		if (err) {
			return err;
		}
		if ((status2 & mask) == mask) {
			return 0;
		}
		if (k_uptime_get() > deadline) {
			LOG_ERR("Timed out waiting for %s (STATUS2=0x%02x, want mask 0x%02x)", what,
				status2, mask);
			return -ETIMEDOUT;
		}
		k_usleep(100);
	}
}

/* ── FastDSP parameter access ────────────────────────────────────────────────
 * Safeload: stage the 5 words for one slot in FDSP_SL_P0_0.. then pulse
 * FDSP_SL_UPDATE; the engine swaps them in between frames, so a filter never
 * runs on a half-written coefficient set. Applies to the currently selected
 * bank only (upstream marks writes to inactive banks "not working").
 */
static int fdsp_safe_load(uint8_t slot, const uint32_t params[ADAU1860_FDSP_NUM_PARAMS])
{
	int err = reg_write8(ADAU1860_REG_FDSP_SL_ADDR, slot);

	if (err) {
		return err;
	}
	err = mem_write_words(ADAU1860_REG_FDSP_SL_P0_0, params, ADAU1860_FDSP_NUM_PARAMS);
	if (err) {
		return err;
	}
	return reg_write8(ADAU1860_REG_FDSP_SL_UPDATE, 1);
}

/* Slot's shipped parameters from the running bank, with one word replaced --
 * upstream's fdsp_safe_load(address, n, param) pattern, used for the volume
 * and mute stages whose other parameters we keep as Lark Studio exported.
 */
static int fdsp_set_slot_param(uint8_t slot, int n, uint32_t value)
{
	uint32_t params[ADAU1860_FDSP_NUM_PARAMS];

	for (int i = 0; i < ADAU1860_FDSP_NUM_PARAMS; i++) {
		params[i] = lark_fdsp_param_bank[FDSP_BANK][i][slot];
	}
	params[n] = value;
	return fdsp_safe_load(slot, params);
}

static int fdsp_select_bank(uint8_t bank)
{
	uint8_t ctrl1 = 0;
	int err = reg_read(ADAU1860_REG_FDSP_CTRL1, &ctrl1, 1);

	if (err) {
		return err;
	}
	ctrl1 = (ctrl1 & ~0x03u) | (bank & 0x03u);
	return reg_write8(ADAU1860_REG_FDSP_CTRL1, ctrl1);
}

/* ── Coefficient math + encoding ───────────────────────────────────────────── */

/* Q5.27 with saturation (range [-16, 16)). 1.0 = 0x08000000. */
static uint32_t q27_encode(double v)
{
	double scaled = v * 134217728.0; /* 2^27 */

	if (scaled >= 2147483647.0) {
		return 0x7FFFFFFFu;
	}
	if (scaled <= -2147483648.0) {
		return 0x80000000u;
	}
	int32_t i = (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);

	return (uint32_t)i;
}

/* Same RBJ cookbook math validated on the Teensy prototype and in
 * tinnitus_dsp/test_filter.py, extended with a variable-depth peaking cut:
 * at PROTOCOL_ATTEN_MAX_DB the band degenerates to the classic notch.
 * Computed in double: at 192 kHz a 200 Hz / Q=20 notch has a2 within 3e-4 of
 * 1.0, which single precision represents to only ~6e-8 -- coarser than a
 * Q5.27 LSB (7.5e-9).
 */
static void calc_band_coeffs(const struct filter_band *band, struct adau1860_biquad *c)
{
	double fs = (double)ADAU1860_FDSP_RATE_HZ;
	double w0 = 2.0 * M_PI * ((double)band->f0_hz / fs);
	double alpha = sin(w0) / (2.0 * (double)band->q);
	double cosw0 = cos(w0);

	if (band->atten_db >= PROTOCOL_ATTEN_MAX_DB) {
		/* Pure notch */
		double a0 = 1.0 + alpha;

		c->b0 = 1.0 / a0;
		c->b1 = -2.0 * cosw0 / a0;
		c->b2 = 1.0 / a0;
		c->a1 = -2.0 * cosw0 / a0;
		c->a2 = (1.0 - alpha) / a0;
	} else {
		/* Peaking EQ cut of atten_db decibels */
		double A = pow(10.0, -(double)band->atten_db / 40.0);
		double a0 = 1.0 + alpha / A;

		c->b0 = (1.0 + alpha * A) / a0;
		c->b1 = -2.0 * cosw0 / a0;
		c->b2 = (1.0 - alpha * A) / a0;
		c->a1 = -2.0 * cosw0 / a0;
		c->a2 = (1.0 - alpha / A) / a0;
	}
}

/* RBJ → FastDSP slot parameters. Feedback taps are negated: see the header. */
static void biquad_to_fdsp(const struct adau1860_biquad *c, struct adau1860_fdsp_biquad *out)
{
	out->p[0] = q27_encode(c->b0);
	out->p[1] = q27_encode(c->b1);
	out->p[2] = q27_encode(c->b2);
	out->p[3] = q27_encode(-c->a1);
	out->p[4] = q27_encode(-c->a2);
}

static const struct adau1860_fdsp_biquad unity_biquad = {
	.p = { ADAU1860_Q27_ONE, 0, 0, 0, 0 },
};

/* ── Bring-up sequence (ported from upstream ADAU1860::begin/setup_*) ──────── */

static int power_up(void)
{
	int err;

#if DT_NODE_HAS_PROP(ADAU1860_NODE, supply_gpios)
	if (!gpio_is_ready_dt(&supply_gpio)) {
		LOG_ERR("supply GPIO not ready");
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&supply_gpio, GPIO_OUTPUT_ACTIVE);
	if (err) {
		return err;
	}
	k_usleep(SUPPLY_DELAY_US);
#endif

	if (!gpio_is_ready_dt(&enable_gpio)) {
		LOG_ERR("enable GPIO not ready");
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&enable_gpio, GPIO_OUTPUT_ACTIVE);
	if (err) {
		return err;
	}
	/* Common-mode rise time before the control port is usable (upstream:
	 * "CM rise time (see datasheet)"). */
	k_msleep(35);

	uint8_t id[4];

	err = reg_read(ADAU1860_REG_VENDOR_ID, id, sizeof(id));
	if (err) {
		LOG_ERR("No response at I2C 0x%02x -- check DAC_ENABLE, the V_LS rail, and the "
			"bus/pins (see docs/fastdsp-program.md)", bus.addr);
		return -ENODEV;
	}
	LOG_INF("ADAU1860 vendor 0x%02x device 0x%02x%02x rev 0x%02x", id[0], id[1], id[2],
		id[3]);

	/* Non self-boot: bypass the start-up delay counter. */
	err = reg_write8(ADAU1860_REG_PMU_CTRL2, 0x01);
	if (err) {
		return err;
	}
	/* CM_STARTUP_OVER */
	err = reg_write8(ADAU1860_REG_CHIP_PWR, 0x10);
	if (err) {
		return err;
	}
	/* PLL bypass, 24.576 MHz crystal index (UG-2017: MCLK_FREQ_24P576 +
	 * PLL_FM_BYPASS), bit 7 set during power-up. */
	err = reg_write8(ADAU1860_REG_CLK_CTRL13, (1 << 7) | (1 << 4) | 0x01);
	if (err) {
		return err;
	}
	err = wait_status2(1 << 7, "power-up complete");
	if (err) {
		return err;
	}
	/* Frequency multiplier on, then drop bit 7 and wait for FM ready. */
	err = reg_write8(ADAU1860_REG_CLK_CTRL12, 0x01);
	if (err) {
		return err;
	}
	err = reg_write8(ADAU1860_REG_CLK_CTRL13, (1 << 4) | 0x01);
	if (err) {
		return err;
	}
	err = wait_status2((1 << 7) | (1 << 1), "frequency multiplier ready");
	if (err) {
		return err;
	}
	/* CM_STARTUP_OVER | master enable | Hibernate 1 (UG-2017: Hibernate1 +
	 * BLOCKS_ON + CM_BST_ON). */
	return reg_write8(ADAU1860_REG_CHIP_PWR, 0x40 | 0x04 | 0x01);
}

/* DMIC in → decimator → output ASRC → serial port 0 (mic to the nRF, unused
 * by Haven today but harmless), serial port 0 in → input ASRC (future tone
 * path). Mirrors upstream's bidirectional-headset branch register for
 * register.
 */
static int configure_routing(void)
{
	static const struct {
		uint32_t reg;
		uint8_t val;
	} seq[] = {
		{ ADAU1860_REG_SPT0_CTRL1, 0x10 },       /* 16 BCLKs per slot */
		{ ADAU1860_REG_SAI_CLK_PWR, 0x01 | 0x02 | 0x10 }, /* I2S in, I2S out, mic */
		{ ADAU1860_REG_ASRC_PWR, 0x31 },         /* ASRCO1, ASRCO0, ASRCI0 */
		{ ADAU1860_REG_DMIC_PWR, 0x03 },         /* DMIC channels 0 and 1 */
		{ ADAU1860_REG_FDEC_PWR, 0x03 },         /* decimators 0 and 1 */
		{ ADAU1860_REG_FDEC_CTRL1, 0x24 },       /* 192 kHz -> 48 kHz */
		{ ADAU1860_REG_FDEC_ROUTE0, 39 },        /* DMIC channel 0 */
		{ ADAU1860_REG_FDEC_ROUTE1, 40 },        /* DMIC channel 1 */
		{ ADAU1860_REG_ASRCO_ROUTE0, 39 },
		{ ADAU1860_REG_ASRCO_ROUTE1, 40 },
		{ ADAU1860_REG_SPT0_ROUTE0, 32 },        /* ASRCO 0 */
		{ ADAU1860_REG_SPT0_ROUTE1, 33 },        /* ASRCO 1 */
		{ ADAU1860_REG_DMIC_VOL0, 0x20 },        /* +12 dB */
		{ ADAU1860_REG_DMIC_VOL1, 0x20 },
		{ ADAU1860_REG_DMIC_CTRL1, 0x33 },       /* 3.072 MHz DMIC clock */
		{ ADAU1860_REG_DMIC_CTRL2, 0x04 },       /* 192 kHz */
	};

	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		int err = reg_write8(seq[i].reg, seq[i].val);

		if (err) {
			return err;
		}
	}
	return 0;
}

static int load_fdsp(void)
{
	int err = reg_write8(ADAU1860_REG_DSP_PWR, 0x01);

	if (err) {
		return err;
	}
	err = mem_write_words(ADAU1860_FDSP_PROG_MEM, lark_fdsp_program, LARK_FDSP_PROGRAM_WORDS);
	if (err) {
		return err;
	}
	for (int k = 0; k < ADAU1860_FDSP_NUM_BANKS; k++) {
		for (int n = 0; n < ADAU1860_FDSP_NUM_PARAMS; n++) {
			err = mem_write_words(ADAU1860_FDSP_BANK(k, n), lark_fdsp_param_bank[k][n],
					      LARK_FDSP_NUM_SLOTS);
			if (err) {
				return err;
			}
		}
	}
	/* Frame-rate source: DMIC01. ADAU1860_FDSP_RATE_HZ must match this. */
	err = reg_write8(ADAU1860_REG_FDSP_CTRL4, 2);
	if (err) {
		return err;
	}
	err = fdsp_select_bank(FDSP_BANK);
	if (err) {
		return err;
	}
	return reg_write8(ADAU1860_REG_FDSP_RUN, 0x01);
}

static int configure_dac(void)
{
	static const struct {
		uint32_t reg;
		uint8_t val;
	} seq[] = {
		{ ADAU1860_REG_DAC_ROUTE0, ADAU1860_DAC_ROUTE_FDSP_CH(0) },
		{ ADAU1860_REG_ADC_DAC_HP_PWR, 0x10 },   /* DAC/HP channel 0 on */
		{ ADAU1860_REG_HP_LVMODE_CTRL1, 0x03 },  /* HP low-voltage mode + CM */
		{ ADAU1860_REG_HP_LVMODE_CTRL3, 0x01 },
		{ ADAU1860_REG_HPLDO_CTRL, 0x01 },
		{ ADAU1860_REG_DAC_CTRL1, 0x04 },        /* 192 kHz */
		{ ADAU1860_REG_DAC_NOISE_CTRL1, 0x10 },
		{ ADAU1860_REG_DAC_NOISE_CTRL2, 0x02 },
		{ ADAU1860_REG_PB_CTRL, 0x02 },          /* high performance */
		{ ADAU1860_REG_DAC_VOL0, 0xFF - 0xC0 },  /* upstream MAX_VOLUME_REG_VAL */
		{ ADAU1860_REG_DAC_CTRL2, 0x00 },        /* unmute */
	};

	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		int err = reg_write8(seq[i].reg, seq[i].val);

		if (err) {
			return err;
		}
	}
	return 0;
}

static int set_all_biquads_unity(void)
{
	for (uint8_t slot = 0; slot < LARK_FDSP_NUM_BIQUADS; slot++) {
		int err = fdsp_safe_load(slot, unity_biquad.p);

		if (err) {
			return err;
		}
	}
	return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

int adau1860_control_init(void)
{
	int err;

	if (!device_is_ready(bus.bus)) {
		LOG_ERR("I2C bus for the ADAU1860 not ready");
		return -ENODEV;
	}

	err = power_up();
	if (err) {
		return err;
	}
	err = configure_routing();
	if (err) {
		return err;
	}
	err = load_fdsp();
	if (err) {
		return err;
	}
	err = configure_dac();
	if (err) {
		return err;
	}
	/* Boot flat: upstream's bank ships a transparency EQ in slots 0-4;
	 * Haven's bands replace it, so start from pass-through rather than
	 * someone else's curve. */
	err = set_all_biquads_unity();
	if (err) {
		return err;
	}

	initialised = true;
	LOG_INF("ADAU1860 up: FastDSP bank %d running, DMIC -> 5 biquads -> DAC, fs %u Hz",
		FDSP_BANK, (unsigned int)ADAU1860_FDSP_RATE_HZ);
	return 0;
}

int adau1860_control_apply_filters(const struct filter_band *bands, size_t count)
{
	int err = 0;

	if (count > PROTOCOL_MAX_BANDS) {
		count = PROTOCOL_MAX_BANDS;
	}

	for (uint8_t slot = 0; slot < LARK_FDSP_NUM_BIQUADS; slot++) {
		struct adau1860_fdsp_biquad w = unity_biquad;

		if (slot < count) {
			struct adau1860_biquad c;

			calc_band_coeffs(&bands[slot], &c);
			biquad_to_fdsp(&c, &w);
			LOG_INF("Band %u: f0=%.1f Hz Q=%.1f atten=%.1f dB", slot,
				(double)bands[slot].f0_hz, (double)bands[slot].q,
				(double)bands[slot].atten_db);
		}
		if (!initialised) {
			continue; /* bench without a codec: math only */
		}
		int e = fdsp_safe_load(slot, w.p);

		if (e && !err) {
			err = e;
		}
	}
	return err;
}

int adau1860_control_set_bypass(bool enabled)
{
	LOG_INF("Bypass: %s", enabled ? "ENABLED (flat hear-through)" : "disabled");
	if (!enabled || !initialised) {
		/* Leaving bypass is main.c re-sending the bands. */
		return 0;
	}
	return set_all_biquads_unity();
}

int adau1860_control_set_volume_pct(uint8_t volume_pct)
{
	if (volume_pct > 100) {
		volume_pct = 100;
	}
	/* Upstream fdsp_set_volume(): gain = 10^(-3 * (255 - v) / 255), v in
	 * 0..255 -> -60 dB .. 0 dB, in Q5.27. */
	double v = (double)volume_pct * 255.0 / 100.0;
	double gain = pow(10.0, -3.0 * (255.0 - v) / 255.0);
	uint32_t word = q27_encode(gain > 1.0 ? 1.0 : gain);

	LOG_INF("Volume %u%% -> gain word 0x%08x", volume_pct, word);
	if (!initialised) {
		return 0;
	}
	return fdsp_set_slot_param(LARK_FDSP_SLOT_VOLUME, 4, word);
}

int adau1860_control_set_mute(bool muted)
{
	LOG_INF("Mute: %s", muted ? "on" : "off");
	if (!initialised) {
		return 0;
	}
	return fdsp_set_slot_param(LARK_FDSP_SLOT_MUTE, 4, muted ? 0u : ADAU1860_Q27_ONE);
}

int adau1860_control_set_tone(float f0_hz, float level_db)
{
	/* NOT IMPLEMENTED on hardware yet -- see the header. Caller
	 * (tone_safety.c) has already clamped level_db. */
	LOG_WRN("Tone [no hardware path yet]: f0=%.1f Hz level=%.1f dB", (double)f0_hz,
		(double)level_db);
	return 0;
}

int adau1860_control_set_tone_level(float level_db)
{
	LOG_WRN("Tone level [no hardware path yet]: %.1f dB", (double)level_db);
	return 0;
}

int adau1860_control_stop_tone(void)
{
	LOG_INF("Tone stop [no hardware path yet]");
	return 0;
}

void adau1860_control_on_ble_connected(void)
{
	LOG_INF("BLE connected -- filters unchanged");
}

void adau1860_control_on_ble_disconnected(void)
{
	LOG_INF("BLE disconnected -- filters kept running");
}
