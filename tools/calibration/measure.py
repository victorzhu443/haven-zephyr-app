"""Pure-numpy/scipy measurement math for the calibration rig. No audio I/O,
no BLE -- everything here is deterministic and unit-tested against synthetic
signals (tests/test_measure.py).

Conventions:
- dBFS of a sine = 20*log10(peak amplitude), so a full-scale sine is 0 dBFS.
  (Broadband levels use RMS re full-scale-sine RMS, i.e. rms*sqrt(2).)
- dB SPL comes only from a mic calibration: the rig never invents an
  acoustic reference.
"""
import math

import numpy as np
from scipy import signal

from constants import MIC_CAL_DEFAULT_REF_DBSPL


# ── Levels ───────────────────────────────────────────────────────────────────
def db(x, floor=1e-12):
    return 20.0 * np.log10(np.maximum(np.asarray(x, dtype=float), floor))


def tone_level_dbfs(x, fs, f0_hz, search_frac=0.02):
    """Peak-amplitude estimate (dBFS) of the sinusoid nearest f0 within
    +/- search_frac*f0, Hann window with coherent-gain correction. Returns
    (level_dbfs, measured_hz)."""
    x = np.asarray(x, dtype=float)
    x = x - x.mean()
    n = len(x)
    win = np.hanning(n)
    spec = np.fft.rfft(x * win)
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    lo, hi = f0_hz * (1 - search_frac), f0_hz * (1 + search_frac)
    band = np.where((freqs >= lo) & (freqs <= hi))[0]
    if band.size == 0:
        band = np.array([int(np.argmin(np.abs(freqs - f0_hz)))])
    k = band[np.argmax(np.abs(spec[band]))]
    # Hann coherent gain is 0.5; single-sided sine amplitude = 2|X|/(N*cg).
    amp = 2.0 * np.abs(spec[k]) / (n * 0.5)
    return float(db(amp)), float(freqs[k])


def broadband_level_dbfs(x):
    """RMS level re a full-scale sine (so a 0 dBFS sine reads 0 dBFS here too)."""
    x = np.asarray(x, dtype=float)
    return float(db(np.sqrt(np.mean(x * x)) * math.sqrt(2.0)))


def a_weighting_db(f_hz):
    """IEC 61672 A-weighting in dB, evaluated exactly from the analog
    definition (no bilinear warping). 0 dB at 1 kHz by construction."""
    f = np.asarray(f_hz, dtype=float)
    f2 = f * f
    num = (12194.0 ** 2) * f2 * f2
    den = (f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194.0 ** 2)
    with np.errstate(divide="ignore"):
        return 20.0 * np.log10(np.maximum(num / np.maximum(den, 1e-30), 1e-30)) + 2.00


def a_weighted_level_dbfs(x, fs):
    """A-weighted broadband level, re full-scale sine, computed in the
    frequency domain so the weighting is exact at every bin (a bilinear IIR
    realisation is ~0.6 dB off at 8 kHz for fs = 48 kHz)."""
    x = np.asarray(x, dtype=float)
    x = x - x.mean()
    n = len(x)
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / fs)
    w = 10.0 ** (a_weighting_db(f) / 20.0)
    w[0] = 0.0
    p = np.abs(spec * w) ** 2
    # Parseval for rfft: mean(x^2) = (p0 + 2*sum(p[1:-1]) + p[-1]) / n^2
    if n % 2 == 0:
        total = p[0] + 2.0 * p[1:-1].sum() + p[-1]
    else:
        total = p[0] + 2.0 * p[1:].sum()
    rms = math.sqrt(max(total, 0.0)) / n
    return float(db(rms * math.sqrt(2.0)))


# ── Mic calibration ──────────────────────────────────────────────────────────
def load_mic_cal(cal):
    """Accepts None, a dict, or a path to JSON with
    {"dbfs_at_ref": <level the mic reads at the calibrator>, "ref_dbspl": 94.0,
     "fixture": "...", "date": "..."}; returns the dict or None."""
    if cal is None:
        return None
    if isinstance(cal, dict):
        return cal
    import json
    with open(cal) as f:
        return json.load(f)


