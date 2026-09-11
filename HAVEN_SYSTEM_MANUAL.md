# Haven Firmware System Manual

Covers the nRF5340 firmware in this repo as it actually exists: module
architecture, the real BLE UUID map, RTT logging setup (including two
real gotchas this project hit and how they were fixed), and a
troubleshooting section built from this project's own hardware review and
bring-up history — not generic Zephyr-tutorial material.

## 1. Firmware architecture

```
                     ┌─────────────────────────────────────────┐
                     │                main.c                   │
                     │  boot sequence + line/command dispatch   │
                     └───────────────┬───────────────────────────┘
                                     │
        ┌────────────────────────────┼─────────────────────────────┐
        │                            │                             │
        ▼                            ▼                             ▼
┌───────────────┐           ┌─────────────────┐           ┌──────────────────┐
│ ble_transport  │  NUS line │  protocol.c      │  parsed  │ gatt_audio_service│
│ (NUS RX/TX,    │─────────▶│  (JSON parse +    │  dsp_cmd │ (Volume/FreqRange │
│  advertising,  │           │  safety clamps)  │─────────▶│  BLE service)     │
│  connections)  │           └─────────────────┘           └─────────┬─────────┘
└───────┬────────┘                                                    │
        │ connect/disconnect                                          │ validated write
        │ events                                                      ▼
        ▼                                                    ┌──────────────────┐
┌───────────────┐                                            │ mock_audio_pipeline│
│  tone_safety   │◀───────────────── TONE_* commands ───────│ (registers as a   │
│ (LDL watchdog, │                                            │ volume/freq_range │
│  auto-silence) │                                            │ consumer)         │
└───────┬────────┘                                            └─────────┬─────────┘
        │                                                                │
        └──────────────────────┬─────────────────────────────────────────┘
                                ▼
                     ┌─────────────────────┐        ┌──────────────────┐
                     │  adau1860_control    │◀──────│ settings_store    │
                     │ (codec bring-up, RBJ │        │ (NVS restore on   │
                     │  biquad math → Q5.27 │        │  boot, save on    │
                     │  → FastDSP safeload  │        │  every validated  │
                     │  over I2C; tone gen  │        │  BLE write)        │
                     │  still a stub)       │        └──────────────────┘
                     └──────────┬──────────┘
                                ▼
                     ADAU1860 FastDSP: PDM mic → 5 biquads → … → DAC
```

**Module responsibilities:**

- **`main.c`** — boot order (ADAU1860 init → BLE transport → GATT service →
  mock pipeline → settings restore, in that specific order — see the code
  comment on why settings restore happens *last*: so a restored value's
  change-callback actually reaches the already-registered pipeline
  consumer instead of firing into nothing) and the JSON-command-to-module
  dispatch table in `handle_line()`.
- **`protocol.c`** — allocation-free parser for the fixed JSON command
  schema (`MULTI_FILTER`, `BYPASS`, `TONE_START`, `TONE_LEVEL`,
  `TONE_STOP`). Pure C, zero Zephyr dependency. Clamps every numeric field;
  malformed input can only ever produce `-EINVAL`, never a
  partially-applied command. `TONE_*`'s `level_db` clamp
  (`PROTOCOL_TONE_LEVEL_MAX_DB` = 85 dB) is deliberately independent of the
  mobile app's own 85 dB cap — a defense-in-depth pair, not a shared
  constant.
- **`ble_transport.c`** — BLE advertising (as `CONFIG_BT_DEVICE_NAME`),
  Nordic UART Service (NUS) line assembly (accumulates fragmented NUS
  writes until `\n`, drops-and-warns on a >512-byte oversized line rather
  than corrupting the next command), and connection lifecycle. See §4 for
  the re-advertising race this file works around.
- **`gatt_audio_service.c`** — the Haven Audio Control Service: Volume
  (uint8 percent) and FreqRange (two little-endian uint16 Hz bounds)
  characteristics, both read/write/notify. BLE writes are validated and
  **rejected outright** (`BT_ATT_ERR_*`) rather than clamped — deliberately
  different from `protocol.c`'s silent-clamp behavior, so a GATT client
  finds out immediately that its value didn't stick. A separate trusted-
  source entry point (`gatt_audio_set_volume()`/`gatt_audio_set_freq_range()`,
  used by `settings_store.c` on boot restore) skips ATT validation
  entirely, on the assumption that a previously-saved value doesn't need
  re-validating.
- **`mock_audio_pipeline.c`** — registers as a consumer of validated
  volume/freq-range changes and runs a synthetic (not real-time, no real
  ADC/I2S input yet) signal chain: a 1kHz test tone through an RBJ
  constant-skirt-gain bandpass built from the FreqRange bounds, at a
  volume-scaled gain. Exists so the BLE control path has something visibly
  reactive to test against before real DSP hardware exists.
