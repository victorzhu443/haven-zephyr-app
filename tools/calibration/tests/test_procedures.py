"""End-to-end runs of every procedure against the Simulator in dry-run:
proves the orchestration, the result schema and the pass/fail logic, NOT
any acoustic fact (the Simulator assumes the firmware's nominal mapping)."""
import argparse
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ceiling_check  # noqa: E402
import firmware_constants  # noqa: E402
import hear_through  # noqa: E402
import latency  # noqa: E402
import tone_level_map  # noqa: E402
from constants import LDL_TEST_FREQUENCIES_HZ, TONE_LEVEL_MAX_DB, TONE_WATCHDOG_MS  # noqa: E402
from rig import Rig, common_parser  # noqa: E402


def sim_args(**over):
    p = common_parser("test")
    args = p.parse_args(["--dry-run", "--simulate", "--settle", "0", "--seconds", "0.4", "--quiet"])
    for k, v in over.items():
        setattr(args, k, v)
    return args


class ToneLevelMapTests(unittest.TestCase):
    def test_simulated_map_has_unity_slopes_and_refuses_constants(self):
        args = sim_args()
        with Rig(args) as rig:
            res = tone_level_map.run_map(rig, [1000, 4000], [30, 45, 60, 75, 85])
        for f0 in ("1000", "4000"):
            fit = res["per_frequency"][f0]["fit"]
            self.assertTrue(fit["slope_ok"], fit)
            self.assertAlmostEqual(fit["slope"], 1.0, delta=0.02)
            self.assertEqual(fit["unit"], "dBFS")
            self.assertAlmostEqual(res["per_frequency"][f0]["at_ceiling"]["predicted"], 0.0, delta=0.3)
        self.assertEqual(len(res["per_frequency"]["1000"]["steps"]), 5)
        # Every command that left the "radio" was clamped and well-formed.
        for d in rig.session.sent:
            msg = json.loads(d)
            if "level_db" in msg:
                self.assertLessEqual(msg["level_db"], TONE_LEVEL_MAX_DB)
        res["provenance"] = {"simulated_audio": True}
        with self.assertRaises(SystemExit):
            firmware_constants.derive(res)

    def test_calibrated_map_reports_spl(self):
        args = sim_args(mic_cal=None)
        with Rig(args) as rig:
            rig.mic_cal = {"dbfs_at_ref": -10.0, "ref_dbspl": 94.0}
            res = tone_level_map.run_map(rig, [2000], [60, 70, 80])
        fit = res["per_frequency"]["2000"]["fit"]
        self.assertEqual(fit["unit"], "dB SPL")
        # Simulator: 85 -> 0 dBFS -> with this mic cal 0 dBFS = 104 dB SPL.
        self.assertAlmostEqual(res["per_frequency"]["2000"]["at_ceiling"]["predicted"], 104.0, delta=0.5)


class CeilingTests(unittest.TestCase):
    def test_overrange_is_clamped_and_held(self):
        args = sim_args()
        with Rig(args) as rig:
            rows = ceiling_check.ceiling_pass(rig, "best", [1000, 8000])
        self.assertTrue(all(r["overrange_held"] for r in rows.values()))
        sent = [json.loads(d) for d in rig.session.sent]
        self.assertTrue(all(m.get("level_db", 0) <= TONE_LEVEL_MAX_DB for m in sent))

    def test_raw_overrange_sends_literal_120(self):
        args = sim_args()
        with Rig(args) as rig:
            rows = ceiling_check.ceiling_pass(rig, "best", [1000], raw_overrange=True)
        sent = [json.loads(d) for d in rig.session.sent]
        self.assertTrue(any(m.get("level_db") == 120.0 for m in sent))
        self.assertTrue(rows["1000"]["overrange_held"])  # the simulated firmware clamps

    def test_disconnect_test_times_watchdog(self):
        args = sim_args()
        with Rig(args) as rig:
            d = ceiling_check.disconnect_test(rig, poll_s=0.1, max_wait_s=6.0)
        self.assertIsNotNone(d["silence_after_s"])
        self.assertLessEqual(d["silence_after_s"], TONE_WATCHDOG_MS / 1000 + 0.6)
        self.assertTrue(d["passed"])


