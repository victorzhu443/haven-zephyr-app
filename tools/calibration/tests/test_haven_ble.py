import json
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from constants import (  # noqa: E402
    ATTEN_MAX_DB,
    F0_MAX_HZ,
    F0_MIN_HZ,
    MAX_BANDS,
    MAX_LINE_BYTES,
    Q_MAX,
    TONE_LEVEL_MAX_DB,
)
from haven_ble import (  # noqa: E402
    HavenSession,
    bypass_payload,
    clamp_band,
    clamp_level_db,
    encode_line,
    multi_filter_payload,
    tone_level_payload,
    tone_start_payload,
)


class ClampTests(unittest.TestCase):
    def test_level_clamped_to_firmware_ceiling(self):
        self.assertEqual(clamp_level_db(120), TONE_LEVEL_MAX_DB)
        self.assertEqual(clamp_level_db(-5), 0.0)
        self.assertEqual(clamp_level_db(42.5), 42.5)

    def test_band_clamps_match_protocol_h(self):
        b = clamp_band(50, 99, 100)
        self.assertEqual((b["f0"], b["Q"], b["atten_db"]), (F0_MIN_HZ, Q_MAX, ATTEN_MAX_DB))
        b = clamp_band(20000, 0.1)
        self.assertEqual(b["f0"], F0_MAX_HZ)
        self.assertEqual(b["Q"], 1.0)
        self.assertNotIn("atten_db", b)


class PayloadTests(unittest.TestCase):
    def test_tone_start_wire_format(self):
        data = encode_line(tone_start_payload(4000, 30))
        self.assertTrue(data.endswith(b"\n"))
        self.assertEqual(json.loads(data), {"type": "TONE_START", "f0": 4000.0, "level_db": 30.0})

    def test_tone_level_uses_uppercase_type_and_level_db_key(self):
        self.assertEqual(tone_level_payload(85), {"type": "TONE_LEVEL", "level_db": 85.0})

    def test_multi_filter_uses_uppercase_Q_and_truncates(self):
        p = multi_filter_payload([(1000 + i, 5) for i in range(MAX_BANDS + 3)])
        self.assertEqual(len(p["bands"]), MAX_BANDS)
        self.assertIn("Q", p["bands"][0])
        self.assertNotIn("q", p["bands"][0])

    def test_multi_filter_requires_a_band(self):
        with self.assertRaises(ValueError):
            multi_filter_payload([])

    def test_bypass(self):
        self.assertEqual(bypass_payload(1), {"type": "BYPASS", "enabled": True})

    def test_oversized_line_refused(self):
        huge = {"type": "MULTI_FILTER", "bands": [{"f0": 1000.0, "Q": 1.0}] * 60}
        with self.assertRaises(ValueError):
            encode_line(huge)
        self.assertLess(len(encode_line(multi_filter_payload([(1000, 1)] * 5))), MAX_LINE_BYTES)


def _types(session):
    return [json.loads(d)["type"] for d in session.sent]


class DryRunSessionTests(unittest.TestCase):
    def test_start_level_stop_sequence(self):
        with HavenSession(dry_run=True, verbose=False) as s:
            s.tone_start(1000, 30, keepalive=False)
            s.tone_level(35)
            s.tone_stop()
            self.assertEqual(_types(s), ["TONE_START", "TONE_LEVEL", "TONE_STOP"])

    def test_exit_sends_tone_stop_for_a_live_tone(self):
        with HavenSession(dry_run=True, verbose=False) as s:
            s.tone_start(1000, 30, keepalive=False)
        self.assertEqual(_types(s)[-1], "TONE_STOP")

    def test_exit_sends_nothing_extra_when_idle(self):
        with HavenSession(dry_run=True, verbose=False) as s:
            s.bypass(True)
        self.assertEqual(_types(s), ["BYPASS"])

    def test_keepalive_refreshes_level_inside_watchdog(self):
        with HavenSession(dry_run=True, verbose=False, keepalive_interval_s=0.05) as s:
            s.tone_start(2000, 40)
            time.sleep(0.3)
            s.tone_stop()
            types = _types(s)
        self.assertGreaterEqual(types.count("TONE_LEVEL"), 3)
        self.assertEqual(types[-1], "TONE_STOP")
        levels = {json.loads(d)["level_db"] for d in s.sent if json.loads(d)["type"] == "TONE_LEVEL"}
        self.assertEqual(levels, {40.0})

    def test_drop_keepalive_sends_no_stop(self):
        with HavenSession(dry_run=True, verbose=False, keepalive_interval_s=0.05) as s:
            s.tone_start(2000, 40)
            time.sleep(0.12)
            s.drop_keepalive()
            n = len(s.sent)
            time.sleep(0.15)
            self.assertEqual(len(s.sent), n)  # silence: the firmware watchdog must act
        self.assertNotIn("TONE_STOP", _types(s))

    def test_dry_run_never_imports_bleak(self):
        sys.modules.pop("bleak", None)
        with HavenSession(dry_run=True, verbose=False):
            pass
        self.assertNotIn("bleak", sys.modules)


if __name__ == "__main__":
    unittest.main()
