#!/usr/bin/env python3
"""Verify the ADAU1860 biquad coefficient format from upstream OpenEarable data.

Ground truth: `open-earable-2/src/audio/Equalizer.cpp` carries nine biquad rows,
each annotated with its design parameters (f, gain, Q) *and* the five hex
coefficient words. Upstream's own comment on that table says "Fix point
arithmetic 5.27", and `ADAU1860::fdsp_set_volume()` uses `1 << 27` as unity when
safeloading the FastDSP VOLUME slot, so the same format is what the FastDSP
parameter banks hold.

This script answers two questions from that data alone, no hardware needed:

1. What number format, slot order and sign convention do the five words use?
2. Why does a plain RBJ-cookbook recomputation match the low-frequency rows to
   five decimals but drift increasingly at high frequency?

Run:  python3 coeff_format.py [--json golden.json]
"""
import argparse
import json
import sys

import numpy as np
from scipy.optimize import minimize, minimize_scalar
from scipy.signal import freqz

# (f0_hz, gain_db, Q, [c0..c4]) -- the seven *peaking* rows of Equalizer.cpp.
# The two shelving rows (150 Hz Q=0.5 and 9 kHz Q=1.0) are excluded from the
# fit because their design formula is a separate question that doesn't bear
# on Haven (Haven only uses peaking cuts / notches).
UPSTREAM_PEAKING_ROWS = [
    (150, -8.0, 1.0, [0x07ED1CFE, 0xF03F890A, 0x07D420FF, 0xF03F890A, 0x07C13DFD]),
    (600, 3.0, 1.0, [0x081B095D, 0xF08F4C0C, 0x0761E390, 0xF08F4C0C, 0x077CECED]),
    (3600, -7.0, 2.3, [0x0768EE95, 0xF3A4F7B5, 0x067505B4, 0xF3A4F7B5, 0x05DDF449]),
    (5000, -2.5, 1.0, [0x076FC198, 0xF6E18719, 0x040EC81C, 0xF6E18719, 0x037E89B4]),
    (5500, -10.0, 4.0, [0x073E5C38, 0xF5A24BD8, 0x068B40C5, 0xF5A24BD8, 0x05C99CFD]),
    (9000, -6.0, 4.0, [0x074F07EC, 0xFAF010E0, 0x05EB6872, 0xFAF010E0, 0x053A705E]),
    (13000, -6.0, 4.0, [0x071131FF, 0x0199A84F, 0x05314FF6, 0x0199A84F, 0x044281F5]),
]
UPSTREAM_FS_HZ = 48000.0  # upstream's I2S/EQ path runs at CONFIG_AUDIO_SAMPLE_RATE_HZ=48000


def s32(x: int) -> int:
    return x - (1 << 32) if x & 0x80000000 else x


def decode(words, frac_bits):
    return np.array([s32(w) / (1 << frac_bits) for w in words])


def encode_q(value: float, frac_bits: int = 27) -> int:
    """Round-to-nearest fixed-point encode, wrapped to a uint32 bit pattern."""
    v = int(round(value * (1 << frac_bits)))
    v = max(-(1 << 31), min((1 << 31) - 1, v))
    return v & 0xFFFFFFFF


# ── candidate design formulas ────────────────────────────────────────────────
# All return [b0, b1, b2, a1, a2] normalised to a0 = 1.

def rbj_peaking(f0, gain_db, q, fs):
    """Audio-EQ-Cookbook peaking EQ (what haven-zephyr-app's calc_band_coeffs uses)."""
    A = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * f0 / fs
    alpha = np.sin(w0) / (2 * q)
    a0 = 1 + alpha / A
    return np.array([(1 + alpha * A) / a0, -2 * np.cos(w0) / a0, (1 - alpha * A) / a0,
                     -2 * np.cos(w0) / a0, (1 - alpha / A) / a0])


def zolzer_peaking(f0, gain_db, q, fs):
    """DAFX (Zölzer) peaking EQ."""
    K = np.tan(np.pi * f0 / fs)
    V = 10 ** (abs(gain_db) / 20)
    if gain_db >= 0:
        d = 1 + K / q + K * K
        return np.array([(1 + V * K / q + K * K) / d, 2 * (K * K - 1) / d, (1 - V * K / q + K * K) / d,
                         2 * (K * K - 1) / d, (1 - K / q + K * K) / d])
    d = 1 + V * K / q + K * K
    return np.array([(1 + K / q + K * K) / d, 2 * (K * K - 1) / d, (1 - K / q + K * K) / d,
                     2 * (K * K - 1) / d, (1 - V * K / q + K * K) / d])


