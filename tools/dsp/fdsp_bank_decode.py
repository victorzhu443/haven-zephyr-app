#!/usr/bin/env python3
"""Decode upstream OpenEarable's shipped FastDSP parameter banks (Lark-fdsp.c)
to settle the feedback-tap sign convention of the ADAU1860 FastDSP biquad
slots.

Why this exists: coeff_format.py proves Q5.27 / [b0,b1,b2,·,·] from upstream's
nRF-side *software* EQ table (Equalizer.cpp), where a1/a2 are stored as-is.
That table is a different consumer from the codec's FastDSP. The only data
that speaks for the hardware slots is the bank contents upstream actually
downloads into the chip — so decode those and ask: with which sign
convention are these filters stable?

Result (run it): for every non-trivial biquad in banks B and C, reading the
last two params as -a1/-a2 gives |pole| < 1 (0.77–0.998); reading them as-is
gives |pole| ≈ 1.7–2.4 — unstable, i.e. impossible for a shipping product.
The FastDSP slots therefore hold [b0, b1, b2, -a1, -a2] (Q5.27), and the
firmware encoder must negate the RBJ feedback taps before safeloading.

Usage:
    python3 fdsp_bank_decode.py [path/to/Lark-fdsp.c]
"""
import re
import sys
from pathlib import Path

import numpy as np

DEFAULT_SRC = Path(__file__).resolve().parents[3] / "open-earable-2" / "src" / "drivers" / "Lark-fdsp.c"
FRAC_BITS = 27


def q527(word: int) -> float:
    if word & 0x80000000:
        word -= 1 << 32
    return word / (1 << FRAC_BITS)


def read_bank(src: str, name: str):
    m = re.search(name + r"\[5\]\[12\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit(f"{name} not found")
    rows = re.findall(r"\{([^}]*)\}", m.group(1))
    return [[int(x, 16) for x in r.replace(" ", "").split(",") if x] for r in rows]


def pole_radius(p3: float, p4: float, negated: bool) -> float:
    den = [1.0, -p3, -p4] if negated else [1.0, p3, p4]
    return float(max(abs(np.roots(den))))


def main(path: Path) -> int:
    src = path.read_text()
    verdicts = []
    for bank in ("fdsp_param_bank_a", "fdsp_param_bank_b", "fdsp_param_bank_c"):
        rows = read_bank(src, bank)  # rows = param index 0..4, columns = slot
        print(f"== {bank} ==")
        for slot in range(5):
            b0, b1, b2, p3, p4 = (q527(rows[i][slot]) for i in range(5))
            if p3 == 0 and p4 == 0:
                kind = "unity/pass-through" if (b0, b1, b2) == (1.0, 0.0, 0.0) else "FIR / silent"
                print(f"  slot {slot}: b=({b0:+.5f},{b1:+.5f},{b2:+.5f})  no feedback  [{kind}]")
                continue
            r_asis = pole_radius(p3, p4, negated=False)
            r_neg = pole_radius(p3, p4, negated=True)
            verdicts.append((r_asis < 1, r_neg < 1))
            print(
                f"  slot {slot}: b=({b0:+.5f},{b1:+.5f},{b2:+.5f}) p3={p3:+.5f} p4={p4:+.5f}"
                f"  |pole| as-is={r_asis:.4f}  negated={r_neg:.4f}"
            )
    asis_ok = all(v[0] for v in verdicts)
    neg_ok = all(v[1] for v in verdicts)
    print()
    print(f"biquads with feedback: {len(verdicts)}")
    print(f"all stable if params are  a1, a2 (as-is):   {asis_ok}")
    print(f"all stable if params are -a1,-a2 (negated): {neg_ok}")
    if neg_ok and not asis_ok:
        print("VERDICT: FastDSP slots store [b0, b1, b2, -a1, -a2]; the encoder must negate RBJ a1/a2.")
        return 0
    print("VERDICT: inconclusive — inspect the table above.")
    return 1


if __name__ == "__main__":
    p = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    if not p.exists():
        raise SystemExit(f"Lark-fdsp.c not found at {p}; pass its path as the first argument")
    sys.exit(main(p))
