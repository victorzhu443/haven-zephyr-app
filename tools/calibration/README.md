# tools/calibration — the rig that turns `level_db` into dB SPL

Every dB number in Haven today — the 85 dB tone ceiling, the 30 dB LDL start,
the 70 dB sensitivity threshold, `CONFIG_HAVEN_TONE_FULL_SCALE_DB` — is a
**commanded** value with no measured relationship to sound pressure at the
eardrum. haven-app `docs/calibration.md` is the procedure that closes that
gap; this directory is the software side of it: drive the board over BLE,
capture a fixture microphone, do the math, and emit the constants the
firmware and app should carry, with provenance.

**Nothing in here has met real hardware.** The BLE leg mirrors
`ml/apply_over_ble.py`'s `bleak` usage and has only run in `--dry-run`; the
audio leg has only run against the built-in `Simulator` and a laptop
microphone. Every result file records `dry_run` / `simulated_audio` flags, and
`firmware_constants.py` refuses to emit constants from a simulated or
uncalibrated run. Treat the first real run as validation of the rig, not of
the device.

## What you need

| Option | What it is | Approx. cost | Good enough for |
|---|---|---|---|
| IEC 60318-4 occluded-ear simulator (GRAS RA0045 / B&K 4157) + calibrated mic + pistonphone | The standard for insert transducers | high — borrow from an audiology clinic or hearing-science lab | numbers you would put in a safety document |
| MiniDSP EARS or similar headphone fixture, with its calibration file | Prosumer fixture with ear canals | ~$200 | relative measurements, seal effects, notch verification; absolute SPL to a few dB |
| Calibrated measurement mic + 2 cc coupler | Different acoustic load than a canal; known correction | low if borrowed | first sanity check that "85" isn't 100 |
| Phone SPL-meter app at the earpiece | Uncalibrated, wrong load | free | **not acceptable** for anything that ends up in `safety.ts` or Kconfig |

Plus: a board running `haven_zephyr_app` (PR #9 + #10 or later) with the tone
path, and a computer with Bluetooth and an audio interface (`sounddevice`
lists devices: `python3 tone_level_map.py --list-devices`).

```sh
pip install -r requirements.txt
python3 -m unittest discover -s tests -v
```

## Mic calibration file

The rig never invents an acoustic reference. Put the microphone (in its
fixture) on a 94 dB SPL calibrator, read its level in dBFS with any of the
scripts' `--simulate`-free capture (or `python3 -c` + `measure.tone_level_dbfs`),
and write:

```json
{"dbfs_at_ref": -20.3, "ref_dbspl": 94.0, "fixture": "GRAS RA0045, foam tip, best seal", "date": "2026-10-01"}
```

Pass it as `--mic-cal mic.json`. Without it every script reports **dBFS**, says
so, and `firmware_constants.py` refuses the result.

## The procedures

All scripts share the flags in `rig.py`: `--dry-run` (log BLE payloads, no
radio), `--simulate` (synthetic acoustic chain, no mic/speaker), `--fs`,
`--input-device`/`--output-device`, `--mic-cal`, `--seconds`, `--settle`,
`--out`, `--fixture`, `--no-prompt`. `--dry-run --simulate` together rehearse
any procedure with no hardware at all.

### 1. `tone_level_map.py` — commanded level → measured level

For each LDL frequency (1, 2, 3, 4, 6, 8 kHz) the tone steps 30 → 85 in 5 dB;
each step is recorded and the tone's level measured (Hann-windowed FFT peak
within ±2 % of f0; A-weighted broadband alongside for reference), then
`SPL = a·level_db + b` is fitted per frequency. The noise floor is captured
with the tone stopped.

- **Slope ≈ 1 is a firmware property**: 1 dB commanded must be 1 dB out. A
  slope outside 1 ± 0.1 means the `level_db → gain` conversion in
  `tone_gen.c` is wrong and must be fixed there — the header emitter puts
  an `#error` in its output rather than let a table paper over it.
- Output: `results/tone_level_map.json`, `results/tone_level_map.png`.
- Run it twice: `--seal best` and `--seal worst` (eartip half out). The
  worst seal bounds how far the ceiling can drift in real ears.

### 2. `ceiling_check.py` — does 85 hold?

