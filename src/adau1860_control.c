/* ADAU1860 ("Lark") codec/DSP driver.
 *
 * The bring-up sequence (power_up / configure_routing / load_fdsp /
 * configure_dac) and the safeload / volume / mute register recipes are
 * ported from OpenEarable/open-earable-2 src/drivers/ADAU1860.cpp
 * (OpenEarable project, TECO / KIT). Original distributed under the license
 * in third_party/open-earable-2/LICENSE; modifications for Haven 2026.
 * The coefficient math, Q5.27 encoding and everything else is Haven's.
 */
#include "adau1860_control.h"
#include "adau1860_regs.h"
#include "lark_fdsp_program.h"
#include "lark_eq_program.h"
#include "tone_gen.h"

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

/* What feeds the DAC. Normal operation is FastDSP channel 0 (the biquad
 * chain). CONFIG_HAVEN_DAC_SOURCE_DMIC_DIRECT routes the raw PDM mic straight
 * to the DAC -- a first-power-on smoke test that proves mic, clocks, DAC and
 * receiver with no DSP in the loop (so a silent device is a hardware fault,
 * not a program-routing question). Not for wearing: no filters, no limiter.
 */
#if defined(CONFIG_HAVEN_DAC_SOURCE_DMIC_DIRECT)
#define HAVEN_DAC_SOURCE_ROUTE ADAU1860_DAC_ROUTE_DMIC(0)
#define HAVEN_DAC_SOURCE_NAME  "DMIC0 direct (smoke test, no DSP)"
#elif defined(CONFIG_HAVEN_DAC_SOURCE_EQ)
#define HAVEN_DAC_SOURCE_ROUTE ADAU1860_DAC_ROUTE_EQ
#define HAVEN_DAC_SOURCE_NAME  "EQ engine <- DMIC0 (Route B, 6 biquads)"
#define HAVEN_USE_EQ_ENGINE 1
#else
#define HAVEN_DAC_SOURCE_ROUTE ADAU1860_DAC_ROUTE_FDSP_CH(0)
#define HAVEN_DAC_SOURCE_NAME  "FastDSP ch0 (5 biquads)"
#endif
#ifndef HAVEN_USE_EQ_ENGINE
#define HAVEN_USE_EQ_ENGINE 0
#endif

/* Hardware output ceiling: DAC digital volume, written once at boot and on
 * adau1860_control_set_output_ceiling_db(). Lark SDK: dB = 24 - 0.375*code,
 * code 0xFF = mute. Sits after every DSP path, unreachable from the app. */
#ifdef CONFIG_HAVEN_OUTPUT_CEILING_DB
#define OUTPUT_CEILING_DB_DEFAULT CONFIG_HAVEN_OUTPUT_CEILING_DB
#else
#define OUTPUT_CEILING_DB_DEFAULT 0
#endif
static int output_ceiling_db = OUTPUT_CEILING_DB_DEFAULT;

/* LDL tone: commanded level that maps to 0 dBFS on the I2S link. Nominal
 * until acoustic calibration (haven-app docs/calibration.md) replaces it. */
#ifdef CONFIG_HAVEN_TONE_FULL_SCALE_DB
#define TONE_FULL_SCALE_DB ((float)CONFIG_HAVEN_TONE_FULL_SCALE_DB)
#else
#define TONE_FULL_SCALE_DB 85.0f
#endif

/* Codec-side routing while a tone plays -- see docs/tone-path.md. Default:
 * DAC fed straight from the I2S input (upstream's no-DSP playback route),
 * hear-through suspended, tone guaranteed unfiltered. */
#ifdef CONFIG_HAVEN_TONE_ROUTE_FDSP_MIX
#define TONE_ROUTE_DAC_DIRECT 0
#else
#define TONE_ROUTE_DAC_DIRECT 1
#endif

