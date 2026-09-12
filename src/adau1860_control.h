/* ADAU1860 ("Lark") codec/DSP control.
 *
 * Topology this driver assumes (the real Haven / OpenEarable 2.0 board):
 *
 *   PDM mic ──DMIC──▶ ADAU1860 FastDSP: 5 biquads → expander → volume →
 *                      mute → mixer → limiter ──▶ DAC ──▶ speaker
 *                                   ▲
 *   nRF5340 ──I2C (32-bit register addressing)── coefficients only
 *
 * The nRF5340 is never on the audio path: hear-through latency is set by the
 * codec alone. The FastDSP program and its register bring-up sequence are
 * ported from upstream OpenEarable 2.0 (src/drivers/ADAU1860.cpp), which
 * runs on this exact board; see docs/fastdsp-program.md for what was
 * verified how, and what still is not.
 */
#ifndef HAVEN_ADAU1860_CONTROL_H_
#define HAVEN_ADAU1860_CONTROL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* Sample rate the biquad coefficients are designed for. It must equal the
 * FastDSP frame-rate source (FDSP_CTRL4 -- "set fs to be the same as the
 * FastDSP source", EVAL-ADAU1860 UG-2017). Upstream frame-clocks the FDSP
 * from DMIC01, which it runs at 192 kHz (DMIC_CTRL2 = 0x04), so that is the
 * default; the parameter banks upstream ships decode to sensible corner
 * frequencies only under this assumption (docs/fastdsp-program.md). Q5.27
 * precision at this rate is adequate across the whole protocol range
 * (tools/dsp/results/notch_quantisation.md). Override via
 * CONFIG_HAVEN_FDSP_RATE_HZ. First hardware check: a notch commanded at f0
 * that lands at f0/4 means this constant is wrong, not the math.
 */
#ifdef CONFIG_HAVEN_FDSP_RATE_HZ
#define ADAU1860_FDSP_RATE_HZ ((float)CONFIG_HAVEN_FDSP_RATE_HZ)
#else
#define ADAU1860_FDSP_RATE_HZ 192000.0f
#endif

/* One biquad in RBJ form: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 (a0 = 1).
 * Double on purpose: near-unit-circle poles at 192 kHz need more than
 * single precision's ~6e-8 before Q5.27 quantisation (LSB 7.5e-9).
 */
struct adau1860_biquad {
	double b0, b1, b2, a1, a2;
};

/* The same biquad as the FastDSP consumes it: five Q5.27 words, in slot
 * parameter order. NOTE the feedback terms are stored NEGATED relative to
 * RBJ (the FDSP adds its feedback taps): p3 = -a1, p4 = -a2. Decoding
 * upstream's shipped banks under the un-negated convention yields unstable
 * filters (|pole| ~ 2.4) for every stage; negated, every stage is stable
 * with unity/-6 dB/-10 dB DC gains -- see docs/fastdsp-program.md.
 */
#define ADAU1860_Q27_ONE 0x08000000
struct adau1860_fdsp_biquad {
	uint32_t p[5];
};

/* Power up and configure the codec (mirrors upstream's begin()/setup_*):
 * supply + enable GPIOs, CM rise wait, PLL/frequency-multiplier bring-up with
 * STATUS2 polling, DMIC/decimator/ASRC/serial-port routing, FastDSP program +
 * parameter bank load, bank select, DAC/headphone amp on, all five biquad
 * slots set to unity pass-through. Returns 0, or a negative errno (-ENODEV
 * bus/GPIO not ready, -ETIMEDOUT codec never reported power-up, I2C errors).
 */
int adau1860_control_init(void);

/* Compute RBJ notch / peaking-cut coefficients for each band at
 * ADAU1860_FDSP_RATE_HZ, encode to Q5.27 and safeload them into FastDSP
 * slots 0..count-1; unused slots get unity pass-through so stale bands
 * never linger. Bands with atten_db < PROTOCOL_ATTEN_MAX_DB are peaking cuts
 * of that depth; at the max they collapse to a pure notch.
 */
int adau1860_control_apply_filters(const struct filter_band *bands, size_t count);

/* true = all five biquad slots unity (raw hear-through). main.c re-applies
 * the current bands on the next MULTI_FILTER, which is how bypass ends.
 */
int adau1860_control_set_bypass(bool enabled);

/* Output level via the FastDSP VOLUME slot, 0..100 %, using upstream's
 * mapping (0 % = -60 dB, 100 % = 0 dB, log-linear).
 */
int adau1860_control_set_volume_pct(uint8_t volume_pct);

/* Hard mute via the FastDSP MUTE slot (does not disturb the filter slots). */
int adau1860_control_set_mute(bool muted);

/* ── LDL calibration tone ─────────────────────────────────────────────────
 * Safety-critical -- see tone_safety.c, which owns validation/clamping and
 * the auto-stop watchdog. NOT YET IMPLEMENTED on hardware: the FastDSP
 * program has no oscillator. Planned path (upstream already has the pieces):
 * nRF5340 I2S master → codec SPT0 → ASRCI → mixer slot 8 → DAC, i.e. the
 * tone is generated on the nRF and mixed in after the filter chain, so
 * TONE_STOP never touches filter state. Until then these log and return 0.
 */
int adau1860_control_set_tone(float f0_hz, float level_db);
int adau1860_control_set_tone_level(float level_db);
int adau1860_control_stop_tone(void);

/* BLE link lifecycle hooks (main.c). Hearing protection must keep working
 * with the phone gone, so neither touches the filter state; they log.
 */
void adau1860_control_on_ble_connected(void);
void adau1860_control_on_ble_disconnected(void);

#endif /* HAVEN_ADAU1860_CONTROL_H_ */
