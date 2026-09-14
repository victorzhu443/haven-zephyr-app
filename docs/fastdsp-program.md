# The ADAU1860 FastDSP program Haven runs

## Why this doc exists (and what it replaces)

This replaces `sigmastudio-build-spec.md`, which asked someone to build a
DSP program from scratch in SigmaStudio+ and export a parameter-RAM map.
Neither is needed, and the tool was the wrong one:

- The ADAU1860 ("Lark") is designed in ADI's **Lark Studio**, not
  SigmaStudio+ (EVAL-ADAU1860 User Guide UG-2017, www.analog.com/ADAU1860).
  Lark Studio's "Download to Target" writes plain 32-bit words into the
  FastDSP program and parameter memories — there is no `ADISIGM` blob and no
  parameter-RAM address to discover; the memory map is fixed
  (`src/adau1860_regs.h`).
- The chip's control port uses **32-bit register addresses** (`0x4000Cxxx`),
  documented register by register. The earlier scaffold's belief that "no
  public ADAU1860 register map exists" was wrong; the 16-bit SigmaDSP framing
  it implemented would have addressed nothing on this chip.
- **Upstream OpenEarable 2.0 already ships a FastDSP program for this exact
  board** (`OpenEarable/open-earable-2`, `src/drivers/Lark-fdsp.c`) and a
  driver that brings the codec up (`src/drivers/ADAU1860.cpp`). Haven's
  driver (`src/adau1860_control.c`) is a C port of that bring-up, and
  `src/lark_fdsp_program.c` is that program verbatim.

Nothing below is "datasheet-verified": the datasheet PDF was not fetchable
from the environment this was written in. The ground truth is upstream's
code, which runs on the OpenEarable 2.0 hardware Haven's board is a port
of, plus UG-2017 for the tool-level facts. Confidence levels are stated.

## What the upstream program is

Decoded from the three parameter banks in `Lark-fdsp.c` (script and full
table in the `tools/dsp/` PR):

| Slot | Stage | Evidence |
|---|---|---|
| 0–4 | Five cascaded biquads | Upstream `enum sl_address { BIQ_0 = 0, ...}`, `FDSP_USED_BANK_SIZE 5`; every slot's 5 params decode as b0 b1 b2 (-a1) (-a2) of a stable filter |
| 5 | Expander / noise gate | Upstream `EXPANDER`, shell cmd `dsp noise_gate` writes this slot |
| 6 | Volume | Upstream `fdsp_set_volume()` writes param 4 = linear gain, Q5.27 |
| 7 | Mute | Upstream `fdsp_mute()`: param 4 = 0 / 0x08000000 |
| 8 | Mixer | Upstream `MIXER` |
| 9 | Master limiter | Upstream `LIMITER_MASTER` |

Bank index doubles as upstream's audio mode (`hw_codec.h`): **0 = normal**,
**1 = transparency**, **2 = ANC**. Bank 0 zeroes all five biquads — the
mic path is silent; bank 1 has a five-stage hear-through EQ; bank 2 another
curve. The FastDSP frame-rate source is set to DMIC01 (`FDSP_CTRL4 = 2`), the
DMIC runs at 192 kHz (`DMIC_CTRL2 = 0x04`), and FastDSP channel 0 is routed
to the DAC (`DAC_ROUTE0 = 32`). Upstream's Kconfig describes the FDSP as
"required for Transparency mode and ANC".

Put together — **high confidence**: this program takes the in-ear PDM
microphone as its input, runs it through the five biquads and dynamics, mixes
in the I2S music path at slot 8, and drives the DAC. That is Haven's target
topology (mic → notch filters → speaker, MCU not in the audio loop), already
built. Haven runs bank 1 (`CONFIG_HAVEN_FDSP_BANK`) and overwrites slots 0–4
with its own bands.

