"""Procedure 2 (calibration.md): does the 85 dB ceiling hold?

1. Command the ceiling at every LDL frequency, best and worst seal, record
   the level (dB SPL with --mic-cal).
2. Command an out-of-range level (120) and confirm the measured level does
   not exceed the 85-level reading: both the rig's own clamp and the
   firmware's PROTOCOL_TONE_LEVEL_MAX_DB must hold. (The rig clamps before
   sending -- exactly like the app -- so this exercises the same
   defence-in-depth pair the product has: the payload that leaves says 85.)
   Use --raw-overrange to bypass the rig clamp and send a literal 120 so the
   FIRMWARE clamp is the one under test.
3. --disconnect-test: start a tone, stop refreshing it without sending
   TONE_STOP, and time how long the fixture keeps hearing it. Expected: the
   firmware watchdog silences within TONE_WATCHDOG_MS (3 s).

    python3 ceiling_check.py --dry-run --simulate --disconnect-test
"""
import json
import sys
import time

from constants import LDL_TEST_FREQUENCIES_HZ, TONE_LEVEL_MAX_DB, TONE_WATCHDOG_MS
from haven_ble import encode_line
from measure import dbfs_to_dbspl, tone_level_dbfs
from rig import Rig, common_parser, maybe_list_devices, provenance, write_json

OVERRANGE_DB = 120.0
SILENCE_MARGIN_DB = 20.0  # "silent" = tone level fell this far below its on-level


def measure_tone(rig, f0):
    x = rig.record()
    dbfs, _ = tone_level_dbfs(x, rig.args.fs, f0)
    return dbfs, dbfs_to_dbspl(dbfs, rig.mic_cal)


def ceiling_pass(rig, seal, frequencies, raw_overrange=False):
    rows = {}
    for f0 in frequencies:
        rig.tone_start(f0, TONE_LEVEL_MAX_DB)
        at_cap_dbfs, at_cap_spl = measure_tone(rig, f0)
        if raw_overrange:
            # Deliberately bypass the rig clamp to test the firmware's.
            rig.session._send({"type": "TONE_LEVEL", "level_db": OVERRANGE_DB})
            if rig.sim:
                rig.sim.set_tone(f0, TONE_LEVEL_MAX_DB)  # the simulated board clamps too
        else:
            rig.tone_level(OVERRANGE_DB)  # rig clamps to 85 -> same payload as the app would send
        over_dbfs, over_spl = measure_tone(rig, f0)
        rig.tone_stop()
        rows[str(f0)] = {
            "seal": seal,
            "at_ceiling_dbfs": at_cap_dbfs, "at_ceiling_dbspl": at_cap_spl,
            "after_overrange_dbfs": over_dbfs, "after_overrange_dbspl": over_spl,
            "overrange_held": over_dbfs <= at_cap_dbfs + 0.5,
        }
        unit = "dB SPL" if at_cap_spl is not None else "dBFS"
        a = at_cap_spl if at_cap_spl is not None else at_cap_dbfs
        o = over_spl if over_spl is not None else over_dbfs
        rig.say(f"  {f0:5d} Hz [{seal}]  at 85 -> {a:6.1f} {unit};  after {OVERRANGE_DB:.0f} -> {o:6.1f} {unit}  "
              + ("OK" if rows[str(f0)]["overrange_held"] else "FAIL: level rose past the ceiling reading"))
    return rows


def disconnect_test(rig, f0=4000, level=60.0, poll_s=0.25, max_wait_s=6.0):
    """Start a tone, drop the keep-alive, poll the fixture until the tone is
    gone. Returns seconds to silence (None if never)."""
    rig.tone_start(f0, level)
    on_dbfs, _ = measure_tone(rig, f0)
    t0 = time.monotonic()
    rig.drop_keepalive()
    if rig.sim:
        # The simulated firmware silences at the watchdog timeout.
        deadline = t0 + TONE_WATCHDOG_MS / 1000.0
    silence_at = None
    while time.monotonic() - t0 < max_wait_s:
        if rig.sim and time.monotonic() >= deadline:
            rig.sim.stop_tone()
        x = rig.io.record(min(poll_s, 0.25))
        dbfs, _ = tone_level_dbfs(x, rig.args.fs, f0)
        if dbfs < on_dbfs - SILENCE_MARGIN_DB:
            silence_at = time.monotonic() - t0
            break
        time.sleep(poll_s)
    rig.session._level_db = None  # nothing to stop on the wire; the board did it (or should have)
    return {"tone_hz": f0, "level_db": level, "silence_after_s": silence_at,
            "watchdog_ms": TONE_WATCHDOG_MS,
            "passed": silence_at is not None and silence_at <= TONE_WATCHDOG_MS / 1000.0 + 0.5}


def main(argv=None):
    p = common_parser(__doc__.splitlines()[0])
    p.add_argument("--frequencies", type=int, nargs="*", default=LDL_TEST_FREQUENCIES_HZ)
    p.add_argument("--seals", nargs="*", default=["best", "worst"], help='seal conditions to prompt for')
    p.add_argument("--raw-overrange", action="store_true", help="send a literal 120 (bypass the rig clamp) to test the firmware clamp")
    p.add_argument("--disconnect-test", action="store_true", help="drop the keep-alive mid-tone and time the firmware watchdog")
    args = p.parse_args(argv)
    if maybe_list_devices(args):
        return 0
    results = {"ceiling": {}, "disconnect": None}
    with Rig(args) as rig:
        for seal in args.seals:
            rig.prompt(f"Seat the earpiece for the '{seal}' seal condition. The tone will play at the 85 dB ceiling.")
            results["ceiling"][seal] = ceiling_pass(rig, seal, args.frequencies, args.raw_overrange)
        if args.disconnect_test:
            rig.prompt("Disconnect test: a 60 dB tone starts, then the rig stops talking to the board.")
            results["disconnect"] = disconnect_test(rig)
            d = results["disconnect"]
            print(f"  silence after {d['silence_after_s']} s (watchdog {TONE_WATCHDOG_MS} ms) -> "
                  + ("OK" if d["passed"] else "FAIL"))
    results["provenance"] = provenance(args, rig, {"overrange_commanded_db": OVERRANGE_DB,
                                                    "raw_overrange": args.raw_overrange,
                                                    "rig_clamped_payload_example": encode_line(
                                                        {"type": "TONE_LEVEL", "level_db": TONE_LEVEL_MAX_DB}).decode().rstrip()})
    all_ok = all(r["overrange_held"] for s in results["ceiling"].values() for r in s.values())
    if results["disconnect"] is not None:
        all_ok = all_ok and results["disconnect"]["passed"]
    results["all_passed"] = all_ok
    write_json(args, "ceiling_check.json", results)
    print("\nRESULT:", "PASS" if all_ok else "FAIL")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
