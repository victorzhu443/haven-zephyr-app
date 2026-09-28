"""
Empirical check: does RNNoise preserve the kind of narrowband tonal signal
`analyze.py` is trying to find (a machine whine, an alarm, feedback), or does
it suppress that right along with the noise?

This is not a unit test with a pass/fail assertion -- it's a reproducible
experiment, run and its real output recorded in ML_RL_FEASIBILITY.md
(haven-zephyr-app root). Run it yourself to reproduce:

    pip install pyrnnoise   # NOT a project dependency -- see note below
    python3 ml/experiments/rnnoise_feasibility_check.py

Why this exists: RNNoise is a *speech* noise suppressor -- it's trained to
recognize speech's spectral/temporal shape (formants, pitch pulses, onsets)
and pass that through while suppressing everything else. `analyze.py`'s job
is the opposite kind of signal: a narrowband tone or an alarm, which is
exactly the kind of stationary, non-speech content a speech denoiser is
built to remove. Before recommending "denoise the recording before running
analyze.py on it" (an earlier pass in ML_RL_FEASIBILITY.md did exactly
this), it's worth checking whether RNNoise would strip out the very thing
being searched for.

Caveat, stated plainly: these are synthetic proxies (pure tones, a crude
AM'd-harmonics stand-in for speech, a frequency-modulated "warble" as an
alarm proxy), not real recorded speech or real recorded alarms. RNNoise's
actual model was trained on real speech statistics that these signals only
loosely approximate, so a real recording could behave differently -- this
result is a real, reproducible red flag from direct testing, not a
mathematical proof that RNNoise can never be used here.

pyrnnoise's own `denoise_wav()` convenience method also has a real bug in
the installed version (0.4.5): it calls `reader.rate`, but the `Reader`
object's actual attribute is `sample_rate` -- raises `AttributeError` every
time. Worked around here by calling the lower-level (and correctly
implemented) `denoise_chunk()` directly, which is what this script does.
"""
import numpy as np
from scipy import signal

try:
    from pyrnnoise import RNNoise
except ImportError:
    raise SystemExit(
        "pyrnnoise isn't installed -- this is an experiment, not a project "
        "dependency. Run: pip install pyrnnoise"
    )

SAMPLE_RATE = 48000  # RNNoise's native rate


def denoise(samples_float: np.ndarray) -> np.ndarray:
    """Run RNNoise over a full float32 [-1, 1] mono clip, bypassing the
    broken denoise_wav() wrapper (see module docstring)."""
    int16 = (np.clip(samples_float, -1, 1) * 32767).astype(np.int16)
    denoiser = RNNoise(sample_rate=SAMPLE_RATE)
    frames = [frame for _, frame in denoiser.denoise_chunk(int16, partial=True)]
    if not frames:
        return samples_float
    out = np.concatenate(frames, axis=1)[0]
    return out.astype(np.float64) / 32767.0


def report(label: str, signal_in: np.ndarray) -> None:
    out = denoise(signal_in)
    n = min(len(signal_in), len(out))
    rms_in = float(np.sqrt(np.mean(signal_in[:n] ** 2)))
    rms_out = float(np.sqrt(np.mean(out[:n] ** 2)))
    freqs, psd_in = signal.welch(signal_in[:n].astype(np.float64), fs=SAMPLE_RATE, nperseg=4096)
    _, psd_out = signal.welch(out[:n], fs=SAMPLE_RATE, nperseg=4096)
    kept_pct = 100.0 * rms_out / rms_in if rms_in > 0 else float("nan")
    print(
        f"{label}\n"
        f"  RMS in={rms_in:.4f}  out={rms_out:.4f}  ({kept_pct:.1f}% of input energy survived)\n"
        f"  peak freq in={freqs[np.argmax(psd_in)]:.0f} Hz  out={freqs[np.argmax(psd_out)]:.0f} Hz\n"
    )


def main() -> None:
    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE  # 1 second
    rng = np.random.default_rng(0)

    loud_tone = 0.5 * np.sin(2 * np.pi * 1000 * t) + 0.01 * rng.standard_normal(SAMPLE_RATE)
    report("Loud pure 1 kHz tone, favorable SNR (best case for the tone to survive)", loud_tone)

    speechlike = (
        0.2 * np.sin(2 * np.pi * 150 * t)
        + 0.15 * np.sin(2 * np.pi * 600 * t)
        + 0.1 * np.sin(2 * np.pi * 1800 * t)
    ) * (0.5 + 0.5 * np.sin(2 * np.pi * 3 * t))
    report("Crude harmonic+AM speech-like proxy + noise", speechlike + 0.08 * rng.standard_normal(SAMPLE_RATE))

    warble = 0.3 * np.sin(2 * np.pi * (1000 + 300 * np.sin(2 * np.pi * 4 * t)) * t)
    report("Frequency-modulated 'warbling alarm' proxy + noise", warble + 0.05 * rng.standard_normal(SAMPLE_RATE))


if __name__ == "__main__":
    main()