What is **not** known: the exact routing inside the 12-word program (which
FDSP input channel the biquad chain reads, whether the mixer also takes the
EQ engine's output). If, on hardware, hear-through works with no BLE
connection but the notches don't audibly land, the first suspect is that the
mic reaches the DAC by a path that bypasses slots 0–4. The test for that is
the tone sweep described under "First power-up checks".

## Coefficient format (pinned by two independent upstream sources)

- **Q5.27 fixed point**, 1.0 = `0x08000000`, range [-16, 16). Evidence:
  upstream's comment "Fix point arithmetic 5.27" (`Equalizer.cpp`),
  `fdsp_set_volume()` using `1 << 27` as unity, and the golden test below.
- **Slot parameter order: p0 = b0, p1 = b1, p2 = b2, p3 = −a1, p4 = −a2**
  (RBJ a0-normalised; the FastDSP *adds* its feedback taps). Evidence:
  every non-trivial biquad in upstream's shipped banks has |pole| < 1 read
  this way and |pole| ≈ 2.4 read un-negated (`tests/host/test_adau1860_coeffs.c`,
  `test_upstream_fdsp_banks_only_stable_with_negated_feedback`). Bank 1
  slot 0 is a textbook example: b1 = −1.99501, p3 = +1.99501.
- The nRF-side *software* EQ table in upstream `Equalizer.cpp` uses the
  un-negated RBJ convention (its recurrence subtracts the a-taps). Its row
  annotated `f: 150 G: -8.0dB Q: 1.0` recomputed at 48 kHz matches the table
  to ~3e-6 — that is the golden test for scale and b-ordering
  (`test_golden_upstream_equalizer_row_150hz`). Don't confuse the two
  tables: same number format, opposite feedback sign, different consumer.

## Sample rate

Coefficients must be designed at the FastDSP frame rate ("set fs to be the
same as the FastDSP source, FDSP_RATE_SOURCE" — UG-2017). Upstream's frame
source is DMIC01 at **192 kHz**, so `CONFIG_HAVEN_FDSP_RATE_HZ` defaults to
192000. Supporting evidence: upstream's bank corner frequencies decode to
round numbers under 192 kHz (bank 1 slot 1 → 3600 Hz, the same 3600 Hz band
its software EQ uses; slot 2 → a 16 kHz low-pass, sensible anti-imaging for
a 192 kHz mic path; under 48 kHz that slot would be a 4 kHz low-pass, which
would gut speech in a transparency mode). **Medium-high confidence.**

Does Q5.27 at 192 kHz have enough precision for Haven's narrow, low
notches? Yes — `tools/dsp/results/notch_quantisation.md` sweeps the whole
protocol range (f0 200–8000 Hz, Q 1–20, 3–40 dB) and finds every design
stable after quantisation, the worst pure notch still ≥ 48.9 dB deep and the
centre-frequency error ≤ 0.2 cents. The firmware computes coefficients in
double before encoding, and the worst protocol-allowed band (200 Hz, Q = 20)
is host-tested to survive quantisation.

Golden-vector caveat (from the same analysis): of upstream `Equalizer.cpp`'s
rows, only the `150 Hz / −8 dB / Q = 1` row is a valid RBJ vector — the
others were generated with the Orfanidis peaking design and diverge from RBJ
toward Nyquist. The host test uses exactly that row.

## What `adau1860_control.c` does at boot

Ported register-for-register from upstream `ADAU1860::begin()` /
`setup_FDSP()` / `setup_DAC()`, bidirectional-headset branch:

1. Optional supply GPIO (`supply-gpios`, V_LS load switch) → enable GPIO
   (`enable-gpios`, board net DAC_ENABLE, nRF P0.04 → codec E4) → 35 ms
   common-mode rise wait → read `VENDOR_ID..REVISION` (logged; a NAK here
   means the codec is unpowered, not enabled, or on the wrong bus).
2. `PMU_CTRL2 = 0x01`, `CHIP_PWR = 0x10`, `CLK_CTRL13 = 0x91` → poll
   `STATUS2[7]` → `CLK_CTRL12 = 0x01`, `CLK_CTRL13 = 0x11` → poll
   `STATUS2[7] & [1]` → `CHIP_PWR = 0x45`. (UG-2017's recommended settings:
   Hibernate1 + BLOCKS_ON + CM_BST_ON, MCLK_FREQ_INDEX 24.576 MHz,
   PLL_FM_BYPASS.) Polls are bounded (200 ms) so a missing codec is an error
   at boot, not a hang.
3. DMIC/decimator/ASRC/serial-port routing (`configure_routing()`).
4. `DSP_PWR = 1`, program → `0x40008000`, banks → `0x40008100 + k·0x500 +
   n·0x100`, `FDSP_CTRL4 = 2`, bank select in `FDSP_CTRL1[1:0]`,
   `FDSP_RUN = 1`.
5. `DAC_ROUTE0 = 32` (FastDSP ch 0), headphone amp / LV mode / LDO on, DAC
   at 192 kHz, unmute.
6. Five unity safeloads → boots as flat hear-through, not upstream's EQ curve.

Wire format: 4 address bytes big-endian + payload in a single I2C write;
multi-word payloads little-endian per word (what upstream's `memcpy` of a
`uint32_t[]` on Cortex-M produces).

Not ported: the hardware EQ engine setup (`setup_EQ`; only used for music).
The ASRC-lock wait is used by the tone path (`docs/tone-path.md`), which is
also where the I2S data path lives.

## Runtime

`adau1860_control_apply_filters()` computes RBJ notch / peaking-cut
coefficients at `ADAU1860_FDSP_RATE_HZ`, encodes Q5.27, and safeloads slot
i for band i; remaining slots get unity `{0x08000000, 0, 0, 0, 0}`. Safeload
= `FDSP_SL_ADDR ← slot`, 5 words → `FDSP_SL_P0_0..`, `FDSP_SL_UPDATE ← 1`;
the engine swaps between frames so no filter ever runs on a half-written
set. Bypass = unity in all five slots. Volume and mute reuse upstream's
slots 6/7 with the other four parameters kept as exported.

## Route numbers are per-mux

The `*_ROUTE*` registers take a small integer, and **the same integer means a
different source at a different destination**. Cross-checked against the
enums in ADI's Lark SDK (proprietary; read as reference only, not copied):

| Value | `DAC_ROUTE0` / `EQ_ROUTE` | `FDEC_ROUTEn` |
|---|---|---|
| 0–15 | SAI0 slot (I2S in) | FDSP 0–15 |
| 16–31 | SAI1 slot | TDSP 0–15 |
| 32–47 | **FDSP 0–15** | ASRCI 0–3 (32–35), ADC 0–2 (36–38), **DMIC0 = 39, DMIC1 = 40**, DMIC2/3, EQ = 43 |
| 48–63 | TDSP 0–15 | — |
| 64–67 | ASRCI 0–3 | — |
| 68–70 | ADC 0–2 | — |
| 71–74 | **DMIC 0–3** | — |
| 75 | EQ (DAC only) | — |

So `DAC_ROUTE0 = 32` is the biquad chain (normal), `DAC_ROUTE0 = 71` is the
raw mic with no DSP (`CONFIG_HAVEN_DAC_SOURCE_DMIC_DIRECT`, smoke test), and
`EQ_ROUTE = 71` + `DAC_ROUTE0 = 75` would be a mic → hardware-EQ → DAC path
that needs no FastDSP program at all (untested; EQ coefficient format
unverified).

## Route B: the hardware EQ engine (`CONFIG_HAVEN_DAC_SOURCE_EQ`)

A second, independent hear-through path that needs **no FastDSP program**:
`EQ_ROUTE = 71` (DMIC0) → the codec's dedicated EQ engine → `DAC_ROUTE0 = 75`.
It exists because the FastDSP route's one unverified assumption is the
routing inside upstream's 12 program words; if hear-through is silent there,
this route does not share that failure mode.

What the firmware does (`configure_eq()`, `eq_swap_in()`):

1. `EQ_CFG ← 0x00` (stop), `EQ_CFG ← 0x10` (clear), poll `EQ_STATUS[0]`
   (bounded, 200 ms) — upstream's `setup_EQ()` sequence.
2. `EQ_ROUTE ← 71` (upstream used 64 = ASRCI0, its music path).
3. Program: upstream's 57-word `eq_program` verbatim (`src/lark_eq_program.c`),
   written to `0x4000A000` in 16-word chunks.
4. Both parameter banks (`0x4000A200`, `0x4000A400`) written flat, then
   `EQ_CFG ← 0x01` (run, bank 0).
5. `apply_filters()`/`set_bypass()` build a complete 35-word bank — Haven's
   bands in stages 0..n-1, unity for the rest, five unity gain words — write
   it to the **inactive** bank, then flip `EQ_CFG.BANK_SEL` (bit 1). The EQ
   has no safeload; the bank flip is the atomic swap.

**Coefficient format — different from the FastDSP.** `tools/dsp/eq_bank_decode.py`
tries every (number format × word order × feedback sign) against upstream's
two shipped banks; the only combination under which every stage is stable,
the unity groups `{0,0,0x01000000,0,0}` are identities and the real stages
are whole-dB EQ cuts is **28-bit two's complement 4.24** (1.0 = `0x01000000`,
bits 28–31 zero) with words **`[-a1, -a2, b0, b1, b2]`**. Upstream's bank 0
decodes to −10 (low shelf), −8, −2.5 and −7 dB stages — the same gain values
as its nRF-side software EQ table, at a different tuning. Confidence: high
for format and sign; the b0/b2 swap is magnitude-identical and rejected only
because it would make upstream's "unity" group a two-sample delay.

**Sample rate.** The EQ runs at its source's rate (UG-2017: "set fs to be
same as the equalizer source, EQ_ROUTE"); with DMIC0 that is 192 kHz, the
same `CONFIG_HAVEN_FDSP_RATE_HZ` the FastDSP math uses. Six stages are
available; Haven uses up to five.

**Not in this path:** the FastDSP's expander and master limiter (they sit in
the FastDSP program). The DAC output ceiling (below) still is. The FastDSP
keeps running with unity biquads so its output is well-defined if anything
routes from it. **UNVERIFIED on hardware** — the register sequence and the
bank format are pinned by host tests (`tests/host/test_eq_route.c`), not by
a codec.

