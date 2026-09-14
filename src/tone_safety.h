/* Independent firmware-side safety layer for the LDL calibration tone.
 *
 * This is the second half of a defense-in-depth pair with the app's own
 * cap (haven-app's src/constants/safety.ts, MAX_TONE_LEVEL_DB) -- see
 * protocol.h and haven-app's docs/safety.md. Two things live here that the
 * app-side cap alone can't provide:
 *
 *   1. A ceiling enforced regardless of what any peer sends (protocol.c
 *      clamps to PROTOCOL_TONE_LEVEL_MAX_DB before a command ever reaches
 *      here, but this module is where "no tone plays without having gone
 *      through that clamp" is structurally guaranteed).
 *   2. An auto-stop watchdog: a frozen app or a hostile/buggy peer that
 *      stops sending TONE_LEVEL keep-alives gets auto-silenced, not left
 *      holding a loud tone indefinitely.
 */
#ifndef HAVEN_TONE_SAFETY_H_
#define HAVEN_TONE_SAFETY_H_

/* Starts (or restarts) the tone at an already-clamped frequency/level and
 * arms the auto-stop watchdog.
 */
void tone_safety_start(float f0_hz, float level_db);

/* Updates the level of an already-playing tone and refreshes the watchdog
 * -- this is the keep-alive a client must send periodically. No-ops (with
 * a warning) if no tone is active: a bare TONE_LEVEL must never implicitly
 * start one.
 */
void tone_safety_set_level(float level_db);

/* Explicit stop -- BLE STOP command, BLE disconnect, or the watchdog
 * firing all route through this. Safe to call when no tone is active.
 */
void tone_safety_stop(void);

/* Optional: called (from the system workqueue) right AFTER the watchdog has
 * auto-silenced a tone, so the transport layer can tell the app. This
 * module deliberately knows nothing about BLE; the dependency points the
 * other way (main.c wires the callback to an ack over NUS TX). The
 * silencing itself never depends on the callback being set or succeeding.
 */
typedef void (*tone_safety_watchdog_cb_t)(void);
void tone_safety_set_watchdog_cb(tone_safety_watchdog_cb_t cb);

#endif /* HAVEN_TONE_SAFETY_H_ */
