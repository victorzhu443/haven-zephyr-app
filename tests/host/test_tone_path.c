/* Host tests for the LDL tone path: tone_gen.c (nRF-side synthesis + I2S
 * feeding) and the tone functions in adau1860_control.c (level mapping and
 * codec routing). The real production files are included below and run
 * against the fakes in fakes/zephyr -- the I2S fake keeps the samples it is
 * handed, so the waveform is analysed, not just counted.
 *
 * Not covered: the feeder thread's scheduling (threads never run in these
 * tests; the loop body's building blocks are exercised directly), real I2S
 * timing, and whether the codec actually makes the routed audio audible.
 */
#include "test_harness.h"

#include <math.h>
#include <string.h>

#include "../../src/lark_fdsp_program.c"
#include "../../src/lark_eq_program.c"
#include "../../src/tone_gen.c"
#include "../../src/adau1860_control.c"

const struct device haven_fake_i2c_bus_dev = { .name = "fake-i2c" };
const struct device haven_fake_gpio_port_dev = { .name = "fake-gpio" };

/* ── helpers ────────────────────────────────────────────────────────────── */

/* Frequency of a mono int16 sequence from its rising zero crossings. */
static double measure_freq_hz(const int16_t *stereo, size_t frames, double fs)
{
	size_t first = 0, last = 0, crossings = 0;

	for (size_t n = 1; n < frames; n++) {
		if (stereo[2 * (n - 1)] < 0 && stereo[2 * n] >= 0) {
			if (crossings == 0) {
				first = n;
			}
			last = n;
			crossings++;
		}
	}
	if (crossings < 2) {
		return 0.0;
	}
	return (double)(crossings - 1) * fs / (double)(last - first);
}

static int peak_abs(const int16_t *stereo, size_t frames)
{
	int peak = 0;

	for (size_t n = 0; n < frames; n++) {
		int v = stereo[2 * n] < 0 ? -stereo[2 * n] : stereo[2 * n];

		if (v > peak) {
			peak = v;
		}
	}
	return peak;
}

static int max_step(const int16_t *stereo, size_t frames)
{
	int worst = 0;

	for (size_t n = 1; n < frames; n++) {
		int d = stereo[2 * n] - stereo[2 * (n - 1)];

		if (d < 0) {
			d = -d;
		}
		if (d > worst) {
			worst = d;
		}
	}
	return worst;
}

/* ── 1. synthesis math ─────────────────────────────────────────────────── */

static void test_phase_inc_accuracy(void)
{
	/* 1000 Hz at 48 kHz: inc = 2^32/48 = 89478485.33 -> 89478485 */
	CHECK(tone_gen_phase_inc(1000.0f, 48000) == 89478485u);
	/* Nyquist guard: 40 kHz request at 48 kHz clamps below fs/2. */
	CHECK(tone_gen_phase_inc(40000.0f, 48000) < 0x80000000u);
	/* Sub-audio guard: 0 Hz doesn't produce a stuck DC accumulator. */
	CHECK(tone_gen_phase_inc(0.0f, 48000) > 0u);
}

static void test_fill_frequency_and_amplitude(void)
{
	static int16_t buf[2 * 48000]; /* one second */
	struct tone_synth s = { .phase = 0, .phase_inc = tone_gen_phase_inc(1000.0f, 48000),
				.gain_q15 = TONE_GEN_GAIN_ONE };

	tone_synth_fill(&s, buf, 48000, TONE_GEN_GAIN_ONE);

	double f = measure_freq_hz(buf, 48000, 48000.0);

	CHECK(fabs(f - 1000.0) < 0.5);
	int peak = peak_abs(buf, 48000);

	CHECK(peak >= 32700 && peak <= 32767); /* full scale, no overflow/wrap */
	/* Both channels carry the same sample. */
	CHECK(buf[2 * 1234] == buf[2 * 1234 + 1]);

	/* Each LDL frequency the app sweeps is reproduced within 0.1 %. */
	const float ldl[] = { 1000.0f, 2000.0f, 3000.0f, 4000.0f, 6000.0f, 8000.0f };

	for (size_t i = 0; i < ARRAY_SIZE(ldl); i++) {
		struct tone_synth t = { .phase = 0, .phase_inc = tone_gen_phase_inc(ldl[i], 48000),
					.gain_q15 = TONE_GEN_GAIN_ONE };

		tone_synth_fill(&t, buf, 48000, TONE_GEN_GAIN_ONE);
		double m = measure_freq_hz(buf, 48000, 48000.0);

		CHECK(fabs(m - ldl[i]) / ldl[i] < 0.001);
	}
}