## First power-up checks (in this order)

0. **Smoke test first.** Build once with `CONFIG_HAVEN_DAC_SOURCE_DMIC_DIRECT=y`.
   If you hear the room through the receiver, the mic, clocks, DAC and
   receiver all work; every later silence is a FastDSP routing/program
   question. Do not wear the device in this mode (no limiter). Then rebuild
   with the default (`HAVEN_DAC_SOURCE_FDSP`).
1. RTT log shows `ADAU1860 vendor 0x.. device 0x.. rev 0x..` — the control
   port is alive. A NAK/`-ENODEV`: check DAC_ENABLE (P0.04), the V_LS rail
   (P1.11 load switch), the bus (P1.00 SCL / P1.15 SDA on the real board).
2. No `Timed out waiting for ...` — clocks came up.
3. With no phone connected: speak near the device — flat hear-through means
   the mic → FDSP → DAC path is real.
   **Silent here but the step-0 smoke test worked?** Rebuild with
   `CONFIG_HAVEN_DAC_SOURCE_EQ=y` (Route B): if that is audible, the FastDSP
   program's internal routing is the problem, not the hardware.
4. Send `{"type":"MULTI_FILTER","bands":[{"f0":1000,"Q":10}]}` and sweep a
   tone from a speaker. Notch at 1000 Hz: rate and routing are right.
   Notch at 250 Hz: the FDSP frame rate is 48 kHz, not 192 kHz — set
   `CONFIG_HAVEN_FDSP_RATE_HZ=48000`. No notch anywhere while hear-through
   works: the mic bypasses slots 0–4 (see "What is not known").
