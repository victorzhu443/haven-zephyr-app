/* Host tests for the hardware-EQ hear-through path ("Route B",
 * CONFIG_HAVEN_DAC_SOURCE_EQ).
 *
 * The real adau1860_control.c is compiled with the EQ route selected, so
 * these assert the exact register traffic of configure_eq(), the 4.24
 * encoder against upstream's shipped EQ banks, and the write-inactive-then-
 * flip bank discipline of apply_filters()/bypass. Nothing here has run on a
 * codec; it pins what the firmware WILL do.
 */
#define CONFIG_HAVEN_DAC_SOURCE_EQ 1

#include "test_harness.h"

#include <math.h>
#include <string.h>

#include "../../src/lark_fdsp_program.c"
#include "../../src/lark_eq_program.c"
#include "../../src/tone_gen.c"
#include "../../src/adau1860_control.c"

const struct device haven_fake_i2c_bus_dev = { .name = "fake-i2c" };
const struct device haven_fake_gpio_port_dev = { .name = "fake-gpio" };

static uint32_t payload_word(const struct haven_fake_i2c_xfer *x, int i)
{
	return sys_get_le32(&x->data[4 * i]);
}

/* 28-bit two's complement, 24 fractional bits. */
static double q24_decode(uint32_t w)
{
	int32_t v = (int32_t)(w & 0x0FFFFFFFu);

	if (v & 0x08000000) {
		v -= 0x10000000;
	}
	return (double)v / 16777216.0;
}

static double pole_radius(double A1, double A2)
{
	double disc = A1 * A1 - 4.0 * A2;

	if (disc < 0.0) {
		return sqrt(A2);
	}
	double r1 = fabs((-A1 + sqrt(disc)) / 2.0);
	double r2 = fabs((-A1 - sqrt(disc)) / 2.0);

	return r1 > r2 ? r1 : r2;
}

/* Sum the words of every write into [addr, addr+len) -- chunked images land
 * as several consecutive writes. Returns the number of words captured. */
static size_t collect_image(uint32_t base, uint32_t *out, size_t max_words)
{
	size_t n = 0;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		const struct haven_fake_i2c_xfer *x = &haven_fake_i2c_log[i];

		if (x->reg >= base && x->reg < base + max_words * 4 && (x->reg - base) % 4 == 0) {
			size_t off = (x->reg - base) / 4;

			for (size_t w = 0; w < x->len / 4 && off + w < max_words; w++) {
				out[off + w] = payload_word(x, (int)w);
				if (off + w + 1 > n) {
					n = off + w + 1;
				}
			}
		}
	}
	return n;
}

/* ── 4.24 encoder ─────────────────────────────────────────────────────────── */

static void test_q24_encode_basics(void)
{
	CHECK(q24_encode(1.0) == 0x01000000u);
	CHECK(q24_encode(-1.0) == 0x0F000000u);
	CHECK(q24_encode(0.5) == 0x00800000u);
	CHECK(q24_encode(0.0) == 0u);
	/* Saturation stays inside 28 bits, upper nibble clear. */
	CHECK(q24_encode(100.0) == 0x07FFFFFFu);
	CHECK(q24_encode(-100.0) == 0x08000000u);
	CHECK((q24_encode(-1.99) & 0xF0000000u) == 0u);
	CHECK_FLOAT_NEAR(q24_decode(q24_encode(-1.99)), -1.99, 1e-6);
}

static void test_unity_biquad_matches_upstream_unity_group(void)
{
	/* Upstream bank 0, groups 4 and 5 are {0,0,0x01000000,0,0}. */
	uint32_t u[LARK_EQ_PARAMS_PER_BIQUAD];

	eq_unity_biquad(u);
	for (int i = 0; i < LARK_EQ_PARAMS_PER_BIQUAD; i++) {
		CHECK(u[i] == lark_eq_param_bank0[4 * LARK_EQ_PARAMS_PER_BIQUAD + i]);
		CHECK(u[i] == lark_eq_param_bank0[5 * LARK_EQ_PARAMS_PER_BIQUAD + i]);
	}
	/* And the gain block is five unity words in both upstream banks. */
	for (int i = 0; i < LARK_EQ_GAIN_WORDS; i++) {
		CHECK(lark_eq_param_bank0[30 + i] == ADAU1860_EQ_Q24_ONE);
		CHECK(lark_eq_param_bank1[30 + i] == ADAU1860_EQ_Q24_ONE);
	}
}

/* Under [-a1, -a2, b0, b1, b2] / 4.24 every real upstream stage is stable and
 * has a plausible EQ magnitude (whole-dB cuts, no boost above +1 dB). Under
 * the FastDSP's layout/format the same words are not. */