static void test_fill_gain_scales_amplitude(void)
{
	static int16_t buf[2 * 4800];
	struct tone_synth s = { .phase = 0, .phase_inc = tone_gen_phase_inc(1000.0f, 48000),
				.gain_q15 = TONE_GEN_GAIN_ONE / 10 };

	tone_synth_fill(&s, buf, 4800, TONE_GEN_GAIN_ONE / 10);
	int peak = peak_abs(buf, 4800);

	CHECK(peak >= 3200 && peak <= 3280); /* -20 dBFS */

	/* Zero gain is digital silence, exactly. */
	struct tone_synth z = { .phase = 0, .phase_inc = s.phase_inc, .gain_q15 = 0 };

	tone_synth_fill(&z, buf, 4800, 0);
	CHECK(peak_abs(buf, 4800) == 0);
}

static void test_fill_ramps_gain_without_clicks(void)
{
	/* A block that steps from -40 dBFS to 0 dBFS. With a hard step the
	 * first frame of the block would jump by up to ~32000; with the ramp
	 * the largest frame-to-frame step is bounded by the sine's own slope at
	 * full amplitude (2*pi*f/fs * 32767 ~ 4290 at 1 kHz) plus the ramp's
	 * contribution (~32768/240 = 137 per frame). */
	static int16_t buf[2 * 240];
	struct tone_synth s = { .phase = 0, .phase_inc = tone_gen_phase_inc(1000.0f, 48000),
				.gain_q15 = TONE_GEN_GAIN_ONE / 100 };

	tone_synth_fill(&s, buf, 240, TONE_GEN_GAIN_ONE);
	CHECK(max_step(buf, 240) < 4600);
	CHECK(s.gain_q15 == TONE_GEN_GAIN_ONE); /* landed on target exactly */

	/* Ramp to zero (stop path) is equally smooth and ends silent. */
	tone_synth_fill(&s, buf, 240, 0);
	CHECK(max_step(buf, 240) < 4600);
	CHECK(s.gain_q15 == 0);
	CHECK(buf[2 * 239] == 0 && buf[2 * 238] < 200 && buf[2 * 238] > -200);
}

static void test_fill_is_phase_continuous_across_blocks(void)
{
	/* Two consecutive 240-frame blocks must equal one 480-frame block. */
	static int16_t one[2 * 480], two[2 * 480];
	struct tone_synth a = { .phase = 12345, .phase_inc = tone_gen_phase_inc(3000.0f, 48000),
				.gain_q15 = TONE_GEN_GAIN_ONE };
	struct tone_synth b = a;

	tone_synth_fill(&a, one, 480, TONE_GEN_GAIN_ONE);
	tone_synth_fill(&b, two, 240, TONE_GEN_GAIN_ONE);
	tone_synth_fill(&b, two + 2 * 240, 240, TONE_GEN_GAIN_ONE);
	CHECK(memcmp(one, two, sizeof(one)) == 0);
}

/* ── 2. level mapping ──────────────────────────────────────────────────── */

static void test_level_to_gain_mapping(void)
{
	/* TONE_FULL_SCALE_DB (85 by default) -> 0 dBFS. */
	CHECK(adau1860_tone_gain_q15(85.0f) == TONE_GEN_GAIN_ONE);
	/* +/-: 20 dB below full scale = 0.1 */
	CHECK(adau1860_tone_gain_q15(65.0f) == 3277);
	/* 6.02 dB below = 0.5 */
	int32_t half = adau1860_tone_gain_q15(85.0f - 6.0206f);

	CHECK(half >= 16383 && half <= 16385);
	/* The LDL start level (30 dB) is quiet but not silent at 16 bit. */
	int32_t start = adau1860_tone_gain_q15(30.0f);

	CHECK(start > 40 && start < 70); /* 10^(-55/20) * 32768 = 58.3 */
	/* Above full scale can't happen past tone_safety's clamp, but the
	 * mapping itself saturates rather than wrapping. */
	CHECK(adau1860_tone_gain_q15(120.0f) == TONE_GEN_GAIN_ONE);
	/* Way below the 16-bit floor is exactly silent. */
	CHECK(adau1860_tone_gain_q15(-20.0f) == 0);
	/* Monotonic in 2 dB LDL steps. */
	int32_t prev = -1;

	for (float db = 0.0f; db <= 85.0f; db += 2.0f) {
		int32_t g = adau1860_tone_gain_q15(db);

		CHECK(g >= prev);
		prev = g;
	}
}

