/* nRF5340-side sine generator for the LDL calibration tone.
 *
 * The ADAU1860 FastDSP program has no oscillator, so the tone is synthesised
 * here and streamed to the codec over I2S0 (nRF master, 48 kHz, 16-bit
 * stereo -- the format upstream OpenEarable plays music with, and the one
 * the codec's serial port 0 is configured for in adau1860_control.c). The
 * codec side (routing the I2S input to the DAC, muting) is
 * adau1860_control.c's job; this module knows nothing about the codec.
 *
 * Level is a linear gain in [0, 1] of full scale. It is applied per block
 * with a linear ramp across the block, so level changes and start/stop are
 * click-free. Frequency changes only happen at start (the LDL test restarts
 * the tone per frequency).
 *
 * Safety: this module trusts its caller. tone_safety.c owns the clamp and the
 * keep-alive watchdog; adau1860_control.c maps level_db -> gain.
 */
#ifndef HAVEN_TONE_GEN_H_
#define HAVEN_TONE_GEN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TONE_GEN_SAMPLE_RATE_HZ 48000u
#define TONE_GEN_CHANNELS       2u
#define TONE_GEN_WORD_BITS      16u

/* Gain is carried as Q15: 32768 == 1.0 (full scale). */
#define TONE_GEN_GAIN_ONE 32768

/* Pure synthesis state, no Zephyr dependency -- host-testable. */
struct tone_synth {
	uint32_t phase;     /* 32-bit phase accumulator, one revolution = 2^32 */
	uint32_t phase_inc; /* per-frame increment, see tone_gen_phase_inc() */
	int32_t gain_q15;   /* gain at the end of the last block */
};

/* Phase increment for f0 at fs (rounded to nearest; error < 1e-5 Hz). */
uint32_t tone_gen_phase_inc(float f0_hz, uint32_t fs_hz);

/* Fill `frames` interleaved stereo int16 frames (same sample on L and R),
 * ramping the gain linearly from s->gain_q15 to target_gain_q15 across the
 * block; s is advanced so the next block continues seamlessly.
 */
void tone_synth_fill(struct tone_synth *s, int16_t *buf, size_t frames, int32_t target_gain_q15);

/* Bring up the I2S0 peripheral (TX only, nRF as bit/frame clock master) and
 * the feeder thread. Call once at boot. Returns 0 or -errno; a failure
 * leaves the tone functions as safe no-ops (they log and return -ENODEV).
 */
int tone_gen_init(void);

/* Start streaming a sine at f0_hz, ramping from silence to gain_q15 over
 * the first block. Restarting while running retunes and re-ramps. Returns
 * 0 or -errno.
 */
int tone_gen_start(float f0_hz, int32_t gain_q15);

/* Change the level; takes effect on the next block with a ramp. */
int tone_gen_set_gain(int32_t gain_q15);

/* Ramp to silence over one block, drain, stop I2S. Idempotent. The
 * registered stopped-callback (if any) runs from the feeder thread once the
 * link is actually quiet -- that is when the codec may be re-routed.
 */
int tone_gen_stop(void);

bool tone_gen_is_running(void);

typedef void (*tone_gen_stopped_cb_t)(void);
void tone_gen_set_stopped_callback(tone_gen_stopped_cb_t cb);

#endif /* HAVEN_TONE_GEN_H_ */
