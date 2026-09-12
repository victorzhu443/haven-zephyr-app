/* Host tests for adau1860_control.c: the REAL production file is included
 * below (its devicetree macros now resolve against small fakes in
 * fakes/zephyr/device.h, drivers/i2c.h and drivers/gpio.h -- the earlier
 * "verbatim copy of calc_band_coeffs()" workaround is gone, so a formula
 * change here is caught automatically).
 *
 * Three layers are covered:
 *  1. Coefficient math (RBJ notch / peaking cut) -- functional properties.
 *  2. Q5.27 encoding + the FastDSP sign convention, pinned to upstream
 *     OpenEarable data: (a) the nRF-side software EQ row from
 *     Equalizer.cpp, whose f/G/Q annotation lets us recompute it exactly;
 *     (b) the shipped FastDSP parameter banks, which decode to stable
 *     filters only if the feedback taps are stored negated.
 *  3. Register traffic -- the fake I2C layer records every write, so the
 *     bring-up sequence and the safeload slot writes are asserted on bytes.
 */
#include "test_harness.h"

#include <math.h>
#include <string.h>

#include "../../src/lark_fdsp_program.c"
#include "../../src/adau1860_control.c"

const struct device haven_fake_i2c_bus_dev = { .name = "fake-i2c" };
const struct device haven_fake_gpio_port_dev = { .name = "fake-gpio" };

static double q27_decode(uint32_t w)
{
	return (double)(int32_t)w / 134217728.0;
}

/* |pole| of z^2 + A1 z + A2 (denominator 1 + A1 z^-1 + A2 z^-2). */
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

/* Direct-form apply for functional response testing. */
static double apply_biquad_peak(const struct adau1860_biquad *c, double freq_hz,
				double sample_rate_hz, int settle_buffers, int buf_len)
{
	double z1 = 0.0, z2 = 0.0, phase = 0.0;
	double w = 2.0 * M_PI * (freq_hz / sample_rate_hz);
	double peak = 0.0;

	for (int b = 0; b < settle_buffers; b++) {
		peak = 0.0;
		for (int i = 0; i < buf_len; i++) {
			double x = sin(phase);

			phase += w;
			double y = c->b0 * x + z1;

			z1 = c->b1 * x - c->a1 * y + z2;
			z2 = c->b2 * x - c->a2 * y;
			if (fabs(y) > peak) {
				peak = fabs(y);
			}
		}
	}
	return peak;
}

/* ── 1. coefficient math ─────────────────────────────────────────────────── */

