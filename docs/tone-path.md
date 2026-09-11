# The LDL calibration tone path

The app's hearing test (`TONE_START` / `TONE_LEVEL` / `TONE_STOP`, see
haven-app `docs/safety.md`) needs a pure tone at a commanded level in the
wearer's ear. The ADAU1860's FastDSP program has no oscillator, so the tone
is made on the nRF5340 and delivered to the codec over I2S. This document is
the design and the list of what is and isn't verified. **Not compiled
against NCS and not run on hardware** as of writing — host-tested only (see
`tests/host/test_tone_path.c`).

## Signal path

```
tone_gen.c (nRF5340)                      ADAU1860
  sine @ f0, gain g            I2S0        ┌─ SPT0 (slave) ─▶ ASRCI0 ─┐
  48 kHz, 16-bit stereo ──── nRF master ──▶│                            ├─▶ DAC_ROUTE0 ─▶ DAC ─▶ speaker
  5 ms blocks, ramped gain                 │  (hear-through FastDSP    │
                                           │   path idle while routed  │
                                           └   away)                   ┘
adau1860_control.c
  level_db → g ; DAC_ROUTE0 ← I2S (muted switch) ; restore on stop
```

Everything on the codec side of this picture — serial port 0 as slave with
16 BCLKs per slot, ASRCI0 powered, the DAC/headphone amp on — is already
configured at boot by `adau1860_control_init()`, register-for-register from
upstream OpenEarable's bring-up (which streams the phone's music down this
exact path). What the tone adds at runtime is one route switch.

## nRF side — `src/tone_gen.c`

- **Synthesis**: 32-bit phase accumulator, 1024-entry sine table with 8-bit
  linear interpolation. Frequency error < 1e-5 Hz; THD well below what the
  transducer adds. Both I2S channels carry the same sample.
- **Level**: linear gain in Q15 (32768 = 0 dBFS), applied per block with a
  linear ramp from the previous block's gain to the new target, so a
  `TONE_LEVEL` step, the fade-in at start and the fade-out at stop are all
  click-free. Tested: the largest frame-to-frame step during a
  −40 dBFS → 0 dBFS ramp stays under the sine's own slope at full scale.
- **Streaming**: Zephyr `i2s` API, TX only, `I2S_OPT_BIT_CLK_MASTER |
  I2S_OPT_FRAME_CLK_MASTER`, 48 kHz, 16-bit, stereo, 240-frame (5 ms,
  960-byte) blocks from a 6-block slab. A dedicated thread
  (`K_PRIO_PREEMPT(2)`) queues two blocks before `I2S_TRIGGER_START` and
  then keeps writing (`i2s_write()` blocks when the driver's queue is full).
  Stop = one block ramping to zero, `I2S_TRIGGER_DRAIN`, wait for the queue
  to play out, then a callback into `adau1860_control.c`. An I2S error drops
  and reconfigures the peripheral and ends the tone the same way, so the
  codec is never left routed to a dead link.
- **Clocking**: `clock-source = "ACLK"` with `hfclkaudio-frequency =
  12288000` on both board files → exact 48 kHz LRCLK, 1.536 MHz BCLK. This
  is how upstream drives its I2S (`NRF_I2S_CLKSRC_ACLK`). Exactness doesn't
  matter to the codec (ASRC), it just costs nothing.

## Codec side — `adau1860_control.c`

`level_db → gain`: `g = 10^((level_db − CONFIG_HAVEN_TONE_FULL_SCALE_DB)/20)`,
clamped to [0, 1]. Default full-scale constant 85, so the protocol ceiling
(`PROTOCOL_TONE_LEVEL_MAX_DB`, enforced upstream of this in `tone_safety.c`)
is exactly 0 dBFS on the link. The LDL start level (30) is −55 dBFS ≈ 58
LSB peak at 16 bit: quiet, but well clear of the noise floor.

**This constant is nominal.** It fixes the digital level, not the sound
pressure. Between it and the eardrum sit the DAC volume (`DAC_VOL0`, set to
upstream's maximum playback value), the headphone amp mode, the speaker and
the ear seal. haven-app `docs/calibration.md` is the procedure that turns
"85" into a measured dB SPL; until it has been done, no dB figure in this
system is physical. After it, this constant is replaced by a per-frequency
table (the LDL test uses six fixed frequencies) and the FastDSP limiter is
set as a hardware ceiling behind the software clamps.

**Routing** — `CONFIG_HAVEN_TONE_ROUTE` choice:

| | What happens | Why / caveats |
|---|---|---|
| `DAC_DIRECT` (default) | On start: DAC soft-mute → `DAC_ROUTE0 = 0` (I2S) → wait for `STATUS2[2]` (input ASRC lock, bounded 200 ms) → unmute. On stop (after the link is quiet): mute → `DAC_ROUTE0 = 32` (FastDSP ch 0) → unmute. | Upstream's non-DSP playback configuration (`SAI_CLK_PWR` I2S_IN, `ASRC_PWR` ASRCI0, `DAC_ROUTE0 = 0`), so the route is known to make sound. Hear-through is **paused** during the tone, and the tone cannot pass through the wearer's own notch filters — an LDL test at 4 kHz through a 4 kHz notch would be meaningless. The FastDSP keeps running with the bands intact; only the DAC's source changes. |
| `FDSP_MIX` | No codec writes; the DAC stays on FastDSP ch 0. | Assumes upstream's program mixes the I2S/ASRC path in at slot 8, after the biquads, at unity. Whether it does, and pre- or post-filter, is not established (the 12 program words aren't decoded). Hardware experiment only. |

The route switch is idempotent and mutex-guarded, and it runs from two
places on purpose: the feeder thread's stopped-callback (the normal path,
after the fade-out has played) and, synchronously, from
`adau1860_control_on_ble_disconnected()` — so a dropped phone link restores
hear-through even if the feeder thread is wedged. `tone_safety.c` is
unchanged: it still clamps, still runs the 3 s keep-alive watchdog, and
still calls the same three functions.

