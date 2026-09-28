#!/usr/bin/env python3
"""Quantify what Q5.27 fixed-point does to Haven's notch / peaking-cut biquads
at the two FastDSP frame rates on the table: 48 kHz and 192 kHz.

Context: upstream OpenEarable frame-clocks the ADAU1860 FastDSP from the DMIC
stream at 192 kHz (FDSP_CTRL4 = 2, DMIC_CTRL2 = 0x04). Haven's coefficient math
(`src/adau1860_control.c`, RBJ cookbook) assumes 48 kHz. Whichever rate the DSP
actually runs at, the coefficients have to be computed *for that rate* and then
squeezed into 32-bit Q5.27 words. ADI's EVAL-ADAU1860 user guide (UG-2017, p.7)
says the same thing about its own Lark Studio tool: "set fs to be the same as
the FastDSP source, FDSP_RATE_SOURCE" -- i.e. the FastDSP rate is whatever
FDSP_CTRL4 selects as the frame-rate source, and coefficients must match it. Narrow notches at low f0 and high fs put poles
very close to the unit circle, which is exactly where fixed point hurts.

Two independent effects are modelled, and reported separately:

  A. Coefficient quantisation only (exact arithmetic). Round the five RBJ
     coefficients to Q5.27, then evaluate the resulting transfer function with
     scipy.signal.freqz. This is a hard fact about the *design*: no matter how
     wide the DSP's internal datapath is, it cannot do better than this.

  B. Coefficient + state quantisation, a direct-form-I biquad whose two delay
     lines and output are rounded to Q5.27 every sample (double-precision
     multiply-accumulate). This is a MODEL of a 32-bit datapath, not a
     bit-accurate FastDSP simulation -- ADI does not document FastDSP's
     accumulator width. Treat B as "the order of magnitude of noise-floor and
     depth loss you'd see if FastDSP keeps 32-bit state", nothing more.

Haven's parameter space (src/protocol.h clamps):
  f0 200..8000 Hz, Q 1..20, atten 3..40 dB (>= 40 dB == pure notch).

Run:  python3 fixed_point_notch.py [--out results/] [--quick]
Writes results/notch_quantisation.md, results/*.png
"""
import argparse
import os
import sys

import numpy as np
from scipy.signal import freqz

FRAC = 27
LSB = 1 / (1 << FRAC)
F0_MIN, F0_MAX = 200.0, 8000.0
Q_MIN, Q_MAX = 1.0, 20.0
ATTEN_NOTCH = 40.0
RATES = (48000.0, 192000.0)


# ── the exact math haven-zephyr-app uses (calc_band_coeffs) ──────────────────

def haven_coeffs(f0, q, atten_db, fs):
    w0 = 2 * np.pi * f0 / fs
    alpha = np.sin(w0) / (2 * q)
    c = np.cos(w0)
    if atten_db >= ATTEN_NOTCH:
        a0 = 1 + alpha
        return np.array([1 / a0, -2 * c / a0, 1 / a0, -2 * c / a0, (1 - alpha) / a0])
    A = 10 ** (-atten_db / 40)
    a0 = 1 + alpha / A
    return np.array([(1 + alpha * A) / a0, -2 * c / a0, (1 - alpha * A) / a0,
                     -2 * c / a0, (1 - alpha / A) / a0])


def quantise(coeffs, frac=FRAC):
    q = np.round(coeffs * (1 << frac))
    lim = (1 << 31)
    if np.any(q >= lim) or np.any(q < -lim):
        return None  # not representable
    return q / (1 << frac)


# ── metric helpers ───────────────────────────────────────────────────────────

def response_db(c, freqs, fs):
    _, H = freqz(c[:3], [1.0, c[3], c[4]], worN=freqs, fs=fs)
    return 20 * np.log10(np.maximum(np.abs(H), 1e-12))


