"""Procedure 1 (calibration.md): the tone-level map.

For each LDL test frequency, command level_db from 30 to 85 in 5 dB steps,
record the fixture microphone at each step, measure the tone's level, and fit
SPL = a*level_db + b per frequency. Also captures the noise floor with the
tone stopped. A slope far from 1 means the firmware's level_db -> gain
conversion is wrong and must be fixed THERE (not compensated in a table).

Outputs results/tone_level_map.json and results/tone_level_map.png. Levels
are reported in dB SPL only when --mic-cal is given; otherwise in dBFS with
an explicit "uncalibrated" flag, and firmware_constants.py will refuse them.

    python3 tone_level_map.py --dry-run --simulate          # rehearsal, no hardware
    python3 tone_level_map.py --mic-cal mic.json --fixture "GRAS RA0045, foam tip"
"""
import sys

from constants import LDL_TEST_FREQUENCIES_HZ, SWEEP_LEVELS_DB, TONE_LEVEL_MAX_DB
from measure import (
    a_weighted_level_dbfs,
    broadband_level_dbfs,
    dbfs_to_dbspl,
    fit_line,
    slope_is_unity,
    tone_level_dbfs,
)
from rig import Rig, common_parser, maybe_list_devices, provenance, write_json


def run_map(rig, frequencies=LDL_TEST_FREQUENCIES_HZ, levels=SWEEP_LEVELS_DB, seal="best"):
    """Returns the per-frequency results dict. Pure orchestration; the math
    lives in measure.py and is tested there."""
    fs = rig.args.fs
    rig.tone_stop()
    floor = rig.record()
    floor_row = {"broadband_dbfs": broadband_level_dbfs(floor),
                 "a_weighted_dbfs": a_weighted_level_dbfs(floor, fs)}
    per_freq = {}
    for f0 in frequencies:
        rows = []
        rig.tone_start(f0, levels[0])
        for lvl in levels:
            rig.tone_level(lvl)
            x = rig.record()
            dbfs, meas_hz = tone_level_dbfs(x, fs, f0)
            spl = dbfs_to_dbspl(dbfs, rig.mic_cal)
            rows.append({"level_db": lvl, "tone_dbfs": dbfs, "measured_hz": meas_hz,
                         "tone_dbspl": spl, "a_weighted_dbfs": a_weighted_level_dbfs(x, fs)})
            rig.say(f"  f0={f0:5d} Hz  cmd={lvl:3d}  tone={dbfs:7.2f} dBFS"
                  + (f"  = {spl:6.1f} dB SPL" if spl is not None else "  (uncalibrated)"))
        rig.tone_stop()
        y = [r["tone_dbspl"] if r["tone_dbspl"] is not None else r["tone_dbfs"] for r in rows]
        a, b, rmse = fit_line([r["level_db"] for r in rows], y)
        per_freq[str(f0)] = {
            "seal": seal, "steps": rows,
            "fit": {"slope": a, "offset": b, "rmse_db": rmse, "unit": "dB SPL" if rig.mic_cal else "dBFS",
                    "slope_ok": slope_is_unity(a)},
            "at_ceiling": {"commanded_db": TONE_LEVEL_MAX_DB, "predicted": a * TONE_LEVEL_MAX_DB + b},
        }
        flag = "" if slope_is_unity(a) else "  <-- slope != 1: fix level_db->gain in FIRMWARE first"
        rig.say(f"  fit {f0} Hz: y = {a:.3f}*level + {b:.2f}  (rmse {rmse:.2f} dB){flag}")
    return {"noise_floor": floor_row, "per_frequency": per_freq}


def plot(results, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:  # matplotlib is optional
        print(f"(plot skipped: {e})")
        return
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for f0, r in results["per_frequency"].items():
        xs = [s["level_db"] for s in r["steps"]]
        ys = [s["tone_dbspl"] if s["tone_dbspl"] is not None else s["tone_dbfs"] for s in r["steps"]]
        ax.plot(xs, ys, marker="o", label=f"{f0} Hz (slope {r['fit']['slope']:.2f})")
    ax.axvline(TONE_LEVEL_MAX_DB, ls="--", color="k", lw=0.8)
    ax.set_xlabel("commanded level_db (nominal)")
    ax.set_ylabel("measured tone level (" + next(iter(results["per_frequency"].values()))["fit"]["unit"] + ")")
    ax.set_title("Tone-level map" + (" -- SIMULATED" if results.get("provenance", {}).get("simulated_audio") else ""))
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    print(f"wrote {path}")


def main(argv=None):
    p = common_parser(__doc__.splitlines()[0])
    p.add_argument("--frequencies", type=int, nargs="*", default=LDL_TEST_FREQUENCIES_HZ)
    p.add_argument("--levels", type=int, nargs="*", default=SWEEP_LEVELS_DB)
    p.add_argument("--seal", default="best", help='seal condition label recorded with the run ("best"/"worst"/...)')
    args = p.parse_args(argv)
    if maybe_list_devices(args):
        return 0
    with Rig(args) as rig:
        rig.prompt(f"Seat the earpiece in the fixture ({args.seal} seal). Tone will start at {args.levels[0]} dB nominal.")
        results = run_map(rig, args.frequencies, args.levels, args.seal)
    results["provenance"] = provenance(args, rig)
    if rig.mic_cal is None:
        results["provenance"]["uncalibrated"] = True
        print("\nNo --mic-cal given: levels are dBFS, NOT dB SPL. firmware_constants.py will refuse this file.")
    write_json(args, "tone_level_map.json", results)
    plot(results, f"{args.out}/tone_level_map.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