/* ── 3. I2S feeding ────────────────────────────────────────────────────── */

static void test_init_configures_i2s_master_48k_16bit_stereo(void)
{
	haven_fake_i2s_reset();
	i2s_ready = false;

	CHECK(tone_gen_init() == 0);
	CHECK(i2s_ready);
	CHECK(haven_fake_i2s_configure_calls == 1);
	CHECK(haven_fake_i2s_cfg.frame_clk_freq == 48000u);
	CHECK(haven_fake_i2s_cfg.word_size == 16);
	CHECK(haven_fake_i2s_cfg.channels == 2);
	CHECK(haven_fake_i2s_cfg.format == I2S_FMT_DATA_FORMAT_I2S);
	CHECK((haven_fake_i2s_cfg.options & (I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE)) == 0);
	CHECK(haven_fake_i2s_cfg.block_size == TONE_BLOCK_BYTES);
	CHECK(haven_fake_i2s_cfg.block_size % 4 == 0);
	CHECK(haven_fake_i2s_cfg.mem_slab == &tone_slab);
	CHECK(haven_fake_threads_created >= 1);
}

static void test_push_block_alloc_fill_write_free(void)
{
	haven_fake_i2s_reset();
	haven_fake_i2s_cfg.mem_slab = &tone_slab; /* as configure() would */
	synth.phase = 0;
	synth.phase_inc = tone_gen_phase_inc(2000.0f, 48000);
	synth.gain_q15 = TONE_GEN_GAIN_ONE;

	/* 20 blocks = 100 ms; every block must be returned to the slab by the
	 * (fake) driver, so the slab never runs dry. */
	for (int i = 0; i < 20; i++) {
		CHECK(push_block(TONE_GEN_GAIN_ONE) == 0);
	}
	CHECK(haven_fake_i2s_write_calls == 20);
	CHECK(tone_slab.num_used == 0);
	CHECK(haven_fake_i2s_last_block_len == TONE_BLOCK_BYTES);

	size_t frames = haven_fake_i2s_capture_len / 2;
	double f = measure_freq_hz(haven_fake_i2s_capture, frames, 48000.0);

	CHECK(fabs(f - 2000.0) < 2.0);
	CHECK(peak_abs(haven_fake_i2s_capture, frames) > 32000);

	/* A failed write must not leak the block. */
	haven_fake_i2s_fail_writes = 1;
	CHECK(push_block(TONE_GEN_GAIN_ONE) != 0);
	CHECK(tone_slab.num_used == 0);
	haven_fake_i2s_fail_writes = 0;
}

