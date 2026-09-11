/* See tone_gen.h. */
#include "tone_gen.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(tone_gen, LOG_LEVEL_INF);

/* ── Block geometry ──────────────────────────────────────────────────────────
 * 5 ms blocks: short enough that a level step from the app (every 700 ms in
 * the LDL ramp) lands within 5 ms, long enough to keep the feeder thread's
 * wake-ups at 200 Hz. Byte count is a multiple of 4 (nrfx I2S moves 32-bit
 * words). The slab holds the driver's TX queue depth (CONFIG_I2S_NRFX_TX_
 * BLOCK_COUNT, default 4) plus the block in flight plus the one being filled.
 */
#define TONE_FRAMES_PER_BLOCK 240u
#define TONE_BLOCK_BYTES      (TONE_FRAMES_PER_BLOCK * TONE_GEN_CHANNELS * (TONE_GEN_WORD_BITS / 8u))
#define TONE_SLAB_BLOCKS      6
#define TONE_WRITE_TIMEOUT_MS 100

/* ── Pure synthesis ──────────────────────────────────────────────────────────
 * 1024-entry sine table with 8-bit linear interpolation. Table index is the
 * top 10 bits of the phase accumulator, interpolation fraction the next 8.
 * THD from this scheme is well below -80 dBc, far under what a MEMS speaker
 * in an ear canal adds; a 48k/s sinf() per sample would cost ~5 % of the CPU
 * for no audible gain.
 */
#define SINE_TABLE_BITS 10
#define SINE_TABLE_LEN  (1u << SINE_TABLE_BITS)

static int16_t sine_table[SINE_TABLE_LEN + 1];
static bool sine_table_ready;

static void sine_table_init(void)
{
	if (sine_table_ready) {
		return;
	}
	for (uint32_t i = 0; i <= SINE_TABLE_LEN; i++) {
		double v = sin(2.0 * 3.14159265358979323846 * (double)i / (double)SINE_TABLE_LEN);

		sine_table[i] = (int16_t)lrint(v * 32767.0);
	}
	sine_table_ready = true;
}

uint32_t tone_gen_phase_inc(float f0_hz, uint32_t fs_hz)
{
	/* Guard the accumulator: below 1 Hz there is nothing to hear, above
	 * Nyquist the phase wraps into an alias. */
	if (!(f0_hz > 1.0f)) {
		f0_hz = 1.0f;
	}
	float nyq = (float)fs_hz * 0.5f - 1.0f;

	if (f0_hz > nyq) {
		f0_hz = nyq;
	}
	double inc = ((double)f0_hz / (double)fs_hz) * 4294967296.0;

	return (uint32_t)(inc + 0.5);
}

void tone_synth_fill(struct tone_synth *s, int16_t *buf, size_t frames, int32_t target_gain_q15)
{
	sine_table_init();

	if (target_gain_q15 < 0) {
		target_gain_q15 = 0;
	} else if (target_gain_q15 > TONE_GEN_GAIN_ONE) {
		target_gain_q15 = TONE_GEN_GAIN_ONE;
	}

	/* Gain ramps in Q15.16 so a 1-LSB Q15 change over 240 frames is still a
	 * smooth slope, not a step on the first frame. */
	int64_t gain_acc = (int64_t)s->gain_q15 << 16;
	int64_t gain_step = frames ? ((((int64_t)target_gain_q15 << 16) - gain_acc) / (int64_t)frames)
				   : 0;

	for (size_t n = 0; n < frames; n++) {
		gain_acc += gain_step;

		uint32_t idx = s->phase >> (32 - SINE_TABLE_BITS);
		uint32_t frac = (s->phase >> (32 - SINE_TABLE_BITS - 8)) & 0xFFu;
		int32_t a = sine_table[idx];
		int32_t b = sine_table[idx + 1];
		int32_t sample = (a * (int32_t)(256u - frac) + b * (int32_t)frac) >> 8;

		int32_t gain = (int32_t)(gain_acc >> 16);
		int32_t out = (sample * gain) >> 15;

		buf[2 * n] = (int16_t)out;
		buf[2 * n + 1] = (int16_t)out;
		s->phase += s->phase_inc;
	}
	/* Land exactly on the target so successive blocks never drift. */
	s->gain_q15 = target_gain_q15;
}