static void test_upstream_eq_banks_decode_stable_under_chosen_layout(void)
{
	const uint32_t *banks[2] = { lark_eq_param_bank0, lark_eq_param_bank1 };

	for (int b = 0; b < 2; b++) {
		for (int i = 0; i < LARK_EQ_NUM_BIQUADS; i++) {
			const uint32_t *g = &banks[b][i * LARK_EQ_PARAMS_PER_BIQUAD];
			double a1 = -q24_decode(g[0]), a2 = -q24_decode(g[1]);
			double b0 = q24_decode(g[2]), b1 = q24_decode(g[3]), b2 = q24_decode(g[4]);

			if (a1 == 0.0 && a2 == 0.0) {
				CHECK(b0 == 1.0 && b1 == 0.0 && b2 == 0.0);
				continue;
			}
			CHECK(pole_radius(a1, a2) < 1.0);
			/* DC gain within [-12, +1] dB. */
			double dc = (b0 + b1 + b2) / (1.0 + a1 + a2);
			double dc_db = 20.0 * log10(fabs(dc));

			CHECK(dc_db > -12.0 && dc_db < 1.0);
		}
	}
	/* Negative control: read as-is (not negated) the first stage is unstable. */
	const uint32_t *g0 = lark_eq_param_bank0;

	CHECK(pole_radius(q24_decode(g0[0]), q24_decode(g0[1])) >= 1.0);
}

static void test_haven_notch_round_trips_through_q24(void)
{
	struct filter_band band = { .f0_hz = 4500.0f, .q = 10.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct adau1860_biquad c;
	uint32_t w[LARK_EQ_PARAMS_PER_BIQUAD];

	calc_band_coeffs(&band, &c);
	biquad_to_eq(&c, w);
	CHECK_FLOAT_NEAR(q24_decode(w[0]), -c.a1, 2e-7);
	CHECK_FLOAT_NEAR(q24_decode(w[1]), -c.a2, 2e-7);
	CHECK_FLOAT_NEAR(q24_decode(w[2]), c.b0, 2e-7);
	CHECK_FLOAT_NEAR(q24_decode(w[3]), c.b1, 2e-7);
	CHECK_FLOAT_NEAR(q24_decode(w[4]), c.b2, 2e-7);
	/* Notch coefficients are within 4.24 range with margin (|x| < 2). */
	for (int i = 0; i < 5; i++) {
		CHECK((w[i] & 0xF0000000u) == 0u);
	}
}

/* ── Register traffic ─────────────────────────────────────────────────────── */

static void test_init_configures_eq_route(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	eq_active_bank = 1; /* stale on purpose; configure_eq must reset it */

	CHECK(adau1860_control_init() == 0);
	CHECK(initialised);

	/* EQ_CFG sequence: stop (0), clear (0x10), ..., run bank 0 (0x01). */
	int seen_stop = 0, seen_clear = 0;
	const struct haven_fake_i2c_xfer *last_cfg = NULL;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		const struct haven_fake_i2c_xfer *x = &haven_fake_i2c_log[i];

		if (x->reg == ADAU1860_REG_EQ_CFG) {
			if (x->data[0] == 0x00 && !seen_clear) {
				seen_stop = 1;
			}
			if (x->data[0] == ADAU1860_EQ_CFG_CLEAR && seen_stop) {
				seen_clear = 1;
			}
			last_cfg = x;
		}
	}
	CHECK(seen_stop && seen_clear);
	CHECK(last_cfg && last_cfg->data[0] == ADAU1860_EQ_CFG_RUN);
	CHECK(eq_active_bank == 0);

	const struct haven_fake_i2c_xfer *route = haven_fake_i2c_last_write(ADAU1860_REG_EQ_ROUTE);

	CHECK(route && route->data[0] == ADAU1860_EQ_ROUTE_DMIC(0)); /* 71 */

	const struct haven_fake_i2c_xfer *dac = haven_fake_i2c_last_write(ADAU1860_REG_DAC_ROUTE0);

	CHECK(dac && dac->data[0] == ADAU1860_DAC_ROUTE_EQ); /* 75 */

	/* Program image arrives complete and verbatim, in <=16-word chunks. */
	uint32_t img[LARK_EQ_PROGRAM_WORDS];

	memset(img, 0xEE, sizeof(img));
	CHECK(collect_image(ADAU1860_EQ_PROG_MEM, img, LARK_EQ_PROGRAM_WORDS) == LARK_EQ_PROGRAM_WORDS);
	CHECK(memcmp(img, lark_eq_program, sizeof(img)) == 0);

	/* Both banks were written flat at boot. */
	uint32_t bank[LARK_EQ_BANK_WORDS], flat[LARK_EQ_BANK_WORDS];

	eq_build_bank(NULL, 0, flat);
	CHECK(collect_image(ADAU1860_EQ_BANK_0, bank, LARK_EQ_BANK_WORDS) == LARK_EQ_BANK_WORDS);
	CHECK(memcmp(bank, flat, sizeof(bank)) == 0);
	CHECK(collect_image(ADAU1860_EQ_BANK_1, bank, LARK_EQ_BANK_WORDS) == LARK_EQ_BANK_WORDS);
	CHECK(memcmp(bank, flat, sizeof(bank)) == 0);

	/* No FastDSP safeload of Haven bands happened on the EQ route beyond the
	 * five boot-time unity loads. */
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_UPDATE) == 2 * LARK_FDSP_NUM_BIQUADS);
}

