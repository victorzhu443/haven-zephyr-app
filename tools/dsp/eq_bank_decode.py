#!/usr/bin/env python3
"""Decode the ADAU1860 hardware EQ engine's parameter banks as shipped by
upstream OpenEarable (open-earable-2/src/drivers/Lark-eq.c) to settle the
coefficient format the engine wants -- the prerequisite for Haven's
"Route B" hear-through path (mic -> hardware EQ -> DAC, no FastDSP program).

Method: the two banks (35 words each) are tried under every combination of
  * number format: 32-bit signed 8.24, 28-bit signed 4.24, 32-bit 5.27,
    28-bit 1.27;
  * 5-word layout: every permutation of [b0, b1, b2, a1, a2];
  * feedback sign: a1/a2 stored as-is or negated;
and a candidate survives only if every decoded biquad is stable and the
"unity" groups ({0, 0, 0x01000000, 0, 0}) decode to an identity filter.
Survivors are ranked by how plausible their magnitude responses are for an
EQ (smallest maximum |gain| deviation).

Result (run it):
  * format  : 28-bit two's complement, 24 fractional bits ("4.24";
              1.0 = 0x01000000, range [-8, 8)); bit 27 is the sign bit,
              bits 28-31 are always 0 in upstream's data.
  * layout  : [-a1, -a2, b0, b1, b2]  (feedback taps first and NEGATED,
              then the feed-forward taps) -- the only layout under which the
              unity group {0,0,1,0,0} is an identity with b0 = 1.
  * sanity  : bank 0's four real biquads decode to cuts of exactly -10 dB
              (a low shelf, -10 -> -2 dB), -8, -2.5 and -7 dB -- integer /
              half-integer gains, which is what a designer tool exports and
              what random bit-reinterpretation never produces. The same four
              gain values appear in upstream's nRF-side software EQ table
              (Equalizer.cpp), though at different centre frequencies (this
              bank is a separate tuning: with fs = 48 kHz the cuts sit at
              ~3600, ~5000 and ~4600 Hz plus the low shelf). Under every
              other format the same words give +-18 dB swings or unstable
              poles.
  * the 7th group {1,1,1,1,1} is not a biquad (a 5-gain stage, all unity);
    Haven leaves it as shipped.
  * fs: the EQ engine runs at its source's rate (UG-2017: "set fs to be same
    as the equalizer source, EQ_ROUTE"). Upstream feeds it ASRCI0 (48 kHz
    music); Haven's Route B feeds it DMIC0 at 192 kHz, so Haven's EQ
    coefficients are computed at CONFIG_HAVEN_FDSP_RATE_HZ, same as the
    FastDSP path.

Caveat: the b0/b2 swap ([−a1, −a2, b2, b1, b0]) has an identical magnitude
response and also survives; it is rejected because it would make upstream's
"unity" group a pure two-sample delay rather than an identity. Confidence:
high for format and sign, high for layout up to that (phase-only) ambiguity.

Usage: python3 eq_bank_decode.py [--fs 48000]
"""
import argparse
import itertools
import math

import numpy as np

EQ_BANK0 = [
    0x01E51476, 0x0F1A1CBD, 0x00C76C97, 0x0E7E7E85, 0x00BA5649,
    0x018A7D5F, 0x0F4540FB, 0x00EB2887, 0x0E7582A1, 0x00CF967E,
    0x012C8DDB, 0x0F8528EA, 0x00EF5915, 0x0ED37225, 0x008B7E01,
    0x017D88EB, 0x0F310B0E, 0x00F26E8A, 0x0E827715, 0x00DC8668,
    0x00000000, 0x00000000, 0x01000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x01000000, 0x00000000, 0x00000000,
    0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000,
]
EQ_BANK1 = [0x01DE28C5, 0x0F1DB6F9, 0x0101D018, 0x0E21D73B, 0x00E078EF] * 6 + [0x01000000] * 5

NAMES = ["b0", "b1", "b2", "a1", "a2"]
FORMATS = {
    "s32.24": (32, 24),
    "s28.24": (28, 24),
    "s32.27": (32, 27),
    "s28.27": (28, 27),
}