static void test_notch_structural_identity(void)
{
	struct filter_band band = { .f0_hz = 4000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct adau1860_biquad c;

	calc_band_coeffs(&band, &c);
	CHECK_FLOAT_NEAR(c.b1, c.a1, 1e-6f);
	CHECK_FLOAT_NEAR(c.b0, c.b2, 1e-6f);
}

static void test_zero_cut_peaking_is_identity_filter(void)
{
	struct filter_band band = { .f0_hz = 3000.0f, .q = 4.0f, .atten_db = 0.0f };
	struct adau1860_biquad c;

	calc_band_coeffs(&band, &c);
	CHECK_FLOAT_NEAR(c.b0, 1.0, 1e-6f);
	CHECK_FLOAT_NEAR(c.b1, c.a1, 1e-6f);
	CHECK_FLOAT_NEAR(c.b2, c.a2, 1e-6f);
}

static void test_notch_rejects_f0_passes_far_off(void)
{
	struct filter_band band = { .f0_hz = 4000.0f, .q = 8.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct adau1860_biquad c;

	calc_band_coeffs(&band, &c);

	double fs = (double)ADAU1860_FDSP_RATE_HZ;
	double peak_at_notch = apply_biquad_peak(&c, 4000.0, fs, 60, 512);
	double peak_far_off = apply_biquad_peak(&c, 1000.0, fs, 60, 512);

	CHECK(peak_at_notch < 0.1);
	CHECK(peak_far_off > 0.85);
}

static void test_partial_peaking_cut_attenuates_less_than_full_notch(void)
{
	struct filter_band notch_band = { .f0_hz = 4000.0f, .q = 8.0f, .atten_db = 40.0f };
	struct filter_band cut12_band = { .f0_hz = 4000.0f, .q = 8.0f, .atten_db = 12.0f };
	struct adau1860_biquad c_notch, c_cut12;

	calc_band_coeffs(&notch_band, &c_notch);
	calc_band_coeffs(&cut12_band, &c_cut12);

	double fs = (double)ADAU1860_FDSP_RATE_HZ;
	double peak_notch = apply_biquad_peak(&c_notch, 4000.0, fs, 60, 512);
	double peak_cut12 = apply_biquad_peak(&c_cut12, 4000.0, fs, 60, 512);

	CHECK(peak_cut12 > peak_notch);
	CHECK_FLOAT_NEAR(peak_cut12, 0.251, 0.05f);
}

static void test_worst_case_band_survives_q27_at_fdsp_rate(void)
{
	/* Narrowest, lowest band the protocol allows: after Q5.27 rounding the
	 * poles must still be inside the unit circle with margin, and the
	 * quantised notch must still reject f0. */
	struct filter_band band = { .f0_hz = PROTOCOL_F0_MIN_HZ, .q = PROTOCOL_Q_MAX,
				    .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct adau1860_biquad c, q;
	struct adau1860_fdsp_biquad w;

	calc_band_coeffs(&band, &c);
	biquad_to_fdsp(&c, &w);
	q.b0 = q27_decode(w.p[0]);
	q.b1 = q27_decode(w.p[1]);
	q.b2 = q27_decode(w.p[2]);
	q.a1 = -q27_decode(w.p[3]);
	q.a2 = -q27_decode(w.p[4]);

	double r = pole_radius(q.a1, q.a2);

	CHECK(r < 1.0);
	CHECK(r > 0.999); /* it IS a narrow notch: poles near, not on, the circle */

	double fs = (double)ADAU1860_FDSP_RATE_HZ;
	/* Long settle: bandwidth is f0/Q = 10 Hz -> ~30 ms time constant. */
	double at_f0 = apply_biquad_peak(&q, PROTOCOL_F0_MIN_HZ, fs, 40, 8192);

	CHECK(at_f0 < 0.2);
}

/* ── 2. encoding + sign convention ───────────────────────────────────────── */

static void test_q27_encode_basics(void)
{
	CHECK(q27_encode(1.0) == 0x08000000u);
	CHECK(q27_encode(-1.0) == 0xF8000000u);
	CHECK(q27_encode(0.5) == 0x04000000u);
	CHECK(q27_encode(0.0) == 0u);
	CHECK(q27_encode(1.0 / 134217728.0) == 1u);
	CHECK(q27_encode(20.0) == 0x7FFFFFFFu);  /* saturates, no wrap */
	CHECK(q27_encode(-20.0) == 0x80000000u);
	/* Round-trips an upstream bank word exactly (bank 1 slot 0, p3). */
	CHECK(q27_encode(q27_decode(0x0FF5C5FDu)) == 0x0FF5C5FDu);
	CHECK(q27_encode(q27_decode(0xF80A321Au)) == 0xF80A321Au);
}

static void test_haven_notch_round_trips_through_q27(void)
{
	/* A Haven band's own coefficients: encode, decode, re-encode must be a
	 * fixed point, and the decoded values must sit within half an LSB of
	 * the doubles they came from. */
	struct filter_band band = { .f0_hz = 4500.0f, .q = 10.0f, .atten_db = 20.0f };
	struct adau1860_biquad c;
	struct adau1860_fdsp_biquad w, w2;

	calc_band_coeffs(&band, &c);
	biquad_to_fdsp(&c, &w);
	struct adau1860_biquad d = {
		.b0 = q27_decode(w.p[0]), .b1 = q27_decode(w.p[1]), .b2 = q27_decode(w.p[2]),
		.a1 = -q27_decode(w.p[3]), .a2 = -q27_decode(w.p[4]),
	};

	biquad_to_fdsp(&d, &w2);
	for (int k = 0; k < 5; k++) {
		CHECK(w.p[k] == w2.p[k]);
	}
	const double half_lsb = 0.5 / 134217728.0 + 1e-15;

	CHECK(fabs(d.b0 - c.b0) <= half_lsb);
	CHECK(fabs(d.b1 - c.b1) <= half_lsb);
	CHECK(fabs(d.b2 - c.b2) <= half_lsb);
	CHECK(fabs(d.a1 - c.a1) <= half_lsb);
	CHECK(fabs(d.a2 - c.a2) <= half_lsb);
}

static int within_lsb(uint32_t a, uint32_t b, int lsb)
{
	int64_t d = (int64_t)(int32_t)a - (int64_t)(int32_t)b;

	return d <= lsb && d >= -lsb;
}

static void test_golden_upstream_equalizer_row_150hz(void)
{
	/* OpenEarable Equalizer.cpp row: "f: 150 G: -8.0dB Q: 1.0" ->
	 * {0x07ED1CFE, 0xF03F890A, 0x07D420FF, 0xF03F890A, 0x07C13DFD}, RBJ
	 * peaking at 48 kHz, RBJ sign convention (that table feeds a software
	 * biquad that SUBTRACTS its a-taps). Pins the Q5.27 scale and the
	 * b-coefficient order; the FastDSP encoder must reproduce b0..b2 and
	 * store the a-taps NEGATED. */
	double fs = 48000.0, f0 = 150.0, g = -8.0, Q = 1.0;
	double A = pow(10.0, g / 40.0);
	double w0 = 2.0 * M_PI * f0 / fs;
	double alpha = sin(w0) / (2.0 * Q);
	double a0 = 1.0 + alpha / A;
	struct adau1860_biquad c = {
		.b0 = (1.0 + alpha * A) / a0,
		.b1 = -2.0 * cos(w0) / a0,
		.b2 = (1.0 - alpha * A) / a0,
		.a1 = -2.0 * cos(w0) / a0,
		.a2 = (1.0 - alpha / A) / a0,
	};
	struct adau1860_fdsp_biquad w;

	biquad_to_fdsp(&c, &w);
	/* Upstream's table differs from a double-precision RBJ evaluation by
	 * up to ~3e-6 (a few hundred LSB -- it was evidently generated in
	 * single precision). 1e-5 still discriminates hard: a wrong Q format
	 * is off by a factor of 8, a wrong sign by 2x the magnitude. */
	const int tol = 1342; /* 1e-5 in Q5.27 */

	CHECK(within_lsb(w.p[0], 0x07ED1CFEu, tol));
	CHECK(within_lsb(w.p[1], 0xF03F890Au, tol));
	CHECK(within_lsb(w.p[2], 0x07D420FFu, tol));
	/* -a1, -a2 */
	CHECK(within_lsb(w.p[3], (uint32_t)(-(int32_t)0xF03F890Au), tol));
	CHECK(within_lsb(w.p[4], (uint32_t)(-(int32_t)0x07C13DFDu), tol));
	/* ...and the un-negated convention is rejected. */
	CHECK(!within_lsb(w.p[3], 0xF03F890Au, tol));
}

static void test_upstream_fdsp_banks_only_stable_with_negated_feedback(void)
{
	/* Every non-trivial biquad slot in upstream's shipped banks: stable
	 * (|pole| < 1) when p3/p4 are read as -a1/-a2, unstable (|pole| > 1)
	 * when read as a1/a2. This is the evidence for biquad_to_fdsp()'s
	 * negation. */
	int checked = 0;

	for (int bank = 0; bank < ADAU1860_FDSP_NUM_BANKS; bank++) {
		for (int slot = 0; slot < LARK_FDSP_NUM_BIQUADS; slot++) {
			double p3 = q27_decode(lark_fdsp_param_bank[bank][3][slot]);
			double p4 = q27_decode(lark_fdsp_param_bank[bank][4][slot]);

			if (p3 == 0.0 && p4 == 0.0) {
				continue; /* zeroed or unity slot */
			}
			CHECK(pole_radius(-p3, -p4) < 1.0);
			CHECK(pole_radius(p3, p4) > 1.0);
			checked++;
		}
	}
	CHECK(checked >= 5);
}

/* ── 3. register traffic ─────────────────────────────────────────────────── */

static uint32_t payload_word(const struct haven_fake_i2c_xfer *x, int i)
{
	return sys_get_le32(&x->data[4 * i]);
}

static void test_init_sequence_on_fake_codec(void)
{
	haven_fake_i2c_reset();
	initialised = false;

	int err = adau1860_control_init();

	CHECK(err == 0);
	CHECK(initialised);
	CHECK(haven_fake_gpio_configure_calls >= 1);
	CHECK(haven_fake_gpio_last_flags == GPIO_OUTPUT_ACTIVE);

	const struct haven_fake_i2c_xfer *prog = haven_fake_i2c_last_write(ADAU1860_FDSP_PROG_MEM);

	CHECK(prog != NULL);
	if (prog) {
		CHECK(prog->len == LARK_FDSP_PROGRAM_WORDS * 4);
		CHECK(payload_word(prog, 0) == lark_fdsp_program[0]);
		CHECK(payload_word(prog, LARK_FDSP_PROGRAM_WORDS - 1) ==
		      lark_fdsp_program[LARK_FDSP_PROGRAM_WORDS - 1]);
	}
	/* 3 banks x 5 params, each a 12-word block at its own address. */
	int bank_blocks = 0;

	for (int k = 0; k < ADAU1860_FDSP_NUM_BANKS; k++) {
		for (int n = 0; n < ADAU1860_FDSP_NUM_PARAMS; n++) {
			const struct haven_fake_i2c_xfer *b =
				haven_fake_i2c_last_write(ADAU1860_FDSP_BANK(k, n));

			if (b && b->len == LARK_FDSP_NUM_SLOTS * 4 &&
			    payload_word(b, 6) == lark_fdsp_param_bank[k][n][6]) {
				bank_blocks++;
			}
		}
	}
	CHECK(bank_blocks == 15);

	const struct haven_fake_i2c_xfer *run = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_RUN);
	const struct haven_fake_i2c_xfer *route = haven_fake_i2c_last_write(ADAU1860_REG_DAC_ROUTE0);
	const struct haven_fake_i2c_xfer *ctrl1 = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_CTRL1);
	const struct haven_fake_i2c_xfer *unmute = haven_fake_i2c_last_write(ADAU1860_REG_DAC_CTRL2);
	const struct haven_fake_i2c_xfer *rate = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_CTRL4);

	CHECK(run && run->data[0] == 0x01);
	CHECK(route && route->data[0] == ADAU1860_DAC_ROUTE_FDSP_CH(0));
	CHECK(ctrl1 && (ctrl1->data[0] & 0x03) == FDSP_BANK);
	CHECK(unmute && unmute->data[0] == 0x00);
	CHECK(rate && rate->data[0] == 2);

	/* Boot flat: five unity safeloads after the program is running. */
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_UPDATE) == 2 * LARK_FDSP_NUM_BIQUADS /* 0->1 pulse per safeload */);
	const struct haven_fake_i2c_xfer *sl = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_SL_P0_0);

	CHECK(sl && sl->len == 20 && payload_word(sl, 0) == ADAU1860_Q27_ONE &&
	      payload_word(sl, 3) == 0);
}

