#include "tone_safety.h"

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "adau1860_control.h"

LOG_MODULE_REGISTER(tone_safety, LOG_LEVEL_INF);

/* Must comfortably outlast a normal LDL ramp step (haven-app's
 * LDL_RAMP_INTERVAL_MS is 700ms, so keep-alives arrive at roughly that
 * cadence during a real test) while still catching a frozen app or a
 * dropped link quickly -- "a few seconds," per haven-app's docs/safety.md.
 * Deliberately not derived from the app's own timing constants: this
 * watchdog exists specifically to catch cases where the app's own timing
 * has already failed.
 */
#define TONE_WATCHDOG_TIMEOUT_MS 3000

static bool tone_active;
static tone_safety_watchdog_cb_t watchdog_cb;

static void watchdog_fired(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("Tone watchdog fired -- no TONE_LEVEL keep-alive within %d ms, "
		"auto-silencing", TONE_WATCHDOG_TIMEOUT_MS);
	tone_safety_stop();
	/* After the stop, so the tone is already silent by the time anyone
	 * hears about it -- the notification is informational, never a step
	 * the silencing depends on. */
	if (watchdog_cb) {
		watchdog_cb();
	}
}

void tone_safety_set_watchdog_cb(tone_safety_watchdog_cb_t cb)
{
	watchdog_cb = cb;
}

static K_WORK_DELAYABLE_DEFINE(watchdog, watchdog_fired);

void tone_safety_start(float f0_hz, float level_db)
{
	tone_active = true;
	adau1860_control_set_tone(f0_hz, level_db);
	k_work_reschedule(&watchdog, K_MSEC(TONE_WATCHDOG_TIMEOUT_MS));
	LOG_INF("Tone started: f0=%.1f Hz level=%.1f dB", (double)f0_hz,
		(double)level_db);
}

void tone_safety_set_level(float level_db)
{
	if (!tone_active) {
		LOG_WRN("TONE_LEVEL with no active tone -- ignored");
		return;
	}
	adau1860_control_set_tone_level(level_db);
	k_work_reschedule(&watchdog, K_MSEC(TONE_WATCHDOG_TIMEOUT_MS));
	LOG_INF("Tone level: %.1f dB", (double)level_db);
}

void tone_safety_stop(void)
{
	if (!tone_active) {
		return;
	}
	tone_active = false;
	k_work_cancel_delayable(&watchdog);
	adau1860_control_stop_tone();
	LOG_INF("Tone stopped");
}
