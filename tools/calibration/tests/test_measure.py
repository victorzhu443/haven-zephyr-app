import math
import os
import sys
import unittest

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from measure import (  # noqa: E402
    a_weighted_level_dbfs,
    broadband_level_dbfs,
    click,
    dbfs_to_dbspl,
    delay_seconds,
    delayed,
    fit_line,
    insertion_gain_db,
    notch_metrics,
    nperseg_for,
    pink_noise,
    sine,
    slope_is_unity,
    tone_level_dbfs,
    transfer_db,
)

FS = 48000


class ToneLevelTests(unittest.TestCase):
    def test_full_scale_sine_is_0_dbfs_at_every_ldl_frequency(self):
        for f0 in (1000, 2000, 3000, 4000, 6000, 8000):
            x = sine(FS, 1.0, f0, 1.0)
            lvl, hz = tone_level_dbfs(x, FS, f0)
            self.assertAlmostEqual(lvl, 0.0, delta=0.05, msg=f0)
            self.assertAlmostEqual(hz, f0, delta=FS / len(x) * 1.5)

    def test_minus_20_dbfs_with_noise(self):
        rng = np.random.default_rng(1)
        x = sine(FS, 1.0, 4000, 0.1) + rng.standard_normal(FS) * 1e-4
        lvl, _ = tone_level_dbfs(x, FS, 4000)
        self.assertAlmostEqual(lvl, -20.0, delta=0.1)

    def test_off_frequency_tone_not_mistaken(self):
        # Energy at 1 kHz should not register as a 4 kHz tone.
        x = sine(FS, 1.0, 1000, 1.0)
        lvl, _ = tone_level_dbfs(x, FS, 4000)
        self.assertLess(lvl, -60.0)

    def test_broadband_level_of_full_scale_sine(self):
        self.assertAlmostEqual(broadband_level_dbfs(sine(FS, 1.0, 1000)), 0.0, delta=0.01)


class AWeightingTests(unittest.TestCase):
    def test_reference_points(self):
        # IEC 61672 table: 1 kHz 0 dB; 100 Hz -19.1 dB; 8 kHz -1.1 dB.
        base = a_weighted_level_dbfs(sine(FS, 2.0, 1000), FS)
        self.assertAlmostEqual(base, 0.0, delta=0.3)
        self.assertAlmostEqual(a_weighted_level_dbfs(sine(FS, 2.0, 100), FS) - base, -19.1, delta=0.5)
        self.assertAlmostEqual(a_weighted_level_dbfs(sine(FS, 2.0, 8000), FS) - base, -1.1, delta=0.5)


class MicCalTests(unittest.TestCase):
    def test_uncalibrated_returns_none(self):
        self.assertIsNone(dbfs_to_dbspl(-20.0, None))
        self.assertIsNone(dbfs_to_dbspl(-20.0, {"fixture": "x"}))

    def test_spl_from_reference(self):
        # Mic reads -20 dBFS in a 94 dB SPL calibrator -> -30 dBFS is 84 dB SPL.
        self.assertAlmostEqual(dbfs_to_dbspl(-30.0, {"dbfs_at_ref": -20.0, "ref_dbspl": 94.0}), 84.0)
        self.assertAlmostEqual(dbfs_to_dbspl(-30.0, {"dbfs_at_ref": -20.0}), 84.0)  # default ref 94


class FitTests(unittest.TestCase):
    def test_exact_line(self):
        a, b, rmse = fit_line([30, 40, 50, 60], [10, 20, 30, 40])
        self.assertAlmostEqual(a, 1.0)
        self.assertAlmostEqual(b, -20.0)
        self.assertAlmostEqual(rmse, 0.0)
        self.assertTrue(slope_is_unity(a))
        self.assertFalse(slope_is_unity(0.8))

    def test_needs_two_points(self):
        with self.assertRaises(ValueError):
            fit_line([1], [1])


def _rbj_notch(f0, q, fs):
    w0 = 2 * math.pi * f0 / fs
    alpha = math.sin(w0) / (2 * q)
    b = np.array([1, -2 * math.cos(w0), 1])
    a = np.array([1 + alpha, -2 * math.cos(w0), 1 - alpha])
    return b / a[0], a / a[0]