static void test_init_fails_cleanly_when_codec_absent(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	haven_fake_i2c_fail_writes = 1;

	int err = adau1860_control_init();

	CHECK(err < 0);
	CHECK(!initialised);
	haven_fake_i2c_fail_writes = 0;
}

static void test_apply_filters_safeloads_bands_then_unity(void)
{
	struct filter_band bands[2] = {
		{ .f0_hz = 4500.0f, .q = 10.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB },
		{ .f0_hz = 1000.0f, .q = 2.0f, .atten_db = 12.0f },
	};
	struct adau1860_biquad c0;
	struct adau1860_fdsp_biquad w0;

	calc_band_coeffs(&bands[0], &c0);
	biquad_to_fdsp(&c0, &w0);

	haven_fake_i2c_reset();
	initialised = true;

	int err = adau1860_control_apply_filters(bands, 2);

	CHECK(err == 0);
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_ADDR) == LARK_FDSP_NUM_BIQUADS);
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_UPDATE) == 2 * LARK_FDSP_NUM_BIQUADS /* 0->1 pulse per safeload */);

	/* Walk the log: each SL_ADDR write is followed by its 20-byte payload. */
	int slot_seen = 0;

	for (size_t i = 0; i + 1 < haven_fake_i2c_log_count; i++) {
		if (haven_fake_i2c_log[i].reg != ADAU1860_REG_FDSP_SL_ADDR) {
			continue;
		}
		uint8_t slot = haven_fake_i2c_log[i].data[0];
		const struct haven_fake_i2c_xfer *p = &haven_fake_i2c_log[i + 1];

		CHECK(slot == slot_seen);
		CHECK(p->reg == ADAU1860_REG_FDSP_SL_P0_0 && p->len == 20);
		if (slot == 0) {
			for (int k = 0; k < 5; k++) {
				CHECK(payload_word(p, k) == w0.p[k]);
			}
		} else if (slot >= 2) {
			CHECK(payload_word(p, 0) == ADAU1860_Q27_ONE);
			for (int k = 1; k < 5; k++) {
				CHECK(payload_word(p, k) == 0);
			}
		}
		slot_seen++;
	}
	CHECK(slot_seen == LARK_FDSP_NUM_BIQUADS);
}

