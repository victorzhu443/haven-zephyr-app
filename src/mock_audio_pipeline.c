#include "mock_audio_pipeline.h"

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "gatt_audio_service.h"

LOG_MODULE_REGISTER(mock_audio_pipeline, LOG_LEVEL_INF);

/* picolibc's math.h only exposes M_PI under a feature-test macro that
 * -std=c17 doesn't define (see adau1860_control.c). */
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Nominal rate for the mock signal chain — no real ADC/I2S input exists
 * yet (adau1860_control.c's audio path is still unwired), so this is a
 * synthetic timeline, not real-time streaming: one buffer's worth of
 * samples is generated and filtered per tick, at whatever wall-clock rate
 * MOCK_PIPELINE_TICK_MS runs, not at 48 kHz. Matches
 * ADAU1860_FDSP_RATE_HZ in adau1860_control.h (the real codec runs at 192 kHz)
 * unchanged once real audio replaces this.
 */
#define MOCK_SAMPLE_RATE_HZ 48000.0f
#define MOCK_TEST_TONE_HZ   1000.0f
#define MOCK_BUFFER_LEN     32
#define MOCK_PIPELINE_TICK_MS 2000

struct mock_biquad {
	float b0, b1, b2, a1, a2;
};

static struct mock_biquad current_coeffs;
static float filter_z1, filter_z2;
static float current_gain = 1.0f;
static float tone_phase;

static struct audio_freq_range current_range = {
	.lower_hz = AUDIO_FREQ_MIN_HZ,
	.upper_hz = AUDIO_FREQ_MAX_HZ,
};

/* RBJ cookbook constant-skirt-gain bandpass, centered between the two
 * cutoffs with Q set by how narrow the range is. Same family of math as
 * adau1860_control.c's calc_band_coeffs (which does peaking/notch instead,
 * for the single-f0 JSON protocol) -- this one directly reflects a
 * [lower_hz, upper_hz] pass window instead.
 */
static void recompute_filter(const struct audio_freq_range *range)
{
	float center_hz = (range->lower_hz + range->upper_hz) / 2.0f;
	float bandwidth_hz = (float)(range->upper_hz - range->lower_hz);
	float q = center_hz / bandwidth_hz;

	float w0 = 2.0f * (float)M_PI * (center_hz / MOCK_SAMPLE_RATE_HZ);
	float alpha = sinf(w0) / (2.0f * q);
	float a0 = 1.0f + alpha;

	current_coeffs.b0 = alpha / a0;
	current_coeffs.b1 = 0.0f;
	current_coeffs.b2 = -alpha / a0;
	current_coeffs.a1 = -2.0f * cosf(w0) / a0;
	current_coeffs.a2 = (1.0f - alpha) / a0;

	/* Fresh coefficients acting on stale filter memory would just add a
	 * transient to the next tick's log for no informative reason. */
	filter_z1 = 0.0f;
	filter_z2 = 0.0f;

	current_range = *range;

	LOG_INF("Filter recomputed: band=[%u, %u] Hz center=%.1f Q=%.2f "
		"coeffs={b0=%.4f, b1=%.4f, b2=%.4f, a1=%.4f, a2=%.4f}",
		range->lower_hz, range->upper_hz, (double)center_hz, (double)q,
		(double)current_coeffs.b0, (double)current_coeffs.b1,
		(double)current_coeffs.b2, (double)current_coeffs.a1,
		(double)current_coeffs.a2);
}

static void on_volume_changed(uint8_t volume_pct)
{
	current_gain = (float)volume_pct / 100.0f;
	LOG_INF("Pipeline gain updated: %u%% (x%.2f)", volume_pct, (double)current_gain);
}

static void on_freq_range_changed(const struct audio_freq_range *range)
{
	recompute_filter(range);
}

/* Direct Form II Transposed biquad, in place. Returns the buffer's peak
 * absolute output sample (post-gain) for the status log.
 */
static float process_buffer(float *buf, size_t len)
{
	float peak_out = 0.0f;

	for (size_t i = 0; i < len; i++) {
		float x = buf[i];
		float y = current_coeffs.b0 * x + filter_z1;

		filter_z1 = current_coeffs.b1 * x - current_coeffs.a1 * y + filter_z2;
		filter_z2 = current_coeffs.b2 * x - current_coeffs.a2 * y;

		y *= current_gain;
		buf[i] = y;

		float mag = fabsf(y);

		if (mag > peak_out) {
			peak_out = mag;
		}
	}
	return peak_out;
}

static void generate_test_tone(float *buf, size_t len)
{
	const float w = 2.0f * (float)M_PI * (MOCK_TEST_TONE_HZ / MOCK_SAMPLE_RATE_HZ);
	const float two_pi = 2.0f * (float)M_PI;

	for (size_t i = 0; i < len; i++) {
		buf[i] = sinf(tone_phase);
		tone_phase += w;
		if (tone_phase > two_pi) {
			tone_phase -= two_pi;
		}
	}
}

static void pipeline_tick(struct k_work *work)
{
	float buf[MOCK_BUFFER_LEN];

	generate_test_tone(buf, MOCK_BUFFER_LEN);
	float peak_out = process_buffer(buf, MOCK_BUFFER_LEN);

	/* LOG_DBG, not LOG_INF: this fires forever, every tick, whether or not
	 * anything changed -- at LOG_INF it silently exhausts the RTT buffer
	 * during any interactive session longer than a few minutes (bit rate
	 * math: even the 16KB buffer filled solid with nothing but these
	 * before a real multi-step BLE test ever got logged). Recomputes/gain
	 * updates above are the actual events and stay at LOG_INF.
	 */
	LOG_DBG("Pipeline tick: %u samples @ %uHz test tone, gain=x%.2f, "
		"band=[%u,%u]Hz, peak_out=%.3f", MOCK_BUFFER_LEN,
		(unsigned int)MOCK_TEST_TONE_HZ, (double)current_gain,
		current_range.lower_hz, current_range.upper_hz, (double)peak_out);

	k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(MOCK_PIPELINE_TICK_MS));
}

static K_WORK_DELAYABLE_DEFINE(pipeline_work, pipeline_tick);

int mock_audio_pipeline_init(void)
{
	recompute_filter(&current_range);
	gatt_audio_service_set_callbacks(on_volume_changed, on_freq_range_changed);
	k_work_schedule(&pipeline_work, K_MSEC(MOCK_PIPELINE_TICK_MS));

	LOG_INF("Mock audio pipeline started (tick every %d ms)", MOCK_PIPELINE_TICK_MS);
	return 0;
}
