"""Glue shared by the procedure scripts: common CLI flags, the Rig object
that pairs a HavenSession (BLE) with an audio backend, and result writing.

`--dry-run` and `--simulate` are independent: --dry-run means "no radio"
(payloads logged), --simulate means "no microphone/speaker" (Simulator
stands in). A full no-hardware rehearsal uses both.
"""
import argparse
import datetime as _dt
import json
import os

from audio_io import Simulator, make_io
from constants import LDL_TEST_FREQUENCIES_HZ
from haven_ble import HavenSession, hold
from measure import load_mic_cal


def common_parser(description):
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--dry-run", action="store_true", help="log BLE payloads instead of sending them (no bleak needed)")
    p.add_argument("--simulate", action="store_true", help="replace the microphone/speaker with a synthetic chain")
    p.add_argument("--device", default="Haven", help='BLE advertised name (default "Haven")')
    p.add_argument("--timeout", type=float, default=10.0, help="BLE scan timeout, seconds")
    p.add_argument("--fs", type=int, default=48000, help="audio sample rate (default 48000)")
    p.add_argument("--input-device", default=None, help="sounddevice input name/index (see --list-devices)")
    p.add_argument("--output-device", default=None, help="sounddevice output name/index")
    p.add_argument("--list-devices", action="store_true", help="print audio devices and exit")
    p.add_argument("--mic-cal", default=None, help='JSON: {"dbfs_at_ref": -20.0, "ref_dbspl": 94.0, "fixture": "...", "date": "..."}')
    p.add_argument("--seconds", type=float, default=1.0, help="record length per step, seconds")
    p.add_argument("--settle", type=float, default=0.3, help="wait after each command before recording, seconds")
    p.add_argument("--out", default="results", help="output directory")
    p.add_argument("--fixture", default="", help="free text: fixture / coupler / eartip / seal notes for provenance")
    p.add_argument("--no-prompt", action="store_true", help="never wait for Enter (for scripted runs)")
    p.add_argument("--quiet", action="store_true", help="suppress per-step prints (tests use this)")
    return p


class Rig:
    """One BLE session + one audio backend. Forwards tone/band commands to
    the Simulator when that is the backend so simulated recordings react."""

    def __init__(self, args):
        self.args = args
        self.io = make_io(args)
        self.sim = self.io if isinstance(self.io, Simulator) else None
        self.quiet = getattr(args, "quiet", False)
        self.session = HavenSession(dry_run=args.dry_run, device_name=args.device,
                                    timeout_s=args.timeout, verbose=not self.quiet)
        self.mic_cal = load_mic_cal(args.mic_cal)
        if self.mic_cal is not None and "fixture" in self.mic_cal and not args.fixture:
            args.fixture = self.mic_cal["fixture"]

    def __enter__(self):
        self.session.__enter__()
        return self

    def __exit__(self, *exc):
        self.session.__exit__(*exc)

    # commands (mirrored into the simulator)
    def tone_start(self, f0, level):
        p = self.session.tone_start(f0, level)
        if self.sim:
            self.sim.set_tone(p["f0"], p["level_db"])
        hold(self.args.settle)
        return p

    def tone_level(self, level):
        p = self.session.tone_level(level)
        if self.sim:
            self.sim.set_tone(self.sim.tone[0] if self.sim.tone else 1000.0, p["level_db"])
        hold(self.args.settle)
        return p

    def tone_stop(self):
        self.session.tone_stop()
        if self.sim:
            self.sim.stop_tone()
        hold(self.args.settle)

    def drop_keepalive(self):
        self.session.drop_keepalive()

    def multi_filter(self, bands):
        p = self.session.multi_filter(bands)
        if self.sim:
            self.sim.bands = [(b["f0"], b["Q"], b.get("atten_db", 40.0)) for b in p["bands"]]
            self.sim.bypass = False
        hold(self.args.settle)
        return p

    def bypass(self, enabled):
        p = self.session.bypass(enabled)
        if self.sim:
            self.sim.bypass = bool(enabled)
        hold(self.args.settle)
        return p

    # audio
    def record(self, seconds=None):
        return self.io.record(self.args.seconds if seconds is None else seconds)

    def play_and_record(self, x):
        return self.io.play_and_record(x)

    # human in the loop
    def say(self, text):
        if not self.quiet:
            print(text)

    def prompt(self, text):
        if self.args.no_prompt or self.args.simulate:
            self.say(f"[auto] {text}")
            return
        input(f"{text}\nPress Enter to continue... ")


def provenance(args, rig, extra=None):
    d = {
        "date": _dt.datetime.now().isoformat(timespec="seconds"),
        "fixture": args.fixture or None,
        "mic_cal": rig.mic_cal,
        "dry_run": bool(args.dry_run),
        "simulated_audio": bool(args.simulate),
        "fs_hz": args.fs,
        "ldl_test_frequencies_hz": LDL_TEST_FREQUENCIES_HZ,
        "caveat": ("SIMULATED -- not a measurement" if args.simulate else
                   "measurement is only meaningful with a calibrated fixture; see README"),
    }
    if extra:
        d.update(extra)
    return d


def _jsonable(o):
    """numpy scalars/arrays -> plain Python so results always serialise."""
    import numpy as np

    if isinstance(o, (np.bool_,)):
        return bool(o)
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.floating,)):
        return float(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    raise TypeError(f"not JSON serialisable: {type(o).__name__}")


def write_json(args, name, data):
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, name)
    with open(path, "w") as f:
        json.dump(data, f, indent=2, default=_jsonable)
    print(f"wrote {path}")
    return path


def maybe_list_devices(args):
    if args.list_devices:
        from audio_io import SoundDeviceIO

        print(SoundDeviceIO.list_devices())
        return True
    return False
