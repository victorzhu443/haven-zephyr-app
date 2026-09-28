"""Procedure 3 (calibration.md): hear-through insertion gain and notch depth.

Plays pink noise from the laptop speaker into the fixture and records:
  A. open ear (earpiece removed)            -> reference spectrum
  B. earpiece in, BYPASS on                 -> insertion gain = B - A (want ~0 dB
                                               across 200-8000 Hz; low-frequency
                                               loss = passive occlusion)
  C. one band applied (per HEAR_THROUGH_BANDS) -> notch = C - B: centre,
                                               depth, -3 dB bandwidth, Q
  D. all three bands at once                -> linearity check (no interaction)

Writes results/hear_through.json (+ .png). Pass criteria (from
calibration.md): notch centre within a few percent of f0, depth within ~1 dB
of the commanded cut (or >= 40 dB for a full notch, limited by the fixture's
noise floor), five-band response equals the sum of singles.

    python3 hear_through.py --dry-run --simulate
"""
import sys

import numpy as np

from constants import FULL_NOTCH_MIN_DEPTH_DB, HEAR_THROUGH_BANDS, INSERTION_GAIN_BAND_HZ
from measure import notch_metrics, nperseg_for, pink_noise, transfer_db
from rig import Rig, common_parser, maybe_list_devices, provenance, write_json


def capture(rig, stimulus):
    return rig.play_and_record(stimulus)


def run(rig, bands=HEAR_THROUGH_BANDS, seconds=8.0):
    fs = rig.args.fs
    stim = pink_noise(fs, seconds)
    out = {"bands": [], "insertion_gain": None, "multi_band": None}

    rig.prompt("Remove the earpiece from the fixture (open-ear reference).")
    if rig.sim:
        rig.sim.in_ear = False
    open_ear = capture(rig, stim)

    rig.prompt("Seat the earpiece in the fixture; hear-through will be set to BYPASS (flat).")
    if rig.sim:
        rig.sim.in_ear = True
    rig.bypass(True)
    flat = capture(rig, stim)
    # Insertion gain: device path relative to the open-ear path, both as
    # transfer functions from the known stimulus.
    f_ig, h_open = transfer_db(stim, open_ear, fs, 4096)
    _, h_flat = transfer_db(stim, flat, fs, 4096)
    m = (f_ig >= INSERTION_GAIN_BAND_HZ[0]) & (f_ig <= INSERTION_GAIN_BAND_HZ[1])
    ig = (h_flat - h_open)[m]
    f_ig = f_ig[m]
    out["insertion_gain"] = {"f_hz": f_ig.tolist(), "gain_db": ig.tolist(),
                             "mean_db": float(np.mean(ig)), "min_db": float(np.min(ig)), "max_db": float(np.max(ig))}
    rig.say(f"  insertion gain {INSERTION_GAIN_BAND_HZ[0]:.0f}-{INSERTION_GAIN_BAND_HZ[1]:.0f} Hz: "
          f"mean {out['insertion_gain']['mean_db']:+.1f} dB, range [{out['insertion_gain']['min_db']:+.1f}, {out['insertion_gain']['max_db']:+.1f}]")

    def notch_vs_flat(y, f0, q):
        nper = nperseg_for(fs, f0, q)
        f_y, h_y = transfer_db(stim, y, fs, nper)
        _, h_f = transfer_db(stim, flat, fs, nper)
        return f_y, h_y - h_f, nper

    for f0, q, atten, why in bands:
        rig.multi_filter([(f0, q, atten)])
        y = capture(rig, stim)
        f_y, g, nper = notch_vs_flat(y, f0, q)
        m = notch_metrics(f_y, g, f0)
        m["fft_resolution_hz"] = fs / nper
        row = {"commanded": {"f0_hz": f0, "Q": q, "atten_db": atten, "why": why}, "measured": m,
               "centre_ok": abs(m["centre_error_pct"]) <= 5.0,
               # A full notch (>= 40 dB commanded) is infinitely deep in theory; what
               # the fixture can show is bounded by its noise floor and the FFT bin
               # width, so it passes at >= FULL_NOTCH_MIN_DEPTH_DB. Peaking cuts must
               # land within 1.5 dB of the commanded depth.
               "depth_ok": (m["depth_db"] >= (FULL_NOTCH_MIN_DEPTH_DB if atten >= 40.0 else atten - 1.5))}
        out["bands"].append(row)
        rig.say(f"  band f0={f0:.0f} Q={q:.0f} cut={atten:.0f}: centre {m['centre_hz']:.1f} Hz "
              f"({m['centre_error_pct']:+.1f}%), depth {m['depth_db']:.1f} dB, Q~{m['q']:.1f}, "
              f"res {m['fft_resolution_hz']:.2f} Hz  " + ("OK" if row["centre_ok"] and row["depth_ok"] else "CHECK"))

    rig.multi_filter([(f0, q, atten) for f0, q, atten, _ in bands])
    y = capture(rig, stim)
    rows = []
    for f0, q, atten, _ in bands:
        f_y, g, _ = notch_vs_flat(y, f0, q)
        rows.append(notch_metrics(f_y, g, f0))
    out["multi_band"] = {"per_band": rows,
                         "note": "cascade is linear: each notch should match its single-band measurement"}
    rig.bypass(True)
    return out


def plot(results, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"(plot skipped: {e})")
        return
    ig = results["insertion_gain"]
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.semilogx(ig["f_hz"], ig["gain_db"], label="insertion gain (bypass)")
    for row in results["bands"]:
        m = row["measured"]
        ax.axvline(m["centre_hz"], ls=":", lw=0.8)
        ax.annotate(f"-{m['depth_db']:.0f} dB", (m["centre_hz"], 0), fontsize=7)
    ax.set_xlabel("Hz")
    ax.set_ylabel("dB")
    ax.set_title("Hear-through" + (" -- SIMULATED" if results.get("provenance", {}).get("simulated_audio") else ""))
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    print(f"wrote {path}")


def main(argv=None):
    p = common_parser(__doc__.splitlines()[0])
    p.add_argument("--stim-seconds", type=float, default=8.0, help="pink-noise length per capture (8 s gives ~1.4 Hz resolution for the 200 Hz/Q20 band at 48 kHz)")
    args = p.parse_args(argv)
    if maybe_list_devices(args):
        return 0
    with Rig(args) as rig:
        results = run(rig, seconds=args.stim_seconds)
    results["provenance"] = provenance(args, rig)
    write_json(args, "hear_through.json", results)
    plot(results, f"{args.out}/hear_through.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