5. Measure hear-through latency (impulse at the mic, scope on the speaker)
   — this is the number the whole product depends on.

## Still to do

- **LDL tone path: built, not run** — see `docs/tone-path.md`. The tone is
  synthesised on the nRF5340 (`src/tone_gen.c`), streamed over I2S0 (nRF
  master) into serial port 0 → ASRCI0, and the DAC is switched to the I2S
  source for the duration (hear-through paused; the tone never passes
  through the wearer's notches). What remains is hardware: confirm audio,
  then **acoustically calibrate** `level_db` against dB SPL at the ear
  (haven-app `docs/calibration.md`). Until then the 85 dB ceiling is a
  nominal number.
- **Confirm/adjust the FDSP program in Lark Studio** if step 4 above fails:
  open upstream's design (or rebuild: DMIC01 → 5 × biquad → expander →
  volume → mute → mixer → limiter → FDSP out 0), "Download to Target", and
  paste the exported words into `src/lark_fdsp_program.c`. UG-2017 shows the
  input source is a schematic-level choice, so a DMIC-fed chain is possible.
- **Alternative if the FDSP proves awkward — UNVERIFIED hypothesis:** the
  hardware EQ engine has an explicit input-select register (`EQ_ROUTE`;
  upstream sets 64 = ASRCI ch 0, and UG-2017 says "set fs to be the same as
  the equalizer source, EQ_ROUTE"), its own program/bank memories, and
  `DAC_ROUTE0 = 75` takes its output. Routing the DMIC/decimator channel
  into it would give a biquad hear-through path with no FDSP program at all.
  Nobody has tried this.
- Wire the GATT Volume characteristic to `adau1860_control_set_volume_pct()`
  (today only the mock pipeline consumes it).