def decode(word: int, bits: int, frac: int) -> float:
    if word & (1 << (bits - 1)):
        word -= 1 << bits
    return word / (1 << frac)


def encode_4_24(value: float) -> int:
    """Haven's encoder for the EQ engine: 28-bit two's complement, 24 frac bits."""
    v = int(round(value * (1 << 24)))
    v = max(-(1 << 27), min((1 << 27) - 1, v))
    return v & 0x0FFFFFFF


def response_db(b0, b1, b2, a1, a2, n=2048):
    w = np.linspace(1e-4, math.pi, n)
    z = np.exp(-1j * w)
    h = (b0 + b1 * z + b2 * z**2) / (1 + a1 * z + a2 * z**2)
    return w, 20 * np.log10(np.abs(h))


def survivors():
    groups = [EQ_BANK0[i * 5:i * 5 + 5] for i in range(6)] + [EQ_BANK1[0:5]]
    out = []
    for fmt, (bits, frac) in FORMATS.items():
        for perm in itertools.permutations(range(5)):
            for negated in (False, True):
                ok, worst = True, 0.0
                for g in groups:
                    vals = [decode(wd, bits, frac) for wd in g]
                    d = {NAMES[perm[i]]: vals[i] for i in range(5)}
                    a1, a2 = d["a1"], d["a2"]
                    if negated:
                        a1, a2 = -a1, -a2
                    if a1 == 0 and a2 == 0 and d["b1"] == 0 and d["b2"] == 0:
                        if abs(d["b0"] - 1.0) > 1e-9:
                            ok = False
                            break
                        continue
                    if np.abs(np.roots([1, a1, a2])).max() >= 1.0:
                        ok = False
                        break
                    _, mag = response_db(d["b0"], d["b1"], d["b2"], a1, a2)
                    worst = max(worst, float(np.abs(mag).max()))
                if ok:
                    out.append((worst, fmt, perm, negated))
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fs", type=float, default=48000.0, help="assumed EQ sample rate for the centre-frequency check")
    args = ap.parse_args()

    res = survivors()
    print(f"{len(res)} (format, layout, sign) candidates leave every biquad stable and the unity groups flat.")
    print("Best 4 by smallest maximum |gain|:")
    for worst, fmt, perm, neg in res[:4]:
        print(f"  {fmt}  layout={[NAMES[p] for p in perm]}  feedback_negated={neg}  max|gain|={worst:.1f} dB")

    # The winner, spelled out: s28.24, [-a1, -a2, b0, b1, b2].
    bits, frac = FORMATS["s28.24"]
    print(f"\nWinner decoded at fs = {args.fs:.0f} Hz (layout [-a1, -a2, b0, b1, b2], s28.24):")
    for i in range(7):
        g = (EQ_BANK0[i * 5:i * 5 + 5])
        na1, na2, b0, b1, b2 = [decode(wd, bits, frac) for wd in g]
        a1, a2 = -na1, -na2
        if a1 == 0 and a2 == 0 and b1 == 0 and b2 == 0:
            print(f"  biquad {i}: identity (b0 = {b0:g})")
            continue
        if i == 6:
            print(f"  group {i}: five gain words = {[decode(wd, bits, frac) for wd in g]} (not a biquad)")
            continue
        w, mag = response_db(b0, b1, b2, a1, a2)
        k = int(np.argmin(mag))
        f_hz = w[k] / (2 * math.pi) * args.fs
        print(f"  biquad {i}: peak gain {mag.max():+.2f} dB, min {mag.min():+.2f} dB at {f_hz:7.0f} Hz, |pole| = {np.abs(np.roots([1, a1, a2])).max():.4f}")
    print("\nThe same gain VALUES (-10, -8, -2.5, -7 dB) appear in upstream's software EQ table (Equalizer.cpp);\n"
          "the centre frequencies differ -- this bank is a separate tuning by the same tool, not a copy of that table.")

    # Encoder round trip for Haven's use.
    for v in (1.0, -1.0, 0.5, -1.99, 7.999):
        w = encode_4_24(v)
        print(f"  encode_4_24({v:+.4f}) = 0x{w:08X} -> {decode(w, 28, 24):+.6f}")


if __name__ == "__main__":
    main()
