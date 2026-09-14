# Host-side unit tests

Plain host-gcc tests, no Zephyr build system, no native_sim, no twister.

## Why host-gcc instead of native_sim/twister

Checked feasibility first rather than assuming: `~/ncs` has an initialized
NCS/Zephyr workspace with `zephyr/boards/native/native_sim` present, and
`west`/`gcc` both work in this sandbox, so a real native_sim+twister+ztest
setup is *possible* here in principle. But the actual application
(`boards/nrf5340dk_nrf5340_cpuapp.overlay`, I2C1/I2S0 peripherals, the real
BLE stack) is written against real nRF5340 hardware, and every module this
pass needed to test either has zero Zephyr dependency at all (`protocol.c`)
or pulls in just a couple of Zephyr subsystems (kernel work-queue, BT GATT,
settings) rather than needing the full board. Standing up a twister test
app with the right board/overlay/Kconfig combination to make those
subsystems behave under native_sim, for four fairly small modules, was a
larger and riskier lift than directly compiling the real `.c` files under
host gcc against small fake headers -- and the second approach still tests
the actual production code, not a reimplementation of it. That's the
approach used here.

## How this actually works

Each `test_*.c` file `#include`s the real production `.c` file from
`../../src/` directly into its own translation unit -- not by modifying
that file (no `static` was removed from anything in `src/`), but because
`#include`-ing a `.c` file puts its `static` functions in the *same*
translation unit as the test code, making them directly callable. Real
Zephyr headers (`<zephyr/kernel.h>`, `<zephyr/bluetooth/gatt.h>`,
`<zephyr/settings/settings.h>`, etc.) are swapped for minimal fakes in
`fakes/zephyr/...` via `-Ifakes`. Every fake header says exactly what it
fakes and why in its own header comment.

`test_adau1860_coeffs.c` used to be the exception (a verbatim copy of
`calc_band_coeffs()` because the real file's devicetree macros couldn't be
faked). It now includes the real `adau1860_control.c` too: the devicetree
surface it uses is small (`DT_NODELABEL`, `DT_NODE_HAS_PROP`, `DT_PROP`,
`I2C_DT_SPEC_GET`, `GPIO_DT_SPEC_GET`) and is faked in
`fakes/zephyr/device.h`, `drivers/i2c.h`, `drivers/gpio.h`. The I2C fake is
not a no-op: it decodes every write (4-byte big-endian register address +
payload) into a log the tests assert on, and answers reads from a tiny
register model so the bring-up sequence's `STATUS2` polls succeed. That
lets the tests check the driver down to the bytes it puts on the wire
(program/bank memory writes, safeload slot sequence, Q5.27 words) without
any Zephyr build.

## What's covered vs. not

| File | What's tested | What's NOT tested |
|---|---|---|
| `protocol.c` | Full parse/clamp/reject logic, real production code, no stubs needed at all | n/a -- this file has no Zephyr/BLE dependency to begin with |
| `ack.c` + `tone_safety.c` (`test_ack.c`) | Every ack/event string byte-for-byte (acks echo *clamped* values, integers only, `ok:false` paths for parse and driver errors), the ≤244-byte / `ACK_MAX_LEN` bound incl. worst cases, refusal of a too-small buffer; the watchdog → callback hook (fires after the stop, not on explicit stops, silencing independent of the callback) | Actual NUS delivery (`ble_transport_send` is not compiled here) |
| `mock_audio_pipeline.c` | Real `recompute_filter`/`process_buffer`/`generate_test_tone`/`on_volume_changed`, functional DSP correctness (passband/rejection, gain scaling, memory reset) | The `k_work` scheduling/timing itself (fake no-op) |
| `adau1860_control.c` | Real production code: RBJ math (functional), Q5.27 encoding + FastDSP sign convention pinned to upstream OpenEarable data (Equalizer.cpp golden row; shipped FastDSP banks stable only with negated feedback), full `adau1860_control_init()` register sequence against the fake codec (program/bank/run/route/unmute/unity-safeload writes, clean failure when the codec NAKs), `apply_filters` slot-by-slot payloads, bypass, volume/mute words | Real I2C timing, the codec's actual behaviour (nobody has powered one with this code yet), GPIO electrical state |
| `tone_gen.c` + tone functions in `adau1860_control.c` (`test_tone_path.c`) | Real production code: synthesis (frequency within 0.1 % at all LDL frequencies, full-scale without wrap, exact silence at zero gain, click-free gain ramps, phase continuity across blocks), `level_db → Q15 gain` mapping, I2S configuration (master / 48 kHz / 16-bit / stereo / 4-byte blocks), block alloc→fill→write→free with no leaks, start/retune/stop/drain state machine, codec route switch order (mute → route → unmute) and restore paths (feeder callback, BLE disconnect, degraded modes) | The feeder thread's scheduling (threads never run under the fake kernel), real I2S timing, whether the codec makes the routed audio audible, and the acoustic level (see haven-app `docs/calibration.md`) |
| `gatt_audio_service.c` | Real `write_volume`/`write_freq_range`/`read_volume`/`read_freq_range`/`apply_volume`/`apply_freq_range`, including the accept-vs-reject-vs-clamp distinction and the trusted-path-skips-validation contract | Real BLE transport, ATT bearer, connection handling, or notification delivery (fakes are link-satisfying stubs, not a working GATT server) |
| `settings_store.c` | Real `haven_settings_set()` key dispatch, length validation, read-failure propagation, subtree-vs-leaf name matching (using a REAL reimplementation of `settings_name_steq`'s semantics, not a no-op) | An actual flash/NVS save-then-restore round trip -- there is no native_sim or other Zephyr board target buildable in this sandbox to host a real settings backend against simulated flash. Only `haven_settings_set()`'s own dispatch logic is exercised, with isolated test-double `gatt_audio_set_volume`/`gatt_audio_set_freq_range` standing in for the real consumer. |

## Running

```
./run_tests.sh
```

Builds and runs all seven suites with plain gcc, no Zephyr toolchain
required. Exits nonzero if anything fails.