def dbfs_to_dbspl(level_dbfs, mic_cal):
    """SPL = dBFS - dBFS_at_ref + ref_dBSPL. Returns None when uncalibrated
    (callers must then label the result dBFS, never dB SPL)."""
    if not mic_cal or "dbfs_at_ref" not in mic_cal:
        return None
    ref = float(mic_cal.get("ref_dbspl", MIC_CAL_DEFAULT_REF_DBSPL))
    return float(level_dbfs) - float(mic_cal["dbfs_at_ref"]) + ref


# ── Fits ─────────────────────────────────────────────────────────────────────
def fit_line(x, y):
    """Least-squares y = a*x + b. Returns (a, b, rmse)."""
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    if x.size < 2:
        raise ValueError("need at least two points")
    a, b = np.polyfit(x, y, 1)
    rmse = float(np.sqrt(np.mean((a * x + b - y) ** 2)))
    return float(a), float(b), rmse


def slope_is_unity(a, tol=0.1):
    """The firmware promises 1 dB commanded == 1 dB out; a slope outside
    1 +/- tol means the level_db -> gain conversion is wrong in firmware and
    must be fixed there, not compensated in a table."""
    return abs(a - 1.0) <= tol


# ── Spectra ──────────────────────────────────────────────────────────────────
def spectrum_db(x, fs, nperseg=4096):
    """Welch PSD in dB (arbitrary reference; only differences are used)."""
    f, p = signal.welch(np.asarray(x, dtype=float), fs=fs, nperseg=min(nperseg, len(x)))
    return f, 10.0 * np.log10(np.maximum(p, 1e-20))


def nperseg_for(fs, f0_hz, q, cycles_per_bin=8, n_max=1 << 18):
    """Segment length giving ~cycles_per_bin FFT bins across a notch of
    bandwidth f0/q (so a Q=20 notch at 200 Hz -- 10 Hz wide -- is resolved
    by ~8 bins). Power of two, capped."""
    bw = f0_hz / q
    n = int(2 ** math.ceil(math.log2(max(256.0, cycles_per_bin * fs / bw))))
    return min(n, n_max)


def transfer_db(x_in, y_out, fs, nperseg=4096):
    """|H(f)| in dB of the path x_in -> y_out, estimated as Pyx/Pxx (Welch
    cross-spectrum over auto-spectrum). Because the stimulus is known this
    has far lower variance than a ratio of two noise PSDs. Returns (f, dB)."""
    x = np.asarray(x_in, dtype=float)
    y = np.asarray(y_out, dtype=float)
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    nper = min(nperseg, n)
    f, pxx = signal.welch(x, fs=fs, nperseg=nper)
    _, pyx = signal.csd(x, y, fs=fs, nperseg=nper)
    h = np.abs(pyx) / np.maximum(pxx, 1e-30)
    return f, 20.0 * np.log10(np.maximum(h, 1e-12))


def insertion_gain_db(open_ear, in_ear, fs, band_hz=(200.0, 8000.0), nperseg=4096):
    """Device spectrum minus open-ear spectrum, restricted to band_hz.
    ~0 dB across the band = transparent hear-through. Returns (f, gain_db)."""
    f, p_open = spectrum_db(open_ear, fs, nperseg)
    _, p_dev = spectrum_db(in_ear, fs, nperseg)
    m = (f >= band_hz[0]) & (f <= band_hz[1])
    return f[m], (p_dev - p_open)[m]