## What the host tests establish

`tests/host/test_tone_path.c` (169 checks) runs the real `tone_gen.c` and
`adau1860_control.c` against fakes whose I2S side keeps the samples:

- frequency of the generated signal within 0.1 % at all six LDL frequencies;
  full-scale amplitude without wrap; exact silence at zero gain;
- gain ramps bounded (no clicks), landing exactly on target; phase
  continuity across block boundaries (two blocks ≡ one double block);
- `level_db → gain`: 85 → 0 dBFS, 65 → −20 dB, 30 → 58 LSB, monotonic in
  2 dB steps, saturating not wrapping;
- I2S configured master / 48 kHz / 16-bit / stereo / 4-byte-multiple blocks;
  every block handed to the driver is returned to the slab; a failed write
  doesn't leak;
- start/stop/retune state machine including start-during-drain;
- codec write order on start (mute → route → unmute), no safeloads touched,
  level changes produce no I2C traffic, route restored only after the link
  is quiet, restore is idempotent, BLE disconnect restores synchronously,
  and the two degraded modes (codec absent → nRF-only; I2S absent → codec
  route still restored).

Not established: real I2S timing, that `DAC_ROUTE0 = 0` is the ASRCI0 output
rather than the raw serial-port slot (upstream's no-DSP config implies it,
which is why the route is paired with ASRCI0 being powered), the ASRC lock
bit position beyond upstream's usage, and — the big one — how loud any of
this is in an ear.

## First hardware checks

1. Scope BCLK/LRCLK on the DK header (or the board's P1.10 / P0.30) after a
   `TONE_START`: 1.536 MHz and 48.000 kHz. If they are ~47.6 kHz, the ACLK
   clock source didn't take.
2. RTT: `Tone: f0=... -> gain .../32768 (DAC<-I2S, ...)`, no `Input ASRC did
   not report lock` warning. That warning means the codec's serial port
   isn't seeing the clocks — check pinctrl (SDOUT P0.28 → codec `SDATAI_0`).
3. Tone audible at 60 dB commanded; silent at `TONE_STOP`; hear-through
   returns within ~50 ms of stop. Pull the phone's Bluetooth mid-tone: the
   tone stops within 3 s (watchdog) and hear-through returns.
4. Sweep `TONE_START` at 30 and 85 with an ear simulator on the earpiece →
   begin `docs/calibration.md`. Do this before anyone wears the device for
   an LDL test.