static void test_start_stop_state_machine(void)
{
	i2s_ready = true;
	atomic_set(&state, TONE_IDLE);
	run_sem.count = 0;

	CHECK(tone_gen_start(4000.0f, 1000) == 0);
	CHECK(atomic_get(&state) == TONE_RUN);
	CHECK(run_sem.count == 1); /* feeder woken */
	CHECK(atomic_get(&target_gain) == 1000);
	CHECK(atomic_get(&retune_flag) == 1);
	CHECK((uint32_t)atomic_get(&pending_phase_inc) == tone_gen_phase_inc(4000.0f, 48000));
	CHECK(tone_gen_is_running());

	/* Level change while running: target moves, no second wake-up. */
	CHECK(tone_gen_set_gain(20000) == 0);
	CHECK(atomic_get(&target_gain) == 20000);
	CHECK(run_sem.count == 1);

	/* Gain is clamped, never wraps. */
	CHECK(tone_gen_set_gain(1 << 20) == 0);
	CHECK(atomic_get(&target_gain) == TONE_GEN_GAIN_ONE);
	CHECK(tone_gen_set_gain(-5) == 0);
	CHECK(atomic_get(&target_gain) == 0);

	/* Stop: target to 0 and the feeder told to drain. */
	CHECK(tone_gen_start(4000.0f, 1000) == 0); /* re-arm gain */
	CHECK(tone_gen_stop() == 0);
	CHECK(atomic_get(&state) == TONE_STOPPING);
	CHECK(atomic_get(&target_gain) == 0);
	CHECK(tone_gen_is_running()); /* still draining */
	CHECK(tone_gen_stop() == 0);  /* idempotent */

	/* A start during the drain flips straight back to RUN, no new wake-up
	 * (the feeder's inner loop re-runs). */
	CHECK(tone_gen_start(1000.0f, 500) == 0);
	CHECK(atomic_get(&state) == TONE_RUN);
	CHECK(run_sem.count == 1);

	/* Without I2S, everything is a clean -ENODEV. */
	i2s_ready = false;
	CHECK(tone_gen_start(1000.0f, 500) == -ENODEV);
	CHECK(tone_gen_set_gain(1) == -ENODEV);
	CHECK(tone_gen_stop() == -ENODEV);
	i2s_ready = true;
	atomic_set(&state, TONE_IDLE);
}

/* ── 4. codec side ─────────────────────────────────────────────────────── */

static void test_set_tone_starts_i2s_then_routes_dac_to_i2s(void)
{
	haven_fake_i2c_reset();
	haven_fake_i2s_reset();
	initialised = true;
	i2s_ready = true;
	tone_route_engaged = false;
	atomic_set(&state, TONE_IDLE);
	run_sem.count = 0;

	CHECK(adau1860_control_set_tone(4000.0f, 30.0f) == 0);

	/* nRF side armed... */
	CHECK(atomic_get(&state) == TONE_RUN);
	CHECK(atomic_get(&target_gain) == adau1860_tone_gain_q15(30.0f));

	/* ...codec side: mute -> DAC_ROUTE0 = I2S -> unmute, in that order. */
	CHECK(tone_route_engaged);
	const struct haven_fake_i2c_xfer *route = haven_fake_i2c_last_write(ADAU1860_REG_DAC_ROUTE0);

	CHECK(route && route->data[0] == ADAU1860_DAC_ROUTE_I2S);
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_DAC_CTRL2) == 2);

	int seen_mute = -1, seen_route = -1, seen_unmute = -1;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		const struct haven_fake_i2c_xfer *x = &haven_fake_i2c_log[i];

		if (x->reg == ADAU1860_REG_DAC_CTRL2 && x->data[0] == DAC_CTRL2_MUTE && seen_mute < 0) {
			seen_mute = (int)i;
		}
		if (x->reg == ADAU1860_REG_DAC_ROUTE0) {
			seen_route = (int)i;
		}
		if (x->reg == ADAU1860_REG_DAC_CTRL2 && x->data[0] == DAC_CTRL2_UNMUTE) {
			seen_unmute = (int)i;
		}
	}
	CHECK(seen_mute >= 0 && seen_route > seen_mute && seen_unmute > seen_route);

	/* Filters untouched by the tone: no safeloads. */
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_UPDATE) == 0);

	/* Level update: nRF only, no codec traffic. */
	size_t before = haven_fake_i2c_log_count;

	CHECK(adau1860_control_set_tone_level(50.0f) == 0);
	CHECK(atomic_get(&target_gain) == adau1860_tone_gain_q15(50.0f));
	CHECK(haven_fake_i2c_log_count == before);

	/* Stop: generator told to drain; codec route NOT yet restored (that
	 * waits for the link to go quiet)... */
	CHECK(adau1860_control_stop_tone() == 0);
	CHECK(atomic_get(&state) == TONE_STOPPING);
	CHECK(tone_route_engaged);
	CHECK(haven_fake_i2c_log_count == before);

	/* ...then the feeder's completion callback restores it under mute. */
	haven_fake_i2c_reset();
	on_tone_stopped();
	CHECK(!tone_route_engaged);
	route = haven_fake_i2c_last_write(ADAU1860_REG_DAC_ROUTE0);
	CHECK(route && route->data[0] == ADAU1860_DAC_ROUTE_FDSP_CH(0));
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_DAC_CTRL2) == 2);
	const struct haven_fake_i2c_xfer *last = &haven_fake_i2c_log[haven_fake_i2c_log_count - 1];

	CHECK(last->reg == ADAU1860_REG_DAC_CTRL2 && last->data[0] == DAC_CTRL2_UNMUTE);

	/* Disengage is idempotent: a second call writes nothing. */
	haven_fake_i2c_reset();
	on_tone_stopped();
	CHECK(haven_fake_i2c_log_count == 0);
	atomic_set(&state, TONE_IDLE);
}