/* ── I2S streaming ─────────────────────────────────────────────────────────── */

#define I2S_NODE DT_NODELABEL(i2s0)

static const struct device *const i2s_dev = DEVICE_DT_GET(I2S_NODE);

K_MEM_SLAB_DEFINE_STATIC(tone_slab, TONE_BLOCK_BYTES, TONE_SLAB_BLOCKS, 4);

enum tone_state {
	TONE_IDLE = 0,
	TONE_RUN,
	TONE_STOPPING,
};

static atomic_t state = ATOMIC_INIT(TONE_IDLE);
static atomic_t target_gain = ATOMIC_INIT(0);
static atomic_t pending_phase_inc = ATOMIC_INIT(0);
static atomic_t retune_flag = ATOMIC_INIT(0);
static K_SEM_DEFINE(run_sem, 0, 1);
static bool i2s_ready;
static tone_gen_stopped_cb_t stopped_cb;

static struct tone_synth synth;

#define TONE_THREAD_STACK 1536
K_THREAD_STACK_DEFINE(tone_thread_stack, TONE_THREAD_STACK);
static struct k_thread tone_thread;

static int i2s_configure_tx(void)
{
	struct i2s_config cfg = {
		.word_size = TONE_GEN_WORD_BITS,
		.channels = TONE_GEN_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		/* nRF5340 drives BCLK and LRCLK; the codec's serial port 0 is a
		 * slave and its ASRC absorbs the clock-domain difference. */
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = TONE_GEN_SAMPLE_RATE_HZ,
		.mem_slab = &tone_slab,
		.block_size = TONE_BLOCK_BYTES,
		.timeout = TONE_WRITE_TIMEOUT_MS,
	};

	int err = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);

	if (err) {
		LOG_ERR("i2s_configure(TX) failed: %d", err);
	}
	return err;
}

/* Allocate, synthesise and hand one block to the driver. */
static int push_block(int32_t gain)
{
	void *block;
	int err = k_mem_slab_alloc(&tone_slab, &block, K_MSEC(TONE_WRITE_TIMEOUT_MS));

	if (err) {
		LOG_ERR("tone block alloc timed out (%d)", err);
		return err;
	}
	tone_synth_fill(&synth, (int16_t *)block, TONE_FRAMES_PER_BLOCK, gain);
	err = i2s_write(i2s_dev, block, TONE_BLOCK_BYTES);
	if (err) {
		k_mem_slab_free(&tone_slab, block);
		LOG_ERR("i2s_write failed: %d", err);
	}
	return err;
}

static void apply_retune(void)
{
	if (atomic_cas(&retune_flag, 1, 0)) {
		synth.phase_inc = (uint32_t)atomic_get(&pending_phase_inc);
	}
}

/* One tone from START to quiet. Returns when the link is stopped. */
static void run_one_tone(void)
{
	int err;

	synth.phase = 0;
	synth.gain_q15 = 0; /* every tone fades in from silence */
	apply_retune();

	/* Two blocks queued before START so the first DMA completion already
	 * has a successor; an empty queue at that point is an underrun that
	 * drops the driver into the ERROR state. */
	for (int i = 0; i < 2; i++) {
		err = push_block((int32_t)atomic_get(&target_gain));
		if (err) {
			goto recover;
		}
	}
	err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (err) {
		LOG_ERR("I2S START failed: %d", err);
		goto recover;
	}

	while (atomic_get(&state) == TONE_RUN) {
		apply_retune();
		err = push_block((int32_t)atomic_get(&target_gain));
		if (err) {
			goto recover;
		}
	}

	/* Stop requested: one block ramping to zero, then let the queue play
	 * out. DRAIN stops the peripheral after the last queued block; the
	 * driver frees the blocks itself. */
	(void)push_block(0);
	err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	if (err) {
		LOG_WRN("I2S DRAIN failed (%d), dropping", err);
		goto recover;
	}
	/* Queue depth x block time, generously. */
	k_msleep((TONE_SLAB_BLOCKS + 1) * 5);
	return;

recover:
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	/* After an underrun the driver sits in ERROR until re-prepared;
	 * reconfiguring is the unconditional way back to READY. */
	(void)i2s_configure_tx();
	/* An I2S failure ends the tone; the codec must be told (callback). */
	atomic_cas(&state, TONE_RUN, TONE_STOPPING);
}