/* DAC_CTRL2 bits, per upstream mute(): bit 6 = mute, bit 7 = force mute. */
#define DAC_CTRL2_MUTE   0x40
#define DAC_CTRL2_UNMUTE 0x00
/* STATUS2 bit 2: input ASRC locked (upstream check_ascr_lock()). */
#define STATUS2_ASRC_LOCK ADAU1860_STATUS2_ASRCI_LOCK

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

/* Memory images longer than one I2C payload (EQ program: 57 words, EQ bank:
 * 35 words) go out as consecutive 16-word writes; the memories are
 * byte-addressed at 4 bytes per word, exactly like the FastDSP banks. */
static int mem_write_words_chunked(uint32_t addr, const uint32_t *words, size_t count)
{
	const size_t chunk = MAX_PAYLOAD / 4;

	while (count) {
		size_t n = count < chunk ? count : chunk;
		int err = mem_write_words(addr, words, n);

		if (err) {
			return err;
		}
		addr += (uint32_t)(n * 4);
		words += n;
		count -= n;
	}
	return 0;
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
	/* Explicit 0 -> 1 pulse, as ADI's own safeload routine does; upstream
	 * writes only the 1 and works, so this is belt-and-braces for the case
	 * where the bit is still latched from the previous update. */
	err = reg_write8(ADAU1860_REG_FDSP_SL_UPDATE, 0);
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
	err = wait_status2(ADAU1860_STATUS2_POWER_UP_COMPLETE, "power-up complete");
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
	err = wait_status2(ADAU1860_STATUS2_POWER_UP_COMPLETE | ADAU1860_STATUS2_FM_CLK_READY,
			   "frequency multiplier ready");
	if (err) {
		return err;
	}
	/* CM_STARTUP_OVER (bit 4) | MASTER_BLOCK_EN (bit 2) | PWR_MODE = 1
	 * (Hibernate 1) -- UG-2017: Hibernate1 + BLOCKS_ON + CM_BST_ON. Bit 6
	 * (0x40) is not a defined field in ADI's bit-field header; upstream
	 * sets it and runs, so it is kept verbatim rather than "fixed" blind. */
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
		{ ADAU1860_REG_DAC_ROUTE0, HAVEN_DAC_SOURCE_ROUTE },
		{ ADAU1860_REG_ADC_DAC_HP_PWR, 0x10 },   /* DAC/HP channel 0 on */
		{ ADAU1860_REG_HP_LVMODE_CTRL1, 0x03 },  /* HP low-voltage mode + CM */
		{ ADAU1860_REG_HP_LVMODE_CTRL3, 0x01 },
		{ ADAU1860_REG_HPLDO_CTRL, 0x01 },
		{ ADAU1860_REG_DAC_CTRL1, 0x04 },        /* 192 kHz */
		{ ADAU1860_REG_DAC_NOISE_CTRL1, 0x10 },
		{ ADAU1860_REG_DAC_NOISE_CTRL2, 0x02 },
		{ ADAU1860_REG_PB_CTRL, 0x02 },          /* high performance */
	};

	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		int err = reg_write8(seq[i].reg, seq[i].val);

		if (err) {
			return err;
		}
	}
	/* Output ceiling (replaces upstream's fixed 0xFF - 0xC0 = code 63,
	 * +0.375 dB), then unmute. */
	int err = reg_write8(ADAU1860_REG_DAC_VOL0, adau1860_dac_vol_code(output_ceiling_db));

	if (err) {
		return err;
	}
	return reg_write8(ADAU1860_REG_DAC_CTRL2, 0x00);
}

/* Lark SDK adi_lark_dac_set_volume(): "output dB = 24 - 0.375 * volume, if
 * volume is 0xff, mute DAC". Clamped so a ceiling never maps onto mute. */
uint8_t adau1860_dac_vol_code(int ceiling_db)
{
	if (ceiling_db > 24) {
		ceiling_db = 24;
	}
	if (ceiling_db < -60) {
		ceiling_db = -60;
	}
	double code = (24.0 - (double)ceiling_db) / 0.375;
	int c = (int)(code + 0.5);

	if (c > 254) {
		c = 254;
	}
	if (c < 0) {
		c = 0;
	}
	return (uint8_t)c;
}