- **`tone_safety.c`** — the LDL (Loudness Discomfort Level) calibration
  tone's independent auto-stop watchdog: any `TONE_START`/`TONE_LEVEL`
  arms a 3-second timer; no keep-alive within that window auto-silences
  the tone. Exists specifically to catch a frozen app or dropped BLE link
  — i.e. cases where the *app's own* timing safety net has already failed
  — so its timeout is deliberately not derived from the app's own timing
  constants.
- **`settings_store.c`** — Zephyf Settings-subsystem (NVS-backed)
  persistence for Volume/FreqRange. Saves on every validated BLE write
  (from `gatt_audio_service.c`), restores on boot via the trusted-source
  entry points above.
- **`adau1860_control.c`** — the codec driver, ported from upstream
  OpenEarable 2.0's `ADAU1860.cpp` (which runs on the board Haven's PCB is
  a port of): enable/supply GPIO sequencing, PLL/frequency-multiplier
  bring-up with bounded STATUS2 polls, DMIC → decimator → ASRC → serial-port
  routing, FastDSP program + parameter-bank load
  (`src/lark_fdsp_program.c`, upstream's program verbatim), DAC/headphone
  amp on. Runtime: RBJ notch/peaking-cut math (double precision) → Q5.27
  with the feedback taps negated → safeload into FastDSP slots 0–4; unity
  pass-through for unused slots and for bypass; volume/mute via upstream's
  slots 6/7. Register addresses are **32-bit** (`src/adau1860_regs.h`).
  Host-tested down to the bytes on the wire (`tests/host/`), CI-compiled,
  **not yet run against a codec**. The LDL tone functions still only log
  — see `docs/fastdsp-program.md` for the plan and the first-power-up
  checklist.

## 2. BLE service/characteristic UUID map

Both services register automatically inside `bt_enable()` (called from
`ble_transport_init()`); `main.c` just confirms and logs afterward.

| Service / Characteristic | UUID | Notes |
|---|---|---|
| Nordic UART Service (NUS) | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | Standard Nordic SDK UUID, not project-specific. Carries the JSON command protocol (`protocol.c`) as newline-terminated lines. |
| NUS RX (write) | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | App → firmware |
| NUS TX (notify) | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | Firmware → app (used by `ble_transport_send()`) |
| **Haven Audio Control Service** | `7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10` | Project-specific vendor UUID, generated once for this project — not a Bluetooth SIG-assigned service. Keep stable across firmware updates so paired/bonded phones don't need to rediscover. |
| Volume characteristic | `7a1e0002-4b5c-4e8a-9c1a-2f6b8d3c9a10` | 1 byte, uint8 percent 0–100. Read/Write/Notify. |
| FreqRange characteristic | `7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10` | 4 bytes: `[lower_hz_lo, lower_hz_hi, upper_hz_lo, upper_hz_hi]`, little-endian uint16 pairs. Read/Write/Notify. Bounds: `AUDIO_FREQ_MIN_HZ`=200, `AUDIO_FREQ_MAX_HZ`=8000, and `lower_hz` must be strictly less than `upper_hz`. |

The NUS UUIDs above are Nordic's own published constants (`bluetooth/services/nus.h`
in NCS), reproduced here for reference, not re-derived. The Haven Audio
Control Service UUIDs are defined directly in `gatt_audio_service.c`
(`BT_UUID_HAVEN_AUDIO_*_VAL`) — if you're reading this doc side-by-side
with the code and they ever disagree, the code is the source of truth,
this doc just fell behind.

## 3. RTT logging setup

Standard flow for interactive bring-up sessions:

1. `usbipd.exe list` (Windows side) to find the nRF5340 DK's bus ID, then
   `usbipd.exe attach --wsl --busid <id>` to pass it into WSL2. See §4 for
   the auto-attach-loop gotcha.
2. Flash normally (`west flash`), then attach RTT either via
   `JLinkRTTLogger` or a manual `JLinkExe` RTT session.
