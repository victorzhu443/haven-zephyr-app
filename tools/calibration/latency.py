"""Hear-through latency: play a click, record open-ear and in-ear, and
cross-correlate. Reports the delay in ms with sub-sample interpolation.

Why it matters: processed sound that arrives Δt after the sound leaking past
the eartip comb-filters at f = (2k+1)/(2Δt). At Δt = 1 ms the first notch is
at 500 Hz -- inside speech. The product premise needs Δt well under 1 ms
(LATENCY_TARGET_S), which only in-codec processing can deliver.

Resolution: one sample is 1/fs (20.8 µs at 48 kHz, 10.4 µs at 96 kHz). The
parabolic fit gets below that on clean clicks, but treat anything under
~1/(2·fs) as "not resolvable" -- use the highest fs the interface supports.
The laptop/interface round-trip is measured separately (open-ear pass) and
subtracted, so only the device's contribution is reported.

    python3 latency.py --dry-run --simulate
"""
import sys

from constants import LATENCY_TARGET_S
from measure import click, delay_seconds
from rig import Rig, common_parser, maybe_list_devices, provenance, write_json


def run(rig, repeats=5, seconds=0.5):
    fs = rig.args.fs
    stim = click(fs, seconds)
    rig.prompt("Remove the earpiece (open-ear reference for the interface's own round-trip).")
    if rig.sim:
        rig.sim.in_ear = False
    ref = [delay_seconds(stim, rig.play_and_record(stim), fs) for _ in range(repeats)]
    rig.prompt("Seat the earpiece; hear-through in BYPASS.")
    if rig.sim:
        rig.sim.in_ear = True
    rig.bypass(True)
    dev = [delay_seconds(stim, rig.play_and_record(stim), fs) for _ in range(repeats)]
    ref_s = sorted(r["delay_s"] for r in ref)[len(ref) // 2]
    dev_s = sorted(r["delay_s"] for r in dev)[len(dev) // 2]
    device_latency = dev_s - ref_s
    res = {
        "fs_hz": fs, "repeats": repeats,
        "interface_roundtrip_s": ref_s, "in_ear_total_s": dev_s,
        "device_latency_s": device_latency, "device_latency_ms": 1e3 * device_latency,
        "resolution_s": 1.0 / fs,
        "min_peak_corr": min(r["peak_corr"] for r in ref + dev),
        "target_s": LATENCY_TARGET_S,
        "passed": device_latency <= LATENCY_TARGET_S,
        "first_comb_notch_hz": (1.0 / (2.0 * device_latency)) if device_latency > 0 else None,
    }
    rig.say(f"  interface round-trip {1e3*ref_s:.3f} ms; in-ear {1e3*dev_s:.3f} ms; "
          f"device {1e3*device_latency:.3f} ms (resolution {1e6/fs:.1f} µs)  "
          + ("OK" if res["passed"] else "FAIL: above the hear-through target"))
    if res["first_comb_notch_hz"]:
        rig.say(f"  first comb notch would be at {res['first_comb_notch_hz']:.0f} Hz")
    return res


def main(argv=None):
    p = common_parser(__doc__.splitlines()[0])
    p.add_argument("--repeats", type=int, default=5)
    args = p.parse_args(argv)
    if maybe_list_devices(args):
        return 0
    with Rig(args) as rig:
        res = run(rig, args.repeats)
    res["provenance"] = provenance(args, rig)
    write_json(args, "latency.json", res)
    return 0 if res["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