static void tone_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sem_take(&run_sem, K_FOREVER);
		/* A start that arrives while a previous tone is still draining
		 * flips STOPPING back to RUN; the inner loop picks it up without
		 * a second semaphore. */
		while (atomic_get(&state) == TONE_RUN) {
			run_one_tone();
			if (atomic_cas(&state, TONE_STOPPING, TONE_IDLE) && stopped_cb) {
				stopped_cb();
			}
		}
	}
}

int tone_gen_init(void)
{
	sine_table_init();

	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S0 device not ready -- tone path disabled");
		return -ENODEV;
	}
	int err = i2s_configure_tx();

	if (err) {
		return err;
	}
	k_thread_create(&tone_thread, tone_thread_stack, K_THREAD_STACK_SIZEOF(tone_thread_stack),
			tone_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
	k_thread_name_set(&tone_thread, "tone_gen");
	i2s_ready = true;
	LOG_INF("Tone generator ready: I2S0 master, %u Hz, %u-bit stereo, %u-frame blocks",
		TONE_GEN_SAMPLE_RATE_HZ, TONE_GEN_WORD_BITS, TONE_FRAMES_PER_BLOCK);
	return 0;
}

int tone_gen_start(float f0_hz, int32_t gain_q15)
{
	if (!i2s_ready) {
		return -ENODEV;
	}
	if (gain_q15 < 0) {
		gain_q15 = 0;
	} else if (gain_q15 > TONE_GEN_GAIN_ONE) {
		gain_q15 = TONE_GEN_GAIN_ONE;
	}
	atomic_set(&pending_phase_inc, (atomic_val_t)tone_gen_phase_inc(f0_hz, TONE_GEN_SAMPLE_RATE_HZ));
	atomic_set(&retune_flag, 1);
	atomic_set(&target_gain, gain_q15);

	for (;;) {
		atomic_val_t cur = atomic_get(&state);

		if (cur == TONE_IDLE) {
			if (atomic_cas(&state, TONE_IDLE, TONE_RUN)) {
				k_sem_give(&run_sem);
				break;
			}
		} else if (cur == TONE_STOPPING) {
			/* Mid-drain: the feeder loop re-runs the tone as soon as
			 * the drain completes (see tone_thread_fn). */
			if (atomic_cas(&state, TONE_STOPPING, TONE_RUN)) {
				break;
			}
		} else {
			/* Already running: retune + gain land on the next block. */
			break;
		}
	}
	return 0;
}

int tone_gen_set_gain(int32_t gain_q15)
{
	if (!i2s_ready) {
		return -ENODEV;
	}
	if (gain_q15 < 0) {
		gain_q15 = 0;
	} else if (gain_q15 > TONE_GEN_GAIN_ONE) {
		gain_q15 = TONE_GEN_GAIN_ONE;
	}
	atomic_set(&target_gain, gain_q15);
	return 0;
}

int tone_gen_stop(void)
{
	if (!i2s_ready) {
		return -ENODEV;
	}
	atomic_set(&target_gain, 0);
	atomic_cas(&state, TONE_RUN, TONE_STOPPING);
	return 0;
}

bool tone_gen_is_running(void)
{
	return atomic_get(&state) != TONE_IDLE;
}

void tone_gen_set_stopped_callback(tone_gen_stopped_cb_t cb)
{
	stopped_cb = cb;
}