def analyse_design(f0, q, atten, fs, frac=FRAC):
    """Effect A. Returns dict of realised-vs-ideal metrics, or None if the
    quantised coefficients are unrepresentable/unstable."""
    ideal = haven_coeffs(f0, q, atten, fs)
    cq = quantise(ideal, frac)
    if cq is None:
        return None
    poles = np.roots([1.0, cq[3], cq[4]])
    pole_r = float(np.max(np.abs(poles)))
    if pole_r >= 1.0:
        return dict(unstable=True, pole_radius=pole_r)

    bw = f0 / q
    # Coarse grid around f0 (± 3 bandwidths, at least ± 2 Hz) for the -3 dB
    # width, then a two-stage refinement so the located minimum is accurate to
    # ~1e-6 Hz rather than to the coarse step (which would swamp the tiny
    # centre-frequency shifts we're trying to measure).
    span = max(3 * bw, 2.0)
    grid = np.linspace(max(1.0, f0 - span), min(fs / 2 - 1, f0 + span), 4001)
    ideal_db = response_db(ideal, grid, fs)
    quant_db = response_db(cq, grid, fs)
    i_ideal = int(np.argmin(ideal_db))
    i_quant = int(np.argmin(quant_db))

    def refine(c, f_start, step):
        f_lo, f_hi = f_start - 2 * step, f_start + 2 * step
        for _ in range(4):
            g = np.linspace(f_lo, f_hi, 401)
            d = response_db(c, g, fs)
            k = int(np.argmin(d))
            h = (f_hi - f_lo) / 400
            f_lo, f_hi = g[k] - 2 * h, g[k] + 2 * h
            f_min, d_min = g[k], d[k]
        return f_min, -d_min

    step = grid[1] - grid[0]
    f_ideal_min, depth_ideal = refine(ideal, grid[i_ideal], step)
    f_quant_min, depth_quant = refine(cq, grid[i_quant], step)
    at_f0 = -response_db(cq, np.array([f0]), fs)[0]
    pure_notch = atten >= ATTEN_NOTCH

    def bw3(db, fmin_idx):
        level = db[fmin_idx] + 3.0  # 3 dB above the notch floor
        left = fmin_idx
        while left > 0 and db[left] < level:
            left -= 1
        right = fmin_idx
        while right < len(db) - 1 and db[right] < level:
            right += 1
        return grid[right] - grid[left]

    return dict(
        unstable=False,
        pole_radius=pole_r,
        pole_margin_lsb=(1.0 - pole_r) / LSB,
        depth_ideal_db=depth_ideal,
        depth_quant_db=depth_quant,
        depth_at_f0_db=at_f0,
        # For a pure notch the ideal depth is infinite (zeros exactly on the
        # unit circle), so "error" is meaningless -- report realised depth
        # instead (depth_quant_db) and leave this NaN.
        depth_err_db=np.nan if pure_notch else depth_quant - depth_ideal,
        f0_err_hz=f_quant_min - f_ideal_min,
        f0_err_cents=1200 * np.log2(f_quant_min / f_ideal_min),
        bw3_ideal_hz=bw3(ideal_db, i_ideal),
        bw3_quant_hz=bw3(quant_db, i_quant),
    )


def simulate_df1(c, x, frac=FRAC):
    """Effect B: DF-I biquad, double MAC, state + output rounded to Q(frac)."""
    scale = float(1 << frac)
    b0, b1, b2, a1, a2 = c
    y = np.empty_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for n in range(len(x)):
        acc = b0 * x[n] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        yn = np.round(acc * scale) / scale
        y[n] = yn
        x2, x1 = x1, x[n]
        y2, y1 = y1, yn
    return y