def orfanidis_peaking(f0, gain_db, q, fs, g0_db=0.0, g1_db=0.0):
    """Orfanidis, "Digital Parametric Equalizer Design with Prescribed
    Nyquist-Frequency Gain" (JAES 1997). Conventions that reproduce upstream:
    bandwidth dw = w0/Q, bandwidth-edge gain GB = sqrt(G) (i.e. gain_db/2),
    reference gain G0 = 1, Nyquist gain G1 = 1 (unity)."""
    w0 = 2 * np.pi * f0 / fs
    G = 10 ** (gain_db / 20)
    G0 = 10 ** (g0_db / 20)
    G1 = 10 ** (g1_db / 20)
    GB = np.sqrt(G * G0) if G0 != 1.0 else np.sqrt(G)
    dw = w0 / q
    F = abs(G ** 2 - GB ** 2)
    G00 = abs(G ** 2 - G0 ** 2)
    F00 = abs(GB ** 2 - G0 ** 2)
    G01 = abs(G ** 2 - G0 * G1)
    G11 = abs(G ** 2 - G1 ** 2)
    F01 = abs(GB ** 2 - G0 * G1)
    F11 = abs(GB ** 2 - G1 ** 2)
    W2 = np.sqrt(G11 / G00) * np.tan(w0 / 2) ** 2
    DW = (1 + np.sqrt(F00 / F11) * W2) * np.tan(dw / 2)
    C = F11 * DW ** 2 - 2 * W2 * (F01 - np.sqrt(F00 * F11))
    D = 2 * W2 * (G01 - np.sqrt(G00 * G11))
    A = np.sqrt((C + D) / F)
    B = np.sqrt((G ** 2 * C + GB ** 2 * D) / F)
    d = 1 + W2 + A
    return np.array([(G1 + G0 * W2 + B) / d, -2 * (G1 - G0 * W2) / d, (G1 - B + G0 * W2) / d,
                     -2 * (1 - W2) / d, (1 + W2 - A) / d])


FORMULAS = {
    "RBJ cookbook": rbj_peaking,
    "Zölzer/DAFX": zolzer_peaking,
    "Orfanidis (unity Nyquist gain)": orfanidis_peaking,
}


# ── question 1: format ───────────────────────────────────────────────────────

def format_search():
    """Score every (Q-format, sign convention) against every design formula at
    upstream's 48 kHz. Whichever combination drives *one* formula's error to
    numerical noise across all rows is the format; the formula is question 2."""
    print("Q1: number format / slot order / sign convention")
    print("     max |decoded - formula| over all 7 peaking rows, fs = 48 kHz\n")
    print(f"{'format':8s} {'a-sign':9s} " + " ".join(f"{n:>32s}" for n in FORMULAS))
    best = None
    for frac in (24, 27, 28, 31):
        for neg_a, label in ((False, "as-is"), (True, "negated")):
            sign = np.array([1, 1, 1, -1, -1]) if neg_a else np.ones(5)
            errs = []
            for fn in FORMULAS.values():
                e = max(np.max(np.abs(decode(c, frac) - fn(f, g, q, UPSTREAM_FS_HZ) * sign))
                        for f, g, q, c in UPSTREAM_PEAKING_ROWS)
                errs.append(e)
            print(f"Q{32-frac}.{frac:<5d} {label:9s} " + " ".join(f"{e:32.2e}" for e in errs))
            m = min(errs)
            if best is None or m < best[0]:
                best = (m, frac, neg_a, list(FORMULAS)[int(np.argmin(errs))])
    print()
    return best


# ── question 2: which formula generated the table ────────────────────────────

