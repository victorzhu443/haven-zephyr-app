# Haven — feature proposals

A handful of concrete ideas for where the project could go next, grounded
in what actually exists today: nRF5340 + ADAU1860 DSP hardware in fab,
Zephyr firmware with BLE GATT bench controls, an independent tone-level
safety ceiling, NVS settings persistence, and adaptive-advertising power
management; a React Native app with an LDL guided hearing test; and a new
offline ML tool (`ml/`) that finds a troublesome frequency band from a
`.wav` recording.

This is a one-time, bounded list meant to be skimmed periodically, not a
tracked backlog — nothing here is scheduled or committed to.

## Firmware

- ~~**Auto-apply the ML tool's output over BLE.**~~ Done — see
  `ml/apply_over_ble.py`. Analysis + encoding + argument handling are
  unit-tested; the actual radio write hasn't been exercised against a real
  board (no Bluetooth adapter in the environment this was built in), so
  verify that leg for real before relying on it unattended.
- **CONFIG_PM_DEVICE for the I2C/I2S buses.** Once the real ADAU1860 driver
  work starts, suspending those peripherals between transactions (rather
  than just the CPU-idle savings `CONFIG_PM=y` gives today) would matter
  more for a battery-powered wearable than it does on the DK.

## App / UX

- ~~**Surface a "quiet mode" toggle tied to the new adaptive advertising.**~~
  Done (presentation-only, not a real device-state read — see below) — see
  `haven_custom_app`'s `ConnectionBar`. Shows a note after ~5 minutes
  disconnected explaining the reconnect delay; there's no BLE
  characteristic exposing firmware's real advertising-interval state, so
  this is a client-side approximation of `BLE_IDLE_ADV_TIMEOUT_MS`, not an
  actual read. A real toggle (forcing low-power mode, not just narrating
  it) is still open if that's wanted later.
- ~~**LDL result history + trend view.**~~ Done — see `haven_custom_app`'s
  `LdlHistoryStore` and `LdlHistory` component. Persists each completed run
  (timestamp + results, capped at 20) and shows the 5 most recent plus a
  simple improving/steady/more-sensitive trend. Aborted-early runs aren't
  saved, only full passes through every test frequency.

## Hardware

- ~~**Resolve the BMX160/BMI160 schematic-vs-BOM mismatch.**~~ Done — see
  `HAVEN_HARDWARE_REVIEW.md` §8 in `haven_dev_board_kicad`. Wiring
  inspection confirmed the sourced part is BMI160 (6-axis, no
  magnetometer): its ASDX/ASCX aux-interface pins are tied to GND, which
  is BMI160's documented reference pattern for "no external magnetometer,"
  not a valid way to wire a BMX160. Schematic `Value` corrected to match
  the sourced part. No firmware touches IMU functionality yet, so this
  was purely a BOM-accuracy fix, not a functional one.

## ML / DSP

- **Compare the -3dB peak-bandwidth heuristic against real problem
  recordings.** `ml/analyze.py`'s heuristic is deliberately simple and
  untrained; once there's a small set of recordings the user actually
  considers "troublesome" vs. "fine," it'd be worth checking whether the
  half-power-bandwidth heuristic actually agrees with human judgment before
  investing in anything more elaborate (e.g. a learned classifier).
- **Time-varying bands, not just one static range.** The current
  FreqRange characteristic (and `ml/`'s output) describes a single fixed
  band. Real problem noise (e.g. a siren) can sweep or shift over time —
  worth deciding whether that's a real use case before building for it.

## Safety

- **Extend the independent tone-safety ceiling's pattern to the real
  filter path.** `tone_safety.c`'s validation + watchdog approach (clamp
  + timeout, enforced firmware-side regardless of what the app sends) only
  covers the LDL test tone today. Once real ADAU1860 filtering is wired
  up, the same "don't trust the app alone" principle plausibly applies
  there too — worth revisiting once that driver work starts, not before.

## Scoping: time-varying frequency bands

Expanding on the "time-varying bands" idea above, since it was explicitly
flagged as needing scoping before anyone builds it.

**What it would actually require** depends on which of two different
things "time-varying" means, and they're very different in cost:

1. *The reported band changes over time as conditions change* (e.g. a
   sound's dominant frequency drifts, or a new sound becomes the
   problem). This needs **no wire-format change at all** — the FreqRange
   characteristic already supports being written repeatedly, and it
   already notifies on change (`apply_freq_range()` in
   `gatt_audio_service.c`). A phone-side loop that runs `ml/analyze.py`
   over a rolling window and re-writes the characteristic every second or
   so already gets you this, today, with the 4-byte format as-is. At that
   payload size, BLE has enormous throughput headroom for even a 10 Hz
   update rate.
2. *A single message needs to describe a known trajectory* (e.g. "this
   alarm sweeps from 400 Hz to 1200 Hz over 2 seconds," encoded once, so
   the board can react to the *shape* without the phone driving every
   step). This is a genuinely different, harder feature — a new
   characteristic (or a new command framed some other way) carrying a
   sequence of `(band, duration)` pairs or a parametric sweep
   description, plus firmware logic to play that sequence back. It only
   matters if the device needs to act autonomously between phone updates.

**Is it worth the complexity?** Not established yet. Haven's control
model today is phone-drives-firmware for everything — the firmware
doesn't run its own analysis loop, so there's no existing case where the
board needs to react to a sound without the phone already being in the
loop moment-to-moment. Most of the "sweeping siren" style concern is also
softer in practice than it sounds: alarms, motor whines, and feedback
tend to be quasi-stationary over the few-hundred-ms-to-seconds window
that matters for a reaction, which is exactly what option 1 above already
covers for free.

**Recommendation: don't build a new wire format yet.** Option 1 (repeated
writes to the existing characteristic) already covers the case that's
actually plausible today, requires zero firmware changes, and can be
prototyped entirely in `ml/` + a bench script. Option 2 is real
complexity that should only be scoped once there's a concrete real-world
recording or use case that repeated writes genuinely can't handle — which
is the same gap the "validate the ML heuristic against real recordings"
item above is waiting on. Revisit this once that data exists, not before.