def analyse_state(f0, q, atten, fs, frac=FRAC, amp=0.25, seconds=None):
    """Effect B for one design. Drive with a full-scale-ish tone at f0 plus a
    quiet second tone one bandwidth away (which must survive), measure realised
    attenuation of each and the broadband noise floor the rounding adds."""
    cq = quantise(haven_coeffs(f0, q, atten, fs), frac)
    if cq is None:
        return None
    bw = f0 / q
    # Long enough for the narrowest resonator to settle: ~ 10 time constants.
    tau = 1 / (np.pi * bw)
    dur = seconds or max(0.5, 12 * tau)
    n = int(dur * fs)
    n = 1 << int(np.ceil(np.log2(n)))
    t = np.arange(n) / fs
    # Side tone 3 bandwidths above f0, but never past ~Nyquist (8 kHz, Q=1 at
    # 48 kHz would otherwise put it at 32 kHz and alias).
    f_side = f0 + min(3 * bw, 0.4 * (fs / 2 - f0))
    x = amp * np.sin(2 * np.pi * f0 * t) + 0.01 * amp * np.sin(2 * np.pi * f_side * t)
    x = np.round(x * (1 << frac)) / (1 << frac)
    y = simulate_df1(cq, x, frac)
    # analyse the settled second half
    seg = slice(n // 2, n)
    win = np.hanning(n // 2)
    def tone_level(sig, f):
        ph = np.exp(-2j * np.pi * f * t[seg])
        return 20 * np.log10(abs(np.sum(sig[seg] * win * ph)) / np.sum(win) * 2 + 1e-30)
    att_f0 = tone_level(x, f0) - tone_level(y, f0)
    att_side = tone_level(x, f_side) - tone_level(y, f_side)
    ideal_side = -response_db(cq, np.array([f_side]), fs)[0]
    # Noise floor: remove both tones by projecting them out, measure residual.
    resid = y[seg].copy()
    for f in (f0, f_side):
        ph = np.exp(2j * np.pi * f * t[seg])
        comp = np.sum(resid * np.conj(ph)) / len(resid)
        resid = resid - 2 * np.real(comp * ph)
    noise_dbfs = 20 * np.log10(np.sqrt(np.mean(resid ** 2)) + 1e-30)
    return dict(att_f0_db=att_f0, att_side_db=att_side, ideal_side_db=ideal_side,
                noise_floor_dbfs=noise_dbfs, sim_seconds=n / fs)


# ── sweeps ───────────────────────────────────────────────────────────────────

def sweep_design(f0s, qs, attens):
    out = {}
    for fs in RATES:
        for atten in attens:
            grid = np.empty((len(qs), len(f0s)), dtype=object)
            unstable = 0
            for i, q in enumerate(qs):
                for j, f0 in enumerate(f0s):
                    r = analyse_design(f0, q, atten, fs)
                    if r is None or r["unstable"]:
                        unstable += 1
                        continue
                    grid[i, j] = r
            out[(fs, atten)] = (grid, unstable)
    return out


def worst_rows(out, key, f0s, qs, n=3, largest=True):
    rows = []
    for (fs, atten), (grid, _) in out.items():
        vals = np.array([[np.nan if not isinstance(g, dict) else g[key] for g in row] for row in grid])
        flat = np.argsort(np.nan_to_num(np.abs(vals), nan=-np.inf).ravel())[::-1] if largest else \
            np.argsort(np.nan_to_num(vals, nan=np.inf).ravel())
        for k in flat[:n]:
            i, j = divmod(int(k), len(f0s))
            rows.append((fs, atten, f0s[j], qs[i], grid[i, j]))
    return rows


def fmt_md_table(headers, rows):
    s = "| " + " | ".join(headers) + " |\n|" + "|".join("---" for _ in headers) + "|\n"
    for r in rows:
        s += "| " + " | ".join(r) + " |\n"
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results")
    ap.add_argument("--quick", action="store_true", help="coarser grid, fewer sims")
    ap.add_argument("--no-plots", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    nf, nq = (9, 6) if args.quick else (25, 15)
    f0s = np.geomspace(F0_MIN, F0_MAX, nf)
    qs = np.geomspace(Q_MIN, Q_MAX, nq)
    attens = (ATTEN_NOTCH, 20.0, 6.0)

    print("Effect A: coefficient quantisation only")
    out = sweep_design(f0s, qs, attens)

    md = ["# Q5.27 quantisation of Haven's notch / peaking-cut biquads\n",
          f"Grid: f0 {F0_MIN:.0f}–{F0_MAX:.0f} Hz ({nf} log steps), Q {Q_MIN:.0f}–{Q_MAX:.0f} "
          f"({nq} log steps), atten ∈ {{{', '.join(f'{a:.0f}' for a in attens)}}} dB "
          f"(40 = pure notch), fs ∈ {{48000, 192000}} Hz. Coefficients per "
          "`src/adau1860_control.c` `calc_band_coeffs()`, rounded to Q5.27.\n",
          "## Effect A — coefficient quantisation (exact arithmetic)\n",
          "Representability / stability after rounding:\n"]
    for (fs, atten), (grid, unstable) in out.items():
        total = grid.size
        md.append(f"- fs={fs:.0f} atten={atten:.0f} dB: {total - unstable}/{total} designs stable "
                  f"and representable{'' if unstable == 0 else f' ({unstable} NOT)'}")
        print(f"  fs={fs:6.0f} atten={atten:4.0f}: unstable/unrepresentable = {unstable}/{total}")
    md.append("")

    # Summary statistics per (fs, atten)
    md.append("Worst case over the whole grid (|value| max):\n")
    hdr = ["fs (Hz)", "atten (dB)", "max |depth err| (dB)", "min depth at commanded f0 (dB)",
           "max |f0 err| (cents)", "max |f0 err| (Hz)", "min pole margin (LSBs of Q5.27)", "min pole margin (1−r)"]
    rows = []
    headline = {}
    for (fs, atten), (grid, _) in out.items():
        cells = [g for g in grid.ravel() if isinstance(g, dict)]
        de = max(abs(g["depth_err_db"]) for g in cells) if atten < ATTEN_NOTCH else float("nan")
        d0 = min(g["depth_at_f0_db"] for g in cells)
        fc = max(abs(g["f0_err_cents"]) for g in cells)
        fh = max(abs(g["f0_err_hz"]) for g in cells)
        pm = min(g["pole_margin_lsb"] for g in cells)
        pr = min(1 - g["pole_radius"] for g in cells)
        headline[(fs, atten)] = dict(depth_err=de, min_depth_at_f0=d0, f0_cents=fc, f0_hz=fh, pole_lsb=pm, pole_1r=pr)
        rows.append([f"{fs:.0f}", f"{atten:.0f}", "n/a (notch)" if np.isnan(de) else f"{de:.4f}",
                     f"{d0:.1f}" if atten >= ATTEN_NOTCH else "—",
                     f"{fc:.4f}", f"{fh:.5f}", f"{pm:.0f}", f"{pr:.2e}"])
        print(f"  fs={fs:6.0f} atten={atten:4.0f}: max|Δdepth|={de:.4f} dB  min depth@f0={d0:.1f} dB  "
              f"max|Δf0|={fc:.3f} cents ({fh:.4f} Hz)  min pole margin={pm:.0f} LSB (1-r={pr:.2e})")
    md.append(fmt_md_table(hdr, rows))

    # Pure-notch depth at the commanded frequency.
    md.append("Pure notch (atten=40 → full notch). RBJ gives b0 = b2 exactly, and rounding preserves "
              "that equality, so the quantised zeros stay *on* the unit circle: the notch is still "
              "infinitely deep, just shifted by Δf0. The Haven-relevant number is therefore the depth "
              "at the *commanded* f0, which is finite only because of that shift:\n")
    hdr = ["fs (Hz)", "f0 (Hz)", "Q", "depth at commanded f0 (dB)", "Δf0 (Hz)", "Δf0 (cents)"]
    rows = []
    for fs in RATES:
        grid, _ = out[(fs, ATTEN_NOTCH)]
        picks = [(0, 0), (0, len(f0s) - 1), (len(qs) - 1, 0), (len(qs) - 1, len(f0s) - 1), (len(qs) // 2, len(f0s) // 2)]
        for i, j in picks:
            g = grid[i, j]
            if isinstance(g, dict):
                rows.append([f"{fs:.0f}", f"{f0s[j]:.0f}", f"{qs[i]:.1f}", f"{g['depth_at_f0_db']:.1f}",
                             f"{g['f0_err_hz']:+.4f}", f"{g['f0_err_cents']:+.3f}"])
    md.append(fmt_md_table(hdr, rows))

    # Shallow-cut depth error: 6 dB cut is where a 0.1 dB error is a percentage.
    md.append("Peaking-cut depth error, worst three per (fs, atten):\n")
    hdr = ["fs (Hz)", "atten (dB)", "f0 (Hz)", "Q", "ideal depth (dB)", "Q5.27 depth (dB)", "error (dB)", "Δf0 (cents)"]
    rows = []
    for fs, atten, f0, q, g in worst_rows({k: v for k, v in out.items() if k[1] != ATTEN_NOTCH},
                                          "depth_err_db", f0s, qs):
        rows.append([f"{fs:.0f}", f"{atten:.0f}", f"{f0:.0f}", f"{q:.1f}", f"{g['depth_ideal_db']:.3f}",
                     f"{g['depth_quant_db']:.3f}", f"{g['depth_err_db']:+.4f}", f"{g['f0_err_cents']:+.3f}"])
    md.append(fmt_md_table(hdr, rows))

    # Q1.31 comparison: representability only.
    md.append("### Q1.31 for comparison\n")
    n_ok = {fs: 0 for fs in RATES}
    n_tot = 0
    for fs in RATES:
        for q in qs:
            for f0 in f0s:
                n_tot += 1 if fs == RATES[0] else 0
                if quantise(haven_coeffs(f0, q, ATTEN_NOTCH, fs), 31) is not None:
                    n_ok[fs] += 1
    md.append(f"Q1.31 cannot hold |coefficient| ≥ 1, and every Haven biquad has a1 ≈ −2cos(ω0) with "
              f"magnitude > 1. Representable pure-notch designs in Q1.31: "
              + ", ".join(f"fs={fs:.0f}: {n_ok[fs]}/{n_tot}" for fs in RATES) +
              ". Q1.31 would need coefficient pre-scaling by ½ and a compensating shift in the datapath; "
              "not applicable to FastDSP as upstream drives it (unity = 1<<27).\n")

    # Effect B
    print("Effect B: 32-bit state model (slow; a handful of designs)")
    md.append("## Effect B — coefficient + Q5.27 state rounding (direct-form-I model)\n")
    md.append("Model, not a FastDSP simulation (accumulator width undocumented). Drive: 0.25 FS tone at f0 "
              "+ a −40 dB tone 3 bandwidths above (must pass). Reports realised attenuation of each and the "
              "rounding-noise floor.\n")
    hdr = ["fs (Hz)", "f0 (Hz)", "Q", "atten cmd (dB)", "realised att @f0 (dB)", "side tone att (dB)",
           "ideal side att (dB)", "noise floor (dBFS)", "sim (s)"]
    rows = []
    cases = [(200, 20, 40), (200, 20, 20), (200, 1, 40), (4500, 10, 40), (4500, 10, 20), (8000, 20, 40), (8000, 1, 6)]
    if args.quick:
        cases = cases[:3]
    for f0, q, atten in cases:
        for fs in RATES:
            r = analyse_state(f0, q, atten, fs, seconds=0.4 if args.quick else None)
            if r is None:
                continue
            rows.append([f"{fs:.0f}", f"{f0}", f"{q}", f"{atten}", f"{r['att_f0_db']:.1f}", f"{r['att_side_db']:.2f}",
                         f"{r['ideal_side_db']:.2f}", f"{r['noise_floor_dbfs']:.1f}", f"{r['sim_seconds']:.2f}"])
            print(f"  fs={fs:6.0f} f0={f0:5d} Q={q:3d} atten={atten:3d}: att@f0={r['att_f0_db']:6.1f} dB "
                  f"side={r['att_side_db']:5.2f} (ideal {r['ideal_side_db']:5.2f}) noise={r['noise_floor_dbfs']:6.1f} dBFS")
    md.append(fmt_md_table(hdr, rows))

    if not args.no_plots:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            from matplotlib.colors import LinearSegmentedColormap

            # Single-hue sequential ramp (light → dark), recessive frame.
            cmap = LinearSegmentedColormap.from_list("seq", ["#F3F6FA", "#BFD3E6", "#7BA3C9", "#3E6FA3", "#1F3F66"])

            def heatmap_panel(ax, m, vmin, vmax, title):
                im = ax.pcolormesh(f0s, qs, m, cmap=cmap, vmin=vmin, vmax=vmax, shading="nearest")
                ax.set_xscale("log"); ax.set_yscale("log")
                ax.set_title(title, fontsize=10, loc="left")
                for s in ("top", "right"):
                    ax.spines[s].set_visible(False)
                ax.tick_params(colors="#555", labelsize=8)
                return im

            def matrix(fs, atten, key):
                grid, _ = out[(fs, atten)]
                return np.array([[abs(g[key]) if isinstance(g, dict) else np.nan for g in row] for row in grid])

            def finish(fig, axes, im, cb_label, title, fname, md_label):
                for ax in np.atleast_1d(axes).ravel():
                    ax.set_xlabel("f0 (Hz)", fontsize=9, color="#333")
                np.atleast_2d(axes)[:, 0][0].set_ylabel("Q", fontsize=9, color="#333")
                for ax in np.atleast_2d(axes)[:, 0]:
                    ax.set_ylabel("Q", fontsize=9, color="#333")
                cb = fig.colorbar(im, ax=np.atleast_1d(axes).ravel().tolist(), shrink=0.9, pad=0.02)
                cb.set_label(cb_label, fontsize=9, color="#333")
                cb.ax.tick_params(labelsize=8, colors="#555")
                fig.suptitle(title, fontsize=12, x=0.02, ha="left")
                path = os.path.join(args.out, f"{fname}.png")
                fig.savefig(path, dpi=130, bbox_inches="tight")
                plt.close(fig)
                md.append(f"![{md_label}]({fname}.png)\n")
                print(f"  wrote {path}")

            # 1. Pure notch: depth at the commanded f0 (the zeros stay on the unit
            #    circle, so depth at the shifted minimum is unbounded -- clip the
            #    display at 160 dB, far past anything audible or measurable).
            fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
            mats = [np.minimum(matrix(fs, ATTEN_NOTCH, "depth_at_f0_db"), 160.0) for fs in RATES]
            vmin, vmax = min(np.nanmin(m) for m in mats), 160.0
            for ax, fs, m in zip(axes, RATES, mats):
                im = heatmap_panel(ax, m, vmin, vmax, f"fs = {fs/1000:.0f} kHz")
            finish(fig, axes, im, "depth at commanded f0, dB (clipped at 160)",
                   "Pure notch after Q5.27 rounding — depth at the commanded frequency",
                   "notch_depth", "notch depth at commanded f0")

            # 2. Peaking cut: |depth error| for the finite-depth cuts.
            finite = [a for a in attens if a < ATTEN_NOTCH]
            fig, axes = plt.subplots(len(RATES), len(finite), figsize=(5.2 * len(finite) + 1, 3.6 * len(RATES)),
                                     sharex=True, sharey=True, squeeze=False)
            mats = {(fs, a): matrix(fs, a, "depth_err_db") for fs in RATES for a in finite}
            vmax = max(np.nanmax(m) for m in mats.values())
            for r, fs in enumerate(RATES):
                for c, a in enumerate(finite):
                    im = heatmap_panel(axes[r, c], mats[(fs, a)], 0, vmax, f"fs = {fs/1000:.0f} kHz · cut = {a:.0f} dB")
            finish(fig, axes, im, "|realised − ideal| cut depth, dB",
                   "Peaking cut after Q5.27 rounding — depth error", "depth_error", "cut depth error")

            # 3. Centre-frequency error, all designs.
            fig, axes = plt.subplots(len(RATES), len(attens), figsize=(5.2 * len(attens) + 1, 3.6 * len(RATES)),
                                     sharex=True, sharey=True, squeeze=False)
            mats = {(fs, a): matrix(fs, a, "f0_err_cents") for fs in RATES for a in attens}
            vmax = max(np.nanmax(m) for m in mats.values())
            for r, fs in enumerate(RATES):
                for c, a in enumerate(attens):
                    im = heatmap_panel(axes[r, c], mats[(fs, a)], 0, vmax,
                                       f"fs = {fs/1000:.0f} kHz · atten = {a:.0f} dB" + (" (notch)" if a >= ATTEN_NOTCH else ""))
            finish(fig, axes, im, "|centre-frequency shift|, cents",
                   "Centre-frequency shift caused by Q5.27 rounding", "f0_error", "centre-frequency error")

            # Pole margin: line chart, one line per fs, at Q = 20 (worst), pure notch.
            fig, ax = plt.subplots(figsize=(8, 4))
            for fs, color in zip(RATES, ("#3E6FA3", "#C0653A")):
                grid, _ = out[(fs, ATTEN_NOTCH)]
                y = [g["pole_margin_lsb"] if isinstance(g, dict) else np.nan for g in grid[-1]]
                ax.plot(f0s, y, color=color, lw=2, label=f"fs = {fs/1000:.0f} kHz")
                ax.annotate(f"{fs/1000:.0f} kHz", (f0s[-1], y[-1]), textcoords="offset points", xytext=(6, 0),
                            fontsize=9, color="#333", va="center")
            ax.set_xscale("log"); ax.set_yscale("log")
            ax.set_xlabel("f0 (Hz)", fontsize=9, color="#333"); ax.set_ylabel("1 − pole radius, in Q5.27 LSBs", fontsize=9, color="#333")
            ax.set_title(f"Stability margin of the quantised pure notch at Q = {Q_MAX:.0f}", fontsize=11, loc="left")
            ax.grid(True, which="major", color="#E6E6E6", lw=0.8); ax.grid(False, which="minor")
            for s in ("top", "right"):
                ax.spines[s].set_visible(False)
            ax.tick_params(colors="#555", labelsize=8)
            ax.legend(frameon=False, fontsize=9)
            path = os.path.join(args.out, "pole_margin.png")
            fig.savefig(path, dpi=130, bbox_inches="tight"); plt.close(fig)
            md.append("![pole margin](pole_margin.png)\n")
            print(f"  wrote {path}")
        except ImportError:
            md.append("_matplotlib not installed; plots skipped._\n")

    path = os.path.join(args.out, "notch_quantisation.md")
    with open(path, "w") as fh:
        fh.write("\n".join(md))
    print(f"  wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