class HearThroughTests(unittest.TestCase):
    def test_all_bands_pass_on_the_simulator(self):
        args = sim_args()
        with Rig(args) as rig:
            res = hear_through.run(rig, seconds=6.0)
        self.assertAlmostEqual(res["insertion_gain"]["mean_db"], 0.0, delta=1.0)
        for row in res["bands"]:
            self.assertTrue(row["centre_ok"], row)
            self.assertTrue(row["depth_ok"], row)
        self.assertEqual(len(res["multi_band"]["per_band"]), len(res["bands"]))
        for single, multi in zip(res["bands"], res["multi_band"]["per_band"]):
            self.assertAlmostEqual(single["measured"]["depth_db"], multi["depth_db"], delta=3.0)


class LatencyTests(unittest.TestCase):
    def test_simulated_device_latency_recovered(self):
        args = sim_args()
        with Rig(args) as rig:
            rig.sim.latency_s = 250e-6
            res = latency.run(rig, repeats=3)
        self.assertAlmostEqual(res["device_latency_s"], 250e-6, delta=15e-6)
        self.assertTrue(res["passed"])
        self.assertAlmostEqual(res["first_comb_notch_hz"], 2000.0, delta=150)

    def test_slow_device_fails(self):
        args = sim_args()
        with Rig(args) as rig:
            rig.sim.latency_s = 3e-3
            res = latency.run(rig, repeats=3)
        self.assertFalse(res["passed"])


class FirmwareConstantsTests(unittest.TestCase):
    def _map(self, spl_at_85_by_f):
        per = {}
        for f0, spl85 in spl_at_85_by_f.items():
            a, b = 1.0, spl85 - 85.0
            per[str(f0)] = {"fit": {"slope": a, "offset": b, "unit": "dB SPL", "slope_ok": True}}
        return {"per_frequency": per, "provenance": {"date": "2026-09-14", "fixture": "test coupler",
                                                     "mic_cal": {"dbfs_at_ref": -20}}}

    def test_full_scale_shifts_by_worst_frequency(self):
        d = firmware_constants.derive(self._map({1000: 88.0, 4000: 92.0, 8000: 80.0}), margin_db=2.0)
        # Worst: 4000 Hz reads 92 at a commanded 85; want 83 -> shift 9 dB.
        self.assertAlmostEqual(d["worst_shift_db"], 9.0)
        self.assertAlmostEqual(d["full_scale_db_new"], 94.0)
        self.assertAlmostEqual(d["per_frequency"][4000]["per_freq_offset_db"], 0.0)
        self.assertAlmostEqual(d["per_frequency"][8000]["per_freq_offset_db"], 12.0)
        hdr = firmware_constants.render_header(d, {"date": "x", "fixture": "y", "mic_cal": {}})
        self.assertIn("CONFIG_HAVEN_TONE_FULL_SCALE_DB=94", hdr)
        self.assertNotIn("#error", hdr)

    def test_bad_slope_poisons_header(self):
        m = self._map({1000: 85.0})
        m["per_frequency"]["1000"]["fit"]["slope_ok"] = False
        d = firmware_constants.derive(m)
        self.assertIn("#error", firmware_constants.render_header(d, {}))

    def test_refuses_dbfs(self):
        m = self._map({1000: 85.0})
        m["per_frequency"]["1000"]["fit"]["unit"] = "dBFS"
        with self.assertRaises(SystemExit):
            firmware_constants.derive(m)

    def test_cli_end_to_end(self):
        m = self._map({1000: 85.0, 2000: 86.0})
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(m, f)
        try:
            import io
            from contextlib import redirect_stdout
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = firmware_constants.main([f.name, "--margin-db", "1"])
            self.assertEqual(rc, 0)
            self.assertIn("haven_tone_cal[]", buf.getvalue())
        finally:
            os.unlink(f.name)


if __name__ == "__main__":
    unittest.main()