static void test_bypass_writes_unity_everywhere_and_off_is_noop(void)
{
	haven_fake_i2c_reset();
	initialised = true;

	CHECK(adau1860_control_set_bypass(true) == 0);
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_UPDATE) == 2 * LARK_FDSP_NUM_BIQUADS /* 0->1 pulse per safeload */);

	haven_fake_i2c_reset();
	CHECK(adau1860_control_set_bypass(false) == 0);
	CHECK(haven_fake_i2c_log_count == 0);
}

static void test_volume_mapping_matches_upstream(void)
{
	haven_fake_i2c_reset();
	initialised = true;

	CHECK(adau1860_control_set_volume_pct(100) == 0);
	const struct haven_fake_i2c_xfer *sl = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_SL_P0_0);
	const struct haven_fake_i2c_xfer *addr = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_SL_ADDR);

	CHECK(addr && addr->data[0] == LARK_FDSP_SLOT_VOLUME);
	CHECK(sl && payload_word(sl, 4) == ADAU1860_Q27_ONE);
	/* other four words untouched from the shipped bank */
	CHECK(sl && payload_word(sl, 0) == lark_fdsp_param_bank[FDSP_BANK][0][LARK_FDSP_SLOT_VOLUME]);

	haven_fake_i2c_reset();
	CHECK(adau1860_control_set_volume_pct(0) == 0);
	sl = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_SL_P0_0);
	/* -60 dB = 0.001 * 2^27 = 134217.7 -> 0x00020C4A */
	CHECK(sl && within_lsb(payload_word(sl, 4), 0x00020C4Au, 1));

	haven_fake_i2c_reset();
	CHECK(adau1860_control_set_mute(true) == 0);
	sl = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_SL_P0_0);
	addr = haven_fake_i2c_last_write(ADAU1860_REG_FDSP_SL_ADDR);
	CHECK(addr && addr->data[0] == LARK_FDSP_SLOT_MUTE);
	CHECK(sl && payload_word(sl, 4) == 0);
}