3. **Gotcha (hit twice in this project, same root cause both times): RTT
   buffer exhaustion silently masquerading as a hang.**
   - First occurrence: the *default* 1024-byte RTT buffer filled solid
     during the board's own boot-time logging, before any interactive
     session even started — nothing after boot ever appeared, looking
     exactly like a hung board. Fixed by increasing the RTT buffer size.
   - Second occurrence, after the buffer was already enlarged to 16KB:
     `mock_audio_pipeline.c`'s per-tick status line was originally logged
     at `LOG_INF` every `MOCK_PIPELINE_TICK_MS` (2 seconds), unconditionally,
     forever. Over a real multi-minute interactive test session this alone
     filled even the 16KB buffer with nothing but that one line, silently
     crowding out every other log message before it could ever be read —
     again looking like a hang, not an overflowing ring buffer. **Fix**:
     that specific line was dropped to `LOG_DBG` (compiled out at the
     project's default log level), since it fires unconditionally on every
     tick and isn't actually an event — the two lines that ARE real events
     (`recompute_filter()`'s "Filter recomputed" and `on_volume_changed()`'s
     "Pipeline gain updated") stayed at `LOG_INF`. **The general lesson**: a
     periodic/unconditional log line at `LOG_INF` inside anything that ticks
     faster than a human reads RTT output is a real, load-bearing risk on
     this project — treat any new `k_work_delayable`/periodic log
     similarly, at `LOG_DBG` by default, promoting to `LOG_INF` only for
     genuine state-change events.
4. **Gotcha: `JLinkRTTLogger`'s automatic RTT-control-block search is
   unreliable** on this setup — it would sometimes simply fail to find the
   control block even on a board that was definitely running and logging.
   **Workaround**: dump target RAM to a file (`savebin`), grep it for the
   literal `"SEGGER RTT"` control-block signature
   (`grep -aboP "SEGGER RTT"`), and pass the resulting address explicitly
   via `JLinkRTTLogger`'s `-RTTAddress` flag instead of relying on
   auto-search.

## 4. Troubleshooting

### Hardware findings (from this project's own hardware review pass — see
`haven_dev_board_kicad/HAVEN_HARDWARE_REVIEW.md` for full detail and
sourcing/confidence levels on each of these)

- **BLE range/RF issues on the real Haven Dev Board (not the nRF5340 DK)**:
  the KiCad port of the board's ground copper pour was found to extend to
  within ~2.5mm of the MDBT53 module's center on both inner ground/signal
  layers — i.e. no antenna keepout implemented, contrary to Raytac's own
  documented RF layout guidance for this module family ("no ground pad, as
  wide as possible... in EACH layer"). If BLE range or connection
  reliability on the real board is ever noticeably worse than on the DK,
  this is the first thing to check — it's a real, verified finding, not a
  hypothetical.
- **Audio noise/interference on the real Haven Dev Board**: the board has
  a single unified `GND` net/pour, not a split analog/digital ground —
  verified from the actual routed copper. If the ADAU1860's audio output
  picks up digital switching noise, this is a real candidate cause, not
  just a generic "maybe check grounding" guess.
- **ADAU1860 digital-supply noise**: the shared `+1.8V` rail feeding both
  the MDBT53 module and the ADAU1860 sits comparatively far
  (~5.2–5.9mm) from its nearest decoupling cap, vs. ~1.6–2.9mm for the
  charger/fuel-gauge rails on the same board (measured from real placement
  data). Worth a look if the codec's digital supply proves noisy.

### Bring-up / tooling gotchas (from this project's own session history)

- **usbipd attach/detach cycling**: the nRF5340 DK's USB connection into
  WSL2 via `usbipd.exe attach --wsl` can drop (e.g. after a board
  replug or a host sleep/wake) without anything in WSL visibly erroring —
  builds and flashes just start silently failing or timing out. If a build
  that was working suddenly can't find the board, check `usbipd.exe list`
  on the Windows side first and re-attach before assuming a firmware or
  toolchain problem.
- **West manifest: `nrfconnect/nrf` is not a real repo.** The actual GitHub
  repo is `nrfconnect/sdk-nrf` (renamed from `fw-nrfconnect-nrf` pre-NCS
  1.3.0). `west.yml` needs `repo-path: sdk-nrf` alongside `name: nrf` --
  the `name` must stay literally `nrf` (that's what NCS's own
  manifest-import convention keys off), but without the explicit
  `repo-path` override, `west update` fails trying to fetch a
  `nrfconnect/nrf` repo that doesn't exist. This was a real, previously
  dormant bug caught via CI (build 32871505752) — it affects any fresh
  clone, not just CI's environment specifically.
- **CI container Zephyr SDK path**: the `ghcr.io/zephyrproject-rtos/ci`
  container image needs `CMAKE_PREFIX_PATH` pointed explicitly at
  `/opt/toolchains` for CMake to find the Zephyr SDK — it isn't picked up
  automatically inside that container.
- **`west build --pristine`**: on this project's CI container's west
  version, `--pristine` requires an explicit value (`--pristine always`),
  it isn't a bare boolean flag.
- **`GITHUB_TOKEN`-scoped `insteadOf` git rewrites make anonymous-clone
  failures WORSE, not better.** If a CI west-update step is failing to
  anonymously clone a public repo, do not route it through
  `git config --global url.insteadOf` using the job's own `GITHUB_TOKEN` —
  that token is scoped only to the *triggering* repo, so redirecting
  unrelated clones through it turns a plain "repo not found" into an
  authenticated-but-still-"repo not found" failure that's harder to
  diagnose, not easier.