int adau1860_control_set_output_ceiling_db(int ceiling_db)
{
	if (ceiling_db > 24 || ceiling_db < -60) {
		return -EINVAL;
	}
	output_ceiling_db = ceiling_db;
	LOG_INF("Output ceiling: %d dB (DAC_VOL0 code %u)", ceiling_db,
		adau1860_dac_vol_code(ceiling_db));
	if (!initialised) {
		return 0;
	}
	return reg_write8(ADAU1860_REG_DAC_VOL0, adau1860_dac_vol_code(ceiling_db));
}

int adau1860_control_get_output_ceiling_db(void)
{
	return output_ceiling_db;
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

/* ── Hardware EQ engine (Route B) ────────────────────────────────────────────
 * A second, independent hear-through path: DMIC0 -> EQ engine -> DAC, driven
 * entirely by registers and two parameter banks -- no FastDSP program in the
 * loop. Kept as a build-time alternative (CONFIG_HAVEN_DAC_SOURCE_EQ) so first
 * power-up has a fallback if the FastDSP program's internal routing turns out
 * not to be what the bank decode implies.
 *
 * Program: upstream's 57-word EQ program verbatim. Parameter format:
 * 28-bit two's complement 4.24, biquad words [-a1, -a2, b0, b1, b2], six
 * biquads then five unity gain words (tools/dsp/eq_bank_decode.py -- the only
 * format under which upstream's shipped banks are all stable, its unity
 * groups are identities, and its real stages decode to whole-dB cuts).
 * Updates write the INACTIVE bank in full and then flip EQ_CFG.BANK_SEL, so
 * the engine never runs on a half-written set (the EQ has no safeload). The
 * EQ runs at its source's rate (UG-2017: fs = EQ_ROUTE source), i.e. the
 * DMIC's 192 kHz here -- the same ADAU1860_FDSP_RATE_HZ the FastDSP math
 * uses. UNVERIFIED on hardware.
 */
static uint8_t eq_active_bank; /* 0 or 1: which bank EQ_CFG currently selects */

/* 28-bit two's complement, 24 fractional bits, saturating; upper 4 bits 0. */
static uint32_t q24_encode(double v)
{
	double scaled = v * 16777216.0; /* 2^24 */
	int32_t i;

	if (scaled >= 134217727.0) {
		i = 134217727;          /* 2^27 - 1 */
	} else if (scaled <= -134217728.0) {
		i = -134217728;         /* -2^27 */
	} else {
		i = (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
	}
	return (uint32_t)i & 0x0FFFFFFFu;
}

static void biquad_to_eq(const struct adau1860_biquad *c, uint32_t out[LARK_EQ_PARAMS_PER_BIQUAD])
{
	out[0] = q24_encode(-c->a1);
	out[1] = q24_encode(-c->a2);
	out[2] = q24_encode(c->b0);
	out[3] = q24_encode(c->b1);
	out[4] = q24_encode(c->b2);
}

static void eq_unity_biquad(uint32_t out[LARK_EQ_PARAMS_PER_BIQUAD])
{
	out[0] = 0;
	out[1] = 0;
	out[2] = ADAU1860_EQ_Q24_ONE;
	out[3] = 0;
	out[4] = 0;
}

/* Build a whole 35-word bank: bands into biquads 0..count-1, unity for the
 * rest, five unity gain words. NULL bands / count 0 = flat. */
static void eq_build_bank(const struct filter_band *bands, size_t count,
			  uint32_t bank[LARK_EQ_BANK_WORDS])
{
	for (size_t i = 0; i < LARK_EQ_NUM_BIQUADS; i++) {
		uint32_t *w = &bank[i * LARK_EQ_PARAMS_PER_BIQUAD];

		if (bands && i < count) {
			struct adau1860_biquad c;

			calc_band_coeffs(&bands[i], &c);
			biquad_to_eq(&c, w);
		} else {
			eq_unity_biquad(w);
		}
	}
	for (size_t i = 0; i < LARK_EQ_GAIN_WORDS; i++) {
		bank[LARK_EQ_NUM_BIQUADS * LARK_EQ_PARAMS_PER_BIQUAD + i] = ADAU1860_EQ_Q24_ONE;
	}
}

/* Write `bank` to the bank the engine is NOT using, then make it active. */
static int eq_swap_in(const uint32_t bank[LARK_EQ_BANK_WORDS])
{
	uint8_t next = eq_active_bank ? 0 : 1;
	int err = mem_write_words_chunked(ADAU1860_EQ_BANK(next), bank, LARK_EQ_BANK_WORDS);

	if (err) {
		return err;
	}
	err = reg_write8(ADAU1860_REG_EQ_CFG,
			 ADAU1860_EQ_CFG_RUN | (next ? ADAU1860_EQ_CFG_BANK_SEL : 0));
	if (err) {
		return err;
	}
	eq_active_bank = next;
	return 0;
}

/* Upstream setup_EQ() with the input moved from ASRCI0 (music) to DMIC0. */
static int configure_eq(void)
{
	uint32_t flat[LARK_EQ_BANK_WORDS];
	int err = reg_write8(ADAU1860_REG_EQ_CFG, 0x00); /* stop */

	if (err) {
		return err;
	}
	err = reg_write8(ADAU1860_REG_EQ_CFG, ADAU1860_EQ_CFG_CLEAR);
	if (err) {
		return err;
	}
	/* Bounded wait for EQ_STATUS.CLEAR_DONE (upstream spins forever). */
	{
		int64_t deadline = k_uptime_get() + 200;

		for (;;) {
			uint8_t st = 0;

			err = reg_read(ADAU1860_REG_EQ_STATUS, &st, 1);
			if (err) {
				return err;
			}
			if (st & ADAU1860_EQ_STATUS_CLEAR_DONE) {
				break;
			}
			if (k_uptime_get() > deadline) {
				LOG_ERR("Timed out waiting for EQ parameter-RAM clear (EQ_STATUS=0x%02x)", st);
				return -ETIMEDOUT;
			}
			k_usleep(100);
		}
	}
	err = reg_write8(ADAU1860_REG_EQ_ROUTE, ADAU1860_EQ_ROUTE_DMIC(0));
	if (err) {
		return err;
	}
	err = mem_write_words_chunked(ADAU1860_EQ_PROG_MEM, lark_eq_program, LARK_EQ_PROGRAM_WORDS);
	if (err) {
		return err;
	}
	eq_build_bank(NULL, 0, flat);
	err = mem_write_words_chunked(ADAU1860_EQ_BANK_0, flat, LARK_EQ_BANK_WORDS);
	if (err) {
		return err;
	}
	err = mem_write_words_chunked(ADAU1860_EQ_BANK_1, flat, LARK_EQ_BANK_WORDS);
	if (err) {
		return err;
	}
	eq_active_bank = 0;
	return reg_write8(ADAU1860_REG_EQ_CFG, ADAU1860_EQ_CFG_RUN);
}

/* ── LDL tone: codec-side routing ───────────────────────────────────────────
 * The tone itself is synthesised on the nRF (tone_gen.c) and arrives on
 * serial port 0 -> input ASRC 0, both already powered by configure_routing().
 * What the codec has to do is get that signal to the DAC:
 *
 *  TONE_ROUTE_DAC_DIRECT (default): DAC_ROUTE0 <- I2S while the tone plays,
 *    back to FastDSP ch 0 afterwards. This is upstream's non-DSP playback
 *    configuration (SAI I2S_IN + ASRCI0_EN + DAC_ROUTE0 = 0), so it is known
 *    to produce audio. Hear-through is suspended for the duration, and the
 *    tone cannot pass through the user's own notches -- for an LDL test both
 *    are what you want.
 *  FDSP mix (CONFIG_HAVEN_TONE_ROUTE_FDSP_MIX): leave the DAC on FastDSP and
 *    rely on the program's mixer slot taking the I2S path. Whether it does,
 *    and at what gain, is not established -- hardware experiment only.
 *
 * Route switches happen under DAC soft mute so they don't click, and the
 * switch *into* the tone waits for the input ASRC to lock onto the freshly
 * started I2S clock (upstream unmutes the DAC only after STATUS2 bit 2).
 */
static K_MUTEX_DEFINE(tone_route_lock);
static bool tone_route_engaged;

int32_t adau1860_tone_gain_q15(float level_db)
{
	float rel_db = level_db - TONE_FULL_SCALE_DB;

	if (rel_db >= 0.0f) {
		return TONE_GEN_GAIN_ONE;
	}
	if (rel_db < -96.0f) {
		return 0; /* below the 16-bit floor */
	}
	return (int32_t)lrint(pow(10.0, (double)rel_db / 20.0) * (double)TONE_GEN_GAIN_ONE);
}

static int tone_route_engage(void)
{
	int err = 0;

	if (!TONE_ROUTE_DAC_DIRECT || !initialised) {
		return 0;
	}
	k_mutex_lock(&tone_route_lock, K_FOREVER);
	if (!tone_route_engaged) {
		err = reg_write8(ADAU1860_REG_DAC_CTRL2, DAC_CTRL2_MUTE);
		if (!err) {
			err = reg_write8(ADAU1860_REG_DAC_ROUTE0, ADAU1860_DAC_ROUTE_I2S);
		}
		if (!err) {
			/* Bounded; a miss is logged, not fatal -- the tone may just
			 * start a few ms late or, if the I2S clock never arrives,
			 * stay silent, which the watchdog then tidies up. */
			if (wait_status2(STATUS2_ASRC_LOCK, "input ASRC lock (tone)")) {
				LOG_WRN("Input ASRC did not report lock -- is I2S0 clocking?");
			}
			err = reg_write8(ADAU1860_REG_DAC_CTRL2, DAC_CTRL2_UNMUTE);
		}
		tone_route_engaged = true;
	}
	k_mutex_unlock(&tone_route_lock);
	return err;
}

static int tone_route_disengage(void)
{
	int err = 0;

	if (!TONE_ROUTE_DAC_DIRECT || !initialised) {
		return 0;
	}
	k_mutex_lock(&tone_route_lock, K_FOREVER);
	if (tone_route_engaged) {
		err = reg_write8(ADAU1860_REG_DAC_CTRL2, DAC_CTRL2_MUTE);
		if (!err) {
			err = reg_write8(ADAU1860_REG_DAC_ROUTE0, HAVEN_DAC_SOURCE_ROUTE); /* restore the configured source */
		}
		if (!err) {
			err = reg_write8(ADAU1860_REG_DAC_CTRL2, DAC_CTRL2_UNMUTE);
		}
		tone_route_engaged = false;
	}
	k_mutex_unlock(&tone_route_lock);
	return err;
}

/* Runs on the tone_gen feeder thread once the I2S link has actually gone
 * quiet (ramp-down played out, peripheral stopped). */
static void on_tone_stopped(void)
{
	int err = tone_route_disengage();

	if (err) {
		LOG_ERR("Restoring hear-through route after tone failed: %d", err);
	} else {
		LOG_INF("Tone finished -- hear-through route restored");
	}
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

int adau1860_control_init(void)
{
	int err;

	tone_gen_set_stopped_callback(on_tone_stopped);

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
	if (HAVEN_USE_EQ_ENGINE) {
		err = configure_eq();
		if (err) {
			return err;
		}
	}
	err = configure_dac();
	if (err) {
		return err;
	}
	/* Boot flat: upstream's bank ships a transparency EQ in slots 0-4;
	 * Haven's bands replace it, so start from pass-through rather than
	 * someone else's curve. (Done even on the EQ route -- the FastDSP keeps
	 * running so its output is well-defined if anything routes from it.) */
	err = set_all_biquads_unity();
	if (err) {
		return err;
	}

	initialised = true;
	LOG_INF("ADAU1860 up: FastDSP bank %d running, DAC source %s, fs %u Hz, output ceiling %d dB",
		FDSP_BANK, HAVEN_DAC_SOURCE_NAME, (unsigned int)ADAU1860_FDSP_RATE_HZ,
		output_ceiling_db);
	return 0;
}

int adau1860_control_apply_filters(const struct filter_band *bands, size_t count)
{
	int err = 0;

	if (count > PROTOCOL_MAX_BANDS) {
		count = PROTOCOL_MAX_BANDS;
	}

	if (HAVEN_USE_EQ_ENGINE) {
		uint32_t bank[LARK_EQ_BANK_WORDS];

		for (size_t i = 0; i < count; i++) {
			LOG_INF("Band %u: f0=%.1f Hz Q=%.1f atten=%.1f dB (EQ engine)", (unsigned int)i,
				(double)bands[i].f0_hz, (double)bands[i].q, (double)bands[i].atten_db);
		}
		eq_build_bank(bands, count, bank);
		return initialised ? eq_swap_in(bank) : 0;
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
	if (HAVEN_USE_EQ_ENGINE) {
		uint32_t flat[LARK_EQ_BANK_WORDS];

		eq_build_bank(NULL, 0, flat);
		return eq_swap_in(flat);
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
	/* Caller (tone_safety.c) has already clamped level_db to
	 * [PROTOCOL_TONE_LEVEL_MIN_DB, PROTOCOL_TONE_LEVEL_MAX_DB]. */
	int32_t gain = adau1860_tone_gain_q15(level_db);

	LOG_INF("Tone: f0=%.1f Hz level=%.1f dB -> gain %ld/32768 (%s)", (double)f0_hz,
		(double)level_db, (long)gain,
		TONE_ROUTE_DAC_DIRECT ? "DAC<-I2S, hear-through paused" : "FDSP mix");

	/* I2S first so the codec's ASRC has a clock to lock to, then route. */
	int err = tone_gen_start(f0_hz, gain);

	if (err) {
		LOG_ERR("Tone generator start failed: %d", err);
		return err;
	}
	return tone_route_engage();
}

int adau1860_control_set_tone_level(float level_db)
{
	int32_t gain = adau1860_tone_gain_q15(level_db);

	LOG_INF("Tone level: %.1f dB -> gain %ld/32768", (double)level_db, (long)gain);
	return tone_gen_set_gain(gain);
}

int adau1860_control_stop_tone(void)
{
	LOG_INF("Tone stop");
	int err = tone_gen_stop();

	if (err) {
		/* No generator (bench without I2S, or init failed): nothing is
		 * playing, but make sure the codec isn't left routed to I2S. */
		return tone_route_disengage();
	}
	/* Route restore follows from the feeder thread (on_tone_stopped) once
	 * the ramp-down has actually played. */
	return 0;
}

void adau1860_control_on_ble_connected(void)
{
	LOG_INF("BLE connected -- filters unchanged");
}

void adau1860_control_on_ble_disconnected(void)
{
	/* Filters keep running -- hearing protection must not depend on the
	 * phone. The tone must not either: main.c's tone_safety_stop() already
	 * stops the generator; this is the second layer, restoring the codec
	 * route synchronously (under soft mute) even if the feeder thread's
	 * callback is late or never comes. Idempotent. */
	int err = tone_route_disengage();

	if (err) {
		LOG_ERR("Route restore on BLE disconnect failed: %d", err);
	}
	LOG_INF("BLE disconnected -- filters kept running, tone route restored");
}