class SpectraTests(unittest.TestCase):
    def test_transfer_db_matches_freqz_for_a_known_filter(self):
        b, a = _rbj_notch(4500, 10, FS)
        x = pink_noise(FS, 8.0)
        y = signal.lfilter(b, a, x)
        f, h_db = transfer_db(x, y, FS, 8192)
        w, h = signal.freqz(b, a, worN=f, fs=FS)
        m = (f > 300) & (f < 12000) & (np.abs(f - 4500) > 300)  # away from the notch bottom
        self.assertLess(np.max(np.abs(h_db[m] - 20 * np.log10(np.abs(h[m]) + 1e-12))), 0.5)

    def test_nperseg_resolves_narrow_low_notch(self):
        n = nperseg_for(FS, 200, 20)
        self.assertGreaterEqual(n, 8 * FS / 10)  # >= 8 bins across a 10 Hz notch
        self.assertEqual(n & (n - 1), 0)  # power of two
        self.assertLess(nperseg_for(FS, 8000, 1), n)

    def test_notch_metrics_on_full_notch(self):
        b, a = _rbj_notch(200, 20, FS)
        x = pink_noise(FS, 8.0)
        y = signal.lfilter(b, a, x)
        f, g = transfer_db(x, y, FS, nperseg_for(FS, 200, 20))
        m = notch_metrics(f, g, 200)
        self.assertAlmostEqual(m["centre_hz"], 200, delta=2.0)
        self.assertGreater(m["depth_db"], 30.0)
        self.assertAlmostEqual(m["q"], 20, delta=6)

    def test_notch_metrics_on_peaking_cut(self):
        # 20 dB peaking cut at 4500 Hz, Q 10 (same math as adau1860_control.c).
        f0, q, atten = 4500, 10, 20
        w0 = 2 * math.pi * f0 / FS
        alpha = math.sin(w0) / (2 * q)
        A = 10 ** (-atten / 40)
        b = np.array([1 + alpha * A, -2 * math.cos(w0), 1 - alpha * A])
        a = np.array([1 + alpha / A, -2 * math.cos(w0), 1 - alpha / A])
        x = pink_noise(FS, 8.0)
        y = signal.lfilter(b / a[0], a / a[0], x)
        f, g = transfer_db(x, y, FS, nperseg_for(FS, f0, q))
        m = notch_metrics(f, g, f0)
        self.assertAlmostEqual(m["depth_db"], atten, delta=1.0)
        self.assertLess(abs(m["centre_error_pct"]), 2.0)

    def test_insertion_gain_zero_for_identical_paths(self):
        x = pink_noise(FS, 4.0)
        f, ig = insertion_gain_db(x, x, FS)
        self.assertTrue(np.allclose(ig, 0.0, atol=1e-6))
        self.assertGreaterEqual(f.min(), 200.0)
        self.assertLessEqual(f.max(), 8000.0)


class DelayTests(unittest.TestCase):
    def test_integer_sample_delay(self):
        x = click(FS, 0.5)
        y = delayed(x, FS, 37 / FS)
        r = delay_seconds(x, y, FS)
        self.assertAlmostEqual(r["delay_s"] * FS, 37, delta=0.05)
        self.assertGreater(r["peak_corr"], 0.9)

    def test_fractional_sample_delay_with_noise(self):
        rng = np.random.default_rng(3)
        x = pink_noise(FS, 1.0)
        y = delayed(x, FS, 2.4 / FS, gain=0.5) + rng.standard_normal(FS) * 1e-3
        r = delay_seconds(x, y, FS)
        self.assertAlmostEqual(r["delay_s"] * FS, 2.4, delta=0.15)

    def test_sixty_microseconds_is_resolvable_at_48k(self):
        x = click(FS, 0.5, width_samples=3)
        y = delayed(x, FS, 60e-6)
        r = delay_seconds(x, y, FS)
        self.assertAlmostEqual(r["delay_s"], 60e-6, delta=8e-6)
        self.assertAlmostEqual(r["resolution_s"], 1 / FS)


class SynthesisTests(unittest.TestCase):
    def test_pink_noise_rms(self):
        x = pink_noise(FS, 2.0)
        self.assertAlmostEqual(float(np.sqrt(np.mean(x * x))), 0.1, delta=1e-6)


if __name__ == "__main__":
    unittest.main()