static void test_register_framing_is_32bit_big_endian_address(void)
{
	haven_fake_i2c_reset();
	initialised = true;
	CHECK(adau1860_control_set_mute(false) == 0);
	/* The fake decoded the first 4 bytes as a BE32 register address; if the
	 * driver had sent 16-bit addresses these lookups would all miss. */
	CHECK(haven_fake_i2c_last_write(ADAU1860_REG_FDSP_SL_ADDR) != NULL);
	CHECK(haven_fake_i2c_last_write(0x4000C0CEu) != NULL); /* FDSP_SL_UPDATE literal */
}

int main(void)
{
	RUN(test_notch_structural_identity);
	RUN(test_zero_cut_peaking_is_identity_filter);
	RUN(test_notch_rejects_f0_passes_far_off);
	RUN(test_partial_peaking_cut_attenuates_less_than_full_notch);
	RUN(test_worst_case_band_survives_q27_at_fdsp_rate);
	RUN(test_q27_encode_basics);
	RUN(test_haven_notch_round_trips_through_q27);
	RUN(test_golden_upstream_equalizer_row_150hz);
	RUN(test_upstream_fdsp_banks_only_stable_with_negated_feedback);
	RUN(test_init_sequence_on_fake_codec);
	RUN(test_init_fails_cleanly_when_codec_absent);
	RUN(test_apply_filters_safeloads_bands_then_unity);
	RUN(test_bypass_writes_unity_everywhere_and_off_is_noop);
	RUN(test_volume_mapping_matches_upstream);
	RUN(test_register_framing_is_32bit_big_endian_address);
	return haven_test_summary("test_adau1860_coeffs");
}