def notch_metrics(f, gain_db, f0_hz, search_octaves=1.0):
    """Given a filtered-minus-unfiltered gain curve, find the notch nearest
    the commanded f0: centre (Hz), depth (positive dB), -3 dB bandwidth (Hz)
    and the implied Q. Search window is +/- search_octaves around f0."""
    f = np.asarray(f, dtype=float)
    g = np.asarray(gain_db, dtype=float)
    m = (f >= f0_hz / 2 ** search_octaves) & (f <= f0_hz * 2 ** search_octaves)
    if not np.any(m):
        raise ValueError("no spectrum points inside the search window")
    idx = np.where(m)[0]
    k = idx[np.argmin(g[idx])]
    depth = -float(g[k])
    centre = float(f[k])
    # -3 dB points relative to 0 dB (the passband), walking out from the minimum.
    lo = k
    while lo > 0 and g[lo] <= -3.0:
        lo -= 1
    hi = k
    while hi < len(g) - 1 and g[hi] <= -3.0:
        hi += 1
    bw = float(f[hi] - f[lo]) if depth >= 3.0 else float("nan")
    q = centre / bw if bw and bw > 0 and not math.isnan(bw) else float("nan")
    return {"centre_hz": centre, "depth_db": depth, "bandwidth_hz": bw, "q": q,
            "centre_error_pct": 100.0 * (centre - f0_hz) / f0_hz}


# ── Latency ──────────────────────────────────────────────────────────────────
def delay_seconds(reference, device, fs, max_delay_s=0.05):
    """Delay of `device` relative to `reference` by cross-correlation, with
    parabolic sub-sample interpolation. Positive = device lags. Also returns
    the peak normalised correlation and the raw resolution 1/fs."""
    r = np.asarray(reference, dtype=float)
    d = np.asarray(device, dtype=float)
    r = r - r.mean()
    d = d - d.mean()
    n = min(len(r), len(d))
    r, d = r[:n], d[:n]
    corr = signal.correlate(d, r, mode="full", method="fft")
    lags = signal.correlation_lags(n, n, mode="full")
    max_lag = int(max_delay_s * fs)
    m = np.abs(lags) <= max_lag
    corr_m, lags_m = corr[m], lags[m]
    k = int(np.argmax(corr_m))
    lag = float(lags_m[k])
    if 0 < k < len(corr_m) - 1:
        y0, y1, y2 = corr_m[k - 1], corr_m[k], corr_m[k + 1]
        denom = (y0 - 2 * y1 + y2)
        if denom != 0:
            lag += 0.5 * (y0 - y2) / denom
    norm = np.sqrt(np.sum(r * r) * np.sum(d * d)) or 1.0
    return {"delay_s": lag / fs, "peak_corr": float(corr_m[k] / norm), "resolution_s": 1.0 / fs}


# ── Synthesis (used by --simulate and the tests) ─────────────────────────────
def sine(fs, seconds, f0_hz, amplitude=1.0, phase=0.0):
    t = np.arange(int(round(fs * seconds))) / fs
    return amplitude * np.sin(2 * np.pi * f0_hz * t + phase)


def pink_noise(fs, seconds, rng=None):
    """1/f-shaped noise, RMS 0.1, via spectral shaping of white noise."""
    rng = rng or np.random.default_rng(0)
    n = int(round(fs * seconds))
    white = rng.standard_normal(n)
    spec = np.fft.rfft(white)
    f = np.fft.rfftfreq(n, 1.0 / fs)
    f[0] = f[1] if n > 1 else 1.0
    spec /= np.sqrt(f)
    x = np.fft.irfft(spec, n)
    return 0.1 * x / (np.sqrt(np.mean(x * x)) or 1.0)


def click(fs, seconds, at_s=0.1, width_samples=2, amplitude=0.8):
    x = np.zeros(int(round(fs * seconds)))
    i = int(at_s * fs)
    x[i:i + width_samples] = amplitude
    return x


def delayed(x, fs, delay_s, gain=1.0):
    """Shift x later by delay_s (fractional allowed, linear interpolation)."""
    n = len(x)
    t = np.arange(n) / fs
    return gain * np.interp(t - delay_s, t, x, left=0.0, right=0.0)