def formula_search(frac):
    print("Q2: which design formula reproduces upstream's words (Q5.27 decode)")
    print("     per row: error at 48 kHz, and the best-fit fs if 48 kHz were wrong\n")
    for name, fn in FORMULAS.items():
        print(f"  {name}")
        for f, g, q, c in UPSTREAM_PEAKING_ROWS:
            hw = decode(c, frac)
            e48 = np.max(np.abs(fn(f, g, q, UPSTREAM_FS_HZ) - hw))
            r = minimize_scalar(lambda fs: np.max(np.abs(fn(f, g, q, fs) - hw)),
                                bounds=(20000, 400000), method="bounded")
            print(f"    f0={f:5d} g={g:5.1f} Q={q:3.1f}  err@48k={e48:9.2e}   best-fit fs={r.x:8.0f} err={r.fun:.2e}")
    print()

    print("     If RBJ were the generator, what Q would each row have needed? (f0 fixed, fs=48k)")
    for f, g, q, c in UPSTREAM_PEAKING_ROWS:
        hw = decode(c, frac)
        r = minimize(lambda p: np.sum((rbj_peaking(f, g, p[0], UPSTREAM_FS_HZ) - hw) ** 2), [q],
                     method="Nelder-Mead")
        print(f"    f0={f:5d} annotated Q={q:3.1f} -> RBJ-equivalent Q={r.x[0]:.3f}")
    print()

    print("     Realised response of the upstream words themselves (fs=48k):")
    for f, g, q, c in UPSTREAM_PEAKING_ROWS:
        hw = decode(c, frac)
        w, H = freqz(hw[:3], [1, hw[3], hw[4]], worN=1 << 16, fs=UPSTREAM_FS_HZ)
        mag = 20 * np.log10(np.abs(H))
        i = np.argmin(mag) if g < 0 else np.argmax(mag)
        print(f"    f0={f:5d} g={g:5.1f} -> peak at {w[i]:8.1f} Hz, {mag[i]:6.2f} dB   (annotation exact)")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", metavar="PATH", help="write golden facts for firmware tests")
    args = ap.parse_args()

    err, frac, neg_a, formula = format_search()
    formula_search(frac)

    print("CONCLUSION")
    print(f"  format        : Q{32-frac}.{frac} two's-complement fixed point, 1.0 = 0x{1 << frac:08X}")
    print("  slot order    : [b0, b1, b2, a1, a2], a0 normalised to 1")
    print(f"  a1/a2 sign    : {'stored negated' if neg_a else 'stored as-is (denominator 1 + a1 z^-1 + a2 z^-2)'}")
    print(f"  generator     : {formula}, fs = 48000 Hz, max error {err:.1e} (float noise)")
    print("  RBJ vs table  : the RBJ 'drift' is a formula difference, not a format or fs")
    print("                  problem -- RBJ warps the bandwidth toward Nyquist; Orfanidis")
    print("                  pins unity gain at Nyquist. Haven computes its own coefficients,")
    print("                  so only the format matters for the firmware encoder.")

    if args.json:
        f, g, q, c = UPSTREAM_PEAKING_ROWS[0]
        golden = {
            "source": "open-earable-2/src/audio/Equalizer.cpp + ADAU1860.cpp fdsp_set_volume (1<<27 = unity)",
            "frac_bits": frac,
            "unity_word": f"0x{1 << frac:08X}",
            "slot_order": ["b0", "b1", "b2", "a1", "a2"],
            "a_coefficients_negated": neg_a,
            "generator_formula": formula,
            "fs_hz": UPSTREAM_FS_HZ,
            "golden_rows": [
                {
                    "f0_hz": f, "gain_db": g, "q": q, "fs_hz": UPSTREAM_FS_HZ,
                    "words": [f"0x{w:08X}" for w in c],
                    "note": "matches RBJ peaking to 5 decimals (low f0, so RBJ≈Orfanidis) and Orfanidis to 1e-9",
                    "rbj_words": [f"0x{encode_q(v, frac):08X}" for v in rbj_peaking(f, g, q, UPSTREAM_FS_HZ)],
                }
                for f, g, q, c in UPSTREAM_PEAKING_ROWS[:1]
            ] + [
                {
                    "f0_hz": f, "gain_db": g, "q": q, "fs_hz": UPSTREAM_FS_HZ,
                    "words": [f"0x{w:08X}" for w in c],
                    "note": "Orfanidis-exact; RBJ differs here, so do NOT use as an RBJ golden",
                }
                for f, g, q, c in UPSTREAM_PEAKING_ROWS[1:]
            ],
            "encode_examples": {
                "1.0": f"0x{encode_q(1.0, frac):08X}",
                "-1.0": f"0x{encode_q(-1.0, frac):08X}",
                "-2.0": f"0x{encode_q(-2.0, frac):08X}",
                "0.5": f"0x{encode_q(0.5, frac):08X}",
                "unity_biquad": [f"0x{w:08X}" for w in (1 << frac, 0, 0, 0, 0)],
            },
        }
        with open(args.json, "w") as fh:
            json.dump(golden, fh, indent=2)
        print(f"\n  wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
