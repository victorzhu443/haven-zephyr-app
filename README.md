# haven_zephyr_app

Production firmware for **Project Haven**: an nRF5340 acts as the BLE
peripheral for the companion app and as I2C control-port master to an
**ADAU1860** ("Lark") audio codec/DSP. The audio path — in-ear PDM mic →
FastDSP (five biquads) → DAC → speaker — lives entirely inside the codec;
the nRF5340 only writes coefficients. Successor to the validated
`teensy_hearing_shield` prototype.

**Two board targets.** `openearable_v2/nrf5340/cpuapp` is the product: the
OpenEarable 2.0 board Haven's PCB is a port of (`boards/teco/openearable_v2`,
copied from upstream with the codec node on the bus it is physically wired
to). `nrf5340dk/nrf5340/cpuapp` is a bench: the DK's Arduino header driving
an ADAU1860 eval board (`boards/nrf5340dk_nrf5340_cpuapp.overlay`).

## Architecture

```
Mobile app (react-native-ble-plx)
    │  Nordic UART Service, newline-terminated JSON, MTU 247
    ▼
ble_transport.c      — NUS peripheral "Haven", line reassembly,
    │                   BLE connect/disconnect callbacks
    ▼
protocol.c            — fixed-schema JSON parser (MULTI_FILTER / BYPASS), clamps
    ▼
adau1860_control.c    — codec bring-up (ported from upstream OpenEarable),
    │                   RBJ biquad math → Q5.27 → FastDSP safeload over I2C;
    │                   LDL tone: level_db → gain, DAC route switch
    ▼
ADAU1860 FastDSP      — PDM mic → 5 notch / peaking-cut biquads → expander →
                        volume → mute → mixer → limiter → DAC → speaker
                        (src/lark_fdsp_program.c, upstream's program verbatim)

tone_gen.c            — LDL calibration tone: sine synthesised on the nRF,
    │                   streamed over I2S0 (nRF master, 48 kHz) into the
    ▼                   codec's serial port 0 → ASRC → DAC (docs/tone-path.md)
tone_safety.c         — 85 dB clamp + 3 s keep-alive watchdog around it

gatt_audio_service.c  — Haven Audio Control Service (custom GATT, separate
    │                    from NUS above): Volume + FreqRange characteristics
    ▼
mock_audio_pipeline.c — bench-only: simulates a filtered signal so parameter
    │                    changes are observable without real DSP hardware
    ▼
settings_store.c      — NVS: Volume/FreqRange persist across reboots
```

nRF5340 is dual-core: this application runs on the **app core** (cpuapp).
The BLE **Controller** runs on the **network core** (cpunet) as NCS's
unified `ipc_radio` image (configured for HCI serialization —
`SB_CONFIG_NETCORE_IPC_RADIO_BT_HCI_IPC`), built automatically by
sysbuild — see `sysbuild.conf`. You don't write anything for the net core
yourself; just build with `--sysbuild` (below).

### Wire protocol (shared with the app)

```json
{"type":"MULTI_FILTER","bands":[{"f0":4500,"Q":10.0}]}\n
{"type":"BYPASS","enabled":true}\n
```

Up to **5 bands**. Bands may optionally include `"atten_db"` (positive dB of
reduction; omitted = full notch). All parameters are clamped on-device
(`src/protocol.h`).

### Haven Audio Control Service (bench/prototyping — not the production path)

A second, independent control surface added while the real DSP board is
being fabricated, so the nRF5340 DK can serve as a full prototyping bench
in the meantime. Unlike the JSON path above, out-of-range writes are
**rejected** (ATT error), not clamped:

| | UUID | Format |
|---|---|---|
| Service | `7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10` | — |
| Volume | `7a1e0002-4b5c-4e8a-9c1a-2f6b8d3c9a10` | 1 byte, uint8 0–100 (%) |
| FreqRange | `7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10` | 4 bytes, little-endian uint16 pair (lower_hz, upper_hz), bounded [200, 8000] Hz |

Both characteristics are read/write/notify. Not advertised in the
scan-response payload (no room left alongside NUS's own 128-bit UUID) —
discover by connecting and doing GATT service discovery, not by
service-UUID scan filter. `tools/ble_bench_test.html` is a standalone Web
Bluetooth test page for poking at this directly from a desktop browser,
no app or phone needed.

## Building locally (nRF Connect SDK / Zephyr)

1. Install the [nRF Connect SDK toolchain](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/installation.html)
   (nRF Connect for VS Code's toolchain manager is the easiest path — installs
   `west`, the Zephyr SDK, and `nrfutil` together).
2. From a directory that will become your west workspace root (a level
   *above* this repo — `west init -l` uses this repo as the manifest):

   ```sh
   west init -l haven_zephyr_app
   cd haven_zephyr_app/..
   west update
   west zephyr-export
   ```

3. Build — **`--sysbuild` is required**, it's what builds the network-core
   BLE controller image alongside this app:

   ```sh
   west build --board nrf5340dk/nrf5340/cpuapp --sysbuild haven_zephyr_app
   ```

4. Flash (with the DK connected over USB):

   ```sh
   west flash
   ```

   If `west flash` fails with `nrfutil not found`, it isn't installed —
   pass `--runner jlink` instead (works fine, just a different flash tool):

   ```sh
   west flash --runner jlink
   ```

5. Watch logs over the DK's on-board J-Link RTT/USB-CDC serial (115200 8N1,
   or `west build -t rtt_console` / nRF Connect for VS Code's RTT viewer).

`boards/nrf5340dk_nrf5340_cpuapp.overlay` holds the dev-kit pin assignments
for I2C1 (ADAU1860 control), the codec enable pin and I2S0 — double-check
the bus numbering against your exact DK revision first (see the overlay's
own comments). For the real board build with
`--board openearable_v2/nrf5340/cpuapp` instead; its definition is in-tree
under `boards/teco/openearable_v2/` and needs no overlay. Because sysbuild
resolves the board before it configures the application, tell it where that
in-tree board lives (`zephyr/module.yml` declares it, and passing it
explicitly works regardless of how west discovered the app):

   ```sh
   west build --board openearable_v2/nrf5340/cpuapp --sysbuild haven_zephyr_app \
     -- -DBOARD_ROOT="$(pwd)/haven_zephyr_app"
   ```

### Bench hardware

- **The product board**: a stock OpenEarable 2.0 *is* Haven's hardware (same
  nRF5340 module, ADAU1860, PDM mic, speaker, enclosure). The dev bundle is
  €2,348 at shop.openwearables.com; flashing needs a J-Link (EDU Mini works)
  on upstream's debug breakout.
