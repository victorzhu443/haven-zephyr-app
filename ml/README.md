# Haven ML frequency-analysis tool

Standalone offline tool: given a `.wav` recording, find the frequency band
that's most likely to be the "troublesome" one for a hearing-protection use
case, and encode it into the exact byte format Haven's firmware expects on
its FreqRange BLE characteristic.

This is a bench/analysis tool, not firmware -- it doesn't run on the board
itself. `analyze.py` + `ble_translator.py` produce the same 4 bytes a real
BLE write to that characteristic would carry; `apply_over_ble.py` (below)
performs that write directly against a connected board.

## What it does and why

`analyze.py` computes a power spectral density (PSD) estimate over the whole
clip using Welch's method (`scipy.signal.welch`), finds the single dominant
spectral peak, and reports the **-3 dB (half-power) bandwidth** around that
peak as the troublesome band.

Why this heuristic and not something fancier: hearing-protection-relevant
noise problems are very often a single dominant narrowband component -- a
machine whine, an alarm tone, feedback, a motor harmonic -- sitting on top
of a broader noise floor. A "loudest octave" or broad-spectrum measure would
just track overall spectral tilt and flag almost any voice or music clip as
"troublesome," which isn't useful. The half-power bandwidth around the
dominant peak is the standard way (in audio and RF engineering both) to
characterize how wide a tonal/narrowband component actually is, and it's
fully inspectable -- no training data, no black box.

This is **not** a learned model. There's no labeled "troublesome vs. fine"
dataset for this project, so a narrowband-peak heuristic is the honest
starting point rather than a premature ML pipeline. See `PROPOSALS.md` at
the repo root for a note on where a learned approach could go later.

## Usage

```bash
pip install -r requirements.txt
python3 analyze.py path/to/recording.wav
```

Output:

```
Dominant peak: 1199.2 Hz
Troublesome band (-3dB): [1195.3, 1203.1] Hz
FreqRange characteristic payload: ab04b304
```

Programmatically:

```python
from analyze import analyze_file
from ble_translator import encode_freq_range

peak_hz, lower_hz, upper_hz = analyze_file("recording.wav")
wire_bytes = encode_freq_range(lower_hz, upper_hz)  # ready to write to the characteristic
```

## Mapping to the real BLE characteristic

- Service: Haven Audio, UUID `7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10`
- Characteristic: FreqRange, UUID `7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10`
- Wire format: 4 bytes, little-endian `uint16 lower_hz`, `uint16 upper_hz`
  (see `../src/gatt_audio_service.c`'s `read_freq_range()` / `apply_freq_range()`
  and `../src/gatt_audio_service.h` for the authoritative definition -- this
  tool matches that format exactly, it doesn't define its own).
- Valid range: firmware clamps/validates to `[FREQ_MIN_HZ, FREQ_MAX_HZ]` =
  `[200, 8000]` Hz. `ble_translator.encode_freq_range()` clamps to the same
  range before packing, so this tool can never hand the firmware something
  it would have to reject.

## Applying a detected band over BLE

`apply_over_ble.py` closes the loop: analyze a recording, then write the
result straight to a connected board's FreqRange characteristic, using the
same UUIDs and write semantics as `tools/ble_bench_test.html`'s
`freqChar.writeValueWithResponse(...)` (a second, independent client
speaking the same protocol, not a new one).

```bash
pip install -r requirements.txt   # now includes bleak
python3 apply_over_ble.py recording.wav                # scan, connect, write
python3 apply_over_ble.py recording.wav --device Haven  # match by name (default)
python3 apply_over_ble.py recording.wav --dry-run        # analyze + print only, no BLE
```

Needs a working Bluetooth adapter and `bleak` for anything but `--dry-run`.
Neither was available in the environment this was written in -- the
analysis + encoding + argument handling are unit-tested, and the BLE write
path is implemented against `bleak`'s documented API, but the actual radio
write has not been exercised against a real board. Verify that leg for real
before relying on it unattended.

## Tests

```bash
python3 -m unittest discover -s tests -v
```

Covers: clamping behavior at/outside `[200, 8000]` Hz, little-endian byte
order (checked against a manual byte-by-byte packing, not just
`struct.pack` calling itself), round-trip encode/decode, and the
frequency-detection heuristic against synthetic single- and dual-tone test
signals (including confirming `search_min_hz`/`search_max_hz` actually
changes which tone gets picked as dominant).

## `experiments/`

Reproducible one-off feasibility checks that aren't part of the shipped
tool and aren't in `requirements.txt` (each script says what extra package
to `pip install` to run it). `rnnoise_feasibility_check.py`: tested whether
denoising a recording with RNNoise before running `analyze.py` on it would
help — real result: RNNoise suppressed 63-99.9% of the energy in every
non-speech test signal tried (a pure tone, an alarm-like warble), because
it's a *speech* denoiser and this tool's whole job is finding narrowband,
non-speech tones. See `ML_RL_FEASIBILITY.md` (repo root) for the full
writeup; that recommendation has been withdrawn as a result.