Commands the ceiling at every frequency (best and worst seal), then an
out-of-range 120. By default the rig clamps 120 → 85 before sending — the
same thing the app does — so the payload that leaves says 85; use
`--raw-overrange` to send a literal 120 and test the **firmware's** clamp
(`PROTOCOL_TONE_LEVEL_MAX_DB`) instead. Pass = the measured level after the
over-range command is not above the level at the ceiling.

`--disconnect-test`: starts a 60 dB tone, then the rig stops sending
keep-alives without sending `TONE_STOP`, and polls the mic until the tone is
gone. Expected ≤ 3 s (`TONE_WATCHDOG_MS` in `tone_safety.c`). This is the
"frozen app" scenario the firmware watchdog exists for.

### 3. `hear_through.py` — insertion gain and notch depth

Pink noise from the laptop speaker; captures open-ear, earpiece-in with
`BYPASS`, one band at a time, then all bands. Everything is measured as a
**transfer function against the known stimulus** (`csd/psd`), not a ratio of
two noise spectra — that is what makes a 10 Hz-wide notch at 200 Hz
measurable. The FFT length is chosen per band from its Q (`nperseg_for`), so
the default 8 s stimulus gives ~0.7 Hz bins for the 200 Hz / Q 20 case.

- Insertion gain (bypass − open ear) should be ~0 dB across 200–8000 Hz;
  low-frequency loss is passive occlusion.
- Bands: the app default (4500 Hz, Q 10, 20 dB), the numerical worst case
  from `tools/dsp` (200 Hz, Q 20, full notch), and 8000 Hz. Pass: centre
  within 5 %, peaking depth within 1.5 dB of commanded, a full notch ≥ 30 dB
  (`FULL_NOTCH_MIN_DEPTH_DB` — a 40 dB-commanded notch is infinitely deep in
  theory and fixture-limited in practice).
- All three at once must reproduce the single-band depths (the cascade is
  linear; a mismatch means clipping or a wrong coefficient write).

### 4. `latency.py` — hear-through delay

Click out, cross-correlate reference vs device recording with parabolic
sub-sample interpolation; the interface's own round-trip (open-ear pass) is
subtracted. Target < 1 ms (`LATENCY_TARGET_S`); the report also prints where
the first comb-filter notch would fall (`1/(2Δt)`). Resolution is 1/fs
(20.8 µs at 48 kHz) — use the highest rate the interface supports; in-codec
processing should land in the tens of µs, and anything at several ms means
audio is going through the nRF.

### 5. `firmware_constants.py` — results → firmware

```sh
python3 firmware_constants.py results/tone_level_map.json --margin-db 2 > haven_tone_cal.h
```

From the per-frequency fits it computes the `CONFIG_HAVEN_TONE_FULL_SCALE_DB`
value that makes a commanded 85 produce ≤ 85 − margin dB SPL at the **worst**
measured frequency, and a per-frequency offset table (as a C header, with
date / fixture / mic-cal comments) for a later firmware change that corrects
each LDL frequency individually. It refuses simulated or dBFS input and emits
`#error` if any slope failed.

## How the numbers flow

1. `tone_level_map.json` (best and worst seal) → `firmware_constants.py` →
   new `CONFIG_HAVEN_TONE_FULL_SCALE_DB` in `haven_zephyr_app`, and the
   offset table when the firmware grows a per-frequency correction.
2. `ceiling_check.json` passing on the new constants → haven-app
   `docs/safety.md` "Not yet calibrated" becomes "Calibrated on <date>,
   <fixture>", and the LDL result copy may state real dB SPL.
3. `hear_through.json` + `latency.json` → the product premise (transparent
   hear-through, correct notches, sub-ms delay) confirmed or not.
4. Any later change to the codec program, DAC settings, receiver or eartip
   re-runs 1–3.

## What the tests prove, and what they don't

`tests/` (44 checks, plain `unittest`): the wire payloads match
`src/protocol.h` clamps and framing; the keep-alive fires inside the
watchdog and `drop_keepalive` sends nothing; every measurement function is
checked against synthetic signals with known answers (0 dBFS sines at each
LDL frequency, A-weighting table points, a known RBJ notch vs `freqz`, known
integer and fractional delays incl. 60 µs at 48 kHz); each procedure runs end
to end on the Simulator and its pass/fail logic behaves; the constants
emitter refuses bad input and shifts full scale by the worst frequency.

They do **not** prove anything acoustic. The Simulator assumes the firmware's
nominal mapping (85 → 0 dBFS) is right — which is exactly the assumption the
real run exists to test.