- **Lower-cost bench**: nRF5340 DK + EVAL-ADAU1860EBZ (~$485 at Newark,
  ~$535 all in). The eval board exposes the DMIC interface (header P44) and
  serial port 0 (header P2), so it can be wired to the DK exactly like the
  real board: I2C1 + enable on the Arduino header, PDM mic on P44, I2S0 on
  P2 (DK D4 → P2.8 BCLK, D5 → P2.6 LRCLK, D6 → P2.4 data in) for the LDL
  tone.

Neither is cheap; the eval-board route is the one that doesn't wait on a
PCB fab and exercises every register this firmware touches.

## CI

`.github/workflows/build.yml` runs the host unit tests and then this same
build for both board targets (app core, with sysbuild) on every push to
`master` (this repo's default branch), on pull requests targeting it, and
on demand via `workflow_dispatch` for feature branches.

## Status

**Verified on real nRF5340 DK hardware** (not just build-tested) as of
2026-08-25:

- [x] NUS peripheral, advertising, auto re-advertise on disconnect
- [x] BLE connect/disconnect lifecycle wired to `adau1860_control_on_ble_*`
      callback stubs
- [x] Newline framing + JSON parsing with parameter clamping
- [x] RBJ notch / peaking-cut coefficient math (ported from Teensy prototype)
- [x] nRF5340 DT overlay (I2C1 control port, I2S0 audio) + sysbuild net-core
      (`ipc_radio`) image config
- [x] Firmware-side independent safety ceiling for the LDL tone (85 dB clamp
      + 3 s keep-alive watchdog, `tone_safety.c`)

**Written against upstream OpenEarable's codec driver, host-tested and
CI-compiled, NOT yet run on a codec** (see `docs/fastdsp-program.md` for
the evidence chain and the first-power-up checklist):

- [x] Real ADAU1860 control port: 32-bit register addressing, device-ID
      readback, bounded STATUS2 polls
- [x] Codec bring-up (enable/supply GPIOs, CM rise, PLL/FM, DMIC + decimator
      + ASRC + serial-port routing, DAC/headphone amp), ported from upstream
- [x] Upstream FastDSP program + parameter banks loaded at boot
      (`src/lark_fdsp_program.c`); bank 1 (transparency) selected
- [x] Safeload coefficient writes: five biquad slots, Q5.27, feedback taps
      negated (both facts pinned to upstream data by host tests)
- [x] Volume / mute via the FastDSP volume and mute slots
- [x] In-tree `openearable_v2` board target (copied from upstream; codec on
      the bus it's wired to, I2S master, no MCK)
- [x] CI: automated build on push/PR
- [x] Haven Audio Control Service — custom GATT characteristics for
      Volume/FreqRange, validated writes (rejected, not clamped, unlike
      the JSON path)
- [x] Mock audio pipeline — software-in-the-loop biquad filter reacting to
      live BLE parameter changes, standing in for real DSP output
- [x] NVS settings persistence — Volume/FreqRange survive a reboot

### Not done

- [ ] **Run it on a codec.** Everything in the block above is unverified on
      hardware. `docs/fastdsp-program.md` lists the power-up checks in order
      and what each failure mode means (wrong rate, wrong routing, unpowered
      codec).
- [x] **LDL tone path** written and host-tested (`src/tone_gen.c`,
      `docs/tone-path.md`): nRF I2S master → codec SPT0 → ASRCI0 → DAC,
      hear-through paused for the tone, click-free level ramps, route
      restored on stop / watchdog / BLE loss. Not yet run on hardware.
- [ ] **Acoustic calibration** of `level_db` → dB SPL (haven-app
      `docs/calibration.md`). Until that measurement exists the 85 dB
      ceiling is a nominal number, not a physical one — do it before any
      real-ear testing.
- [ ] Hear-through latency measurement (impulse in → speaker out).
- [ ] Wire the GATT Volume characteristic to
      `adau1860_control_set_volume_pct()` (only the mock pipeline consumes
      it today).
- [ ] Device → app acks over NUS TX (`ble_transport_send` is ready)
- [ ] Wire the production `haven-app` to the Haven Audio Control Service
      above (currently only exercised by `tools/ble_bench_test.html`)