static void test_ble_disconnect_restores_route_synchronously(void)
{
	haven_fake_i2c_reset();
	initialised = true;
	i2s_ready = true;
	tone_route_engaged = false;
	atomic_set(&state, TONE_IDLE);

	CHECK(adau1860_control_set_tone(2000.0f, 40.0f) == 0);
	CHECK(tone_route_engaged);

	/* main.c calls tone_safety_stop() (-> stop_tone) and this hook; the
	 * hook must not wait for the feeder thread. */
	haven_fake_i2c_reset();
	adau1860_control_on_ble_disconnected();
	CHECK(!tone_route_engaged);
	const struct haven_fake_i2c_xfer *route = haven_fake_i2c_last_write(ADAU1860_REG_DAC_ROUTE0);

	CHECK(route && route->data[0] == ADAU1860_DAC_ROUTE_FDSP_CH(0));
	/* Hear-through filters were never touched. */
	CHECK(haven_fake_i2c_count_writes(ADAU1860_REG_FDSP_SL_UPDATE) == 0);
	atomic_set(&state, TONE_IDLE);
}

static void test_tone_without_codec_is_nrf_only(void)
{
	/* Bench: I2S up, codec init failed. The tone still streams (scope on
	 * the DK header) but no I2C traffic is attempted. */
	haven_fake_i2c_reset();
	initialised = false;
	i2s_ready = true;
	tone_route_engaged = false;
	atomic_set(&state, TONE_IDLE);

	CHECK(adau1860_control_set_tone(1000.0f, 60.0f) == 0);
	CHECK(atomic_get(&state) == TONE_RUN);
	CHECK(haven_fake_i2c_log_count == 0);
	CHECK(!tone_route_engaged);
	CHECK(adau1860_control_stop_tone() == 0);
	CHECK(haven_fake_i2c_log_count == 0);
	atomic_set(&state, TONE_IDLE);
}

static void test_tone_without_i2s_still_restores_codec(void)
{
	/* tone_gen_init failed: TONE_START can't play, and stop must still
	 * make sure the DAC is on the hear-through route. */
	haven_fake_i2c_reset();
	initialised = true;
	i2s_ready = false;
	tone_route_engaged = true; /* pretend a previous tone left it routed */

	CHECK(adau1860_control_set_tone(1000.0f, 60.0f) == -ENODEV);
	CHECK(adau1860_control_stop_tone() == 0);
	CHECK(!tone_route_engaged);
	const struct haven_fake_i2c_xfer *route = haven_fake_i2c_last_write(ADAU1860_REG_DAC_ROUTE0);

	CHECK(route && route->data[0] == ADAU1860_DAC_ROUTE_FDSP_CH(0));
	i2s_ready = true;
}

int main(void)
{
	RUN(test_phase_inc_accuracy);
	RUN(test_fill_frequency_and_amplitude);
	RUN(test_fill_gain_scales_amplitude);
	RUN(test_fill_ramps_gain_without_clicks);
	RUN(test_fill_is_phase_continuous_across_blocks);
	RUN(test_level_to_gain_mapping);
	RUN(test_init_configures_i2s_master_48k_16bit_stereo);
	RUN(test_push_block_alloc_fill_write_free);
	RUN(test_start_stop_state_machine);
	RUN(test_set_tone_starts_i2s_then_routes_dac_to_i2s);
	RUN(test_ble_disconnect_restores_route_synchronously);
	RUN(test_tone_without_codec_is_nrf_only);
	RUN(test_tone_without_i2s_still_restores_codec);
	return haven_test_summary("test_tone_path");
}