static void test_apply_filters_writes_inactive_bank_then_flips(void)
{
	struct filter_band bands[2] = {
		{ .f0_hz = 1000.0f, .q = 10.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB },
		{ .f0_hz = 4500.0f, .q = 5.0f, .atten_db = 20.0f },
	};

	haven_fake_i2c_reset();
	initialised = true;
	eq_active_bank = 0;

	CHECK(adau1860_control_apply_filters(bands, 2) == 0);
	CHECK(eq_active_bank == 1);

	/* Bank 1 (the inactive one) received the whole image; bank 0 untouched. */
	uint32_t got[LARK_EQ_BANK_WORDS], want[LARK_EQ_BANK_WORDS];

	CHECK(collect_image(ADAU1860_EQ_BANK_1, got, LARK_EQ_BANK_WORDS) == LARK_EQ_BANK_WORDS);
	eq_build_bank(bands, 2, want);
	CHECK(memcmp(got, want, sizeof(got)) == 0);
	CHECK(collect_image(ADAU1860_EQ_BANK_0, got, LARK_EQ_BANK_WORDS) == 0);

	/* Stages 2-5 unity, gains unity. */
	for (int i = 2; i < LARK_EQ_NUM_BIQUADS; i++) {
		CHECK(want[i * 5 + 2] == ADAU1860_EQ_Q24_ONE && want[i * 5] == 0 && want[i * 5 + 4] == 0);
	}
	/* The flip is the LAST EQ_CFG write and selects bank 1 while running. */
	const struct haven_fake_i2c_xfer *cfg = haven_fake_i2c_last_write(ADAU1860_REG_EQ_CFG);

	CHECK(cfg && cfg->data[0] == (ADAU1860_EQ_CFG_RUN | ADAU1860_EQ_CFG_BANK_SEL));
	/* Ordering: every bank-1 write precedes the flip. */
	size_t flip_idx = 0, last_bank_idx = 0;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		if (haven_fake_i2c_log[i].reg == ADAU1860_REG_EQ_CFG) {
			flip_idx = i;
		}
		if (haven_fake_i2c_log[i].reg >= ADAU1860_EQ_BANK_1 &&
		    haven_fake_i2c_log[i].reg < ADAU1860_EQ_BANK_1 + LARK_EQ_BANK_WORDS * 4) {
			last_bank_idx = i;
		}
	}
	CHECK(last_bank_idx < flip_idx);
	/* No FastDSP safeloads on this route. */
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_UPDATE) == 0);

	/* Second apply goes to bank 0 and flips back. */
	haven_fake_i2c_reset();
	CHECK(adau1860_control_apply_filters(bands, 1) == 0);
	CHECK(eq_active_bank == 0);
	cfg = haven_fake_i2c_last_write(ADAU1860_REG_EQ_CFG);
	CHECK(cfg && cfg->data[0] == ADAU1860_EQ_CFG_RUN);
	CHECK(collect_image(ADAU1860_EQ_BANK_0, got, LARK_EQ_BANK_WORDS) == LARK_EQ_BANK_WORDS);
}

static void test_bypass_swaps_in_flat_bank(void)
{
	haven_fake_i2c_reset();
	initialised = true;
	eq_active_bank = 0;

	CHECK(adau1860_control_set_bypass(true) == 0);
	CHECK(eq_active_bank == 1);

	uint32_t got[LARK_EQ_BANK_WORDS], flat[LARK_EQ_BANK_WORDS];

	eq_build_bank(NULL, 0, flat);
	CHECK(collect_image(ADAU1860_EQ_BANK_1, got, LARK_EQ_BANK_WORDS) == LARK_EQ_BANK_WORDS);
	CHECK(memcmp(got, flat, sizeof(got)) == 0);

	/* Bypass off is a no-op (main.c re-sends bands). */
	haven_fake_i2c_reset();
	CHECK(adau1860_control_set_bypass(false) == 0);
	CHECK(haven_fake_i2c_log_count == 0);
}

static void test_apply_without_codec_is_math_only(void)
{
	struct filter_band band = { .f0_hz = 2000.0f, .q = 3.0f, .atten_db = 10.0f };

	haven_fake_i2c_reset();
	initialised = false;
	CHECK(adau1860_control_apply_filters(&band, 1) == 0);
	CHECK(haven_fake_i2c_log_count == 0);
}

int main(void)
{
	RUN(test_q24_encode_basics);
	RUN(test_unity_biquad_matches_upstream_unity_group);
	RUN(test_upstream_eq_banks_decode_stable_under_chosen_layout);
	RUN(test_haven_notch_round_trips_through_q24);
	RUN(test_init_configures_eq_route);
	RUN(test_apply_filters_writes_inactive_bank_then_flips);
	RUN(test_bypass_swaps_in_flat_bank);
	RUN(test_apply_without_codec_is_math_only);
	return haven_test_summary("test_eq_route");
}
