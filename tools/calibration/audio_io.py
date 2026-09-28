"""Audio I/O for the rig: a thin wrapper over `sounddevice` plus a Simulator
that stands in for the whole acoustic chain when there is no fixture.

The Simulator is deliberately simple and documented as such: commanded
level_db maps to a sine at 10^((level_db - 85)/20) full scale (i.e. it
*assumes* the firmware's nominal mapping is right), with -60 dBFS white noise.
It exists so every procedure can be run end to end without hardware and so
the tests can exercise the same code paths the real runs use. A Simulator
result is never a measurement.
"""
import numpy as np

from constants import TONE_LEVEL_MAX_DB
from measure import delayed, pink_noise, sine


class SoundDeviceIO:
    def __init__(self, fs=48000, input_device=None, output_device=None):
        import sounddevice as sd  # deferred so --simulate needs no audio backend

        self.sd = sd
        self.fs = fs
        self.input_device = input_device
        self.output_device = output_device

    @staticmethod
    def list_devices():
        import sounddevice as sd

        return str(sd.query_devices())

    def record(self, seconds):
        x = self.sd.rec(int(seconds * self.fs), samplerate=self.fs, channels=1,
                        dtype="float32", device=self.input_device)
        self.sd.wait()
        return x[:, 0].astype(float)

    def play(self, x):
        self.sd.play(np.asarray(x, dtype="float32"), samplerate=self.fs, device=self.output_device)
        self.sd.wait()

    def play_and_record(self, x):
        y = self.sd.playrec(np.asarray(x, dtype="float32"), samplerate=self.fs, channels=1,
                            device=(self.input_device, self.output_device))
        self.sd.wait()
        return y[:, 0].astype(float)


class Simulator:
    """Fake acoustic chain. State set by the procedures: current tone (f0,
    level), active bands, bypass, seal condition, device latency."""

    def __init__(self, fs=48000, noise_dbfs=-60.0, full_scale_level_db=TONE_LEVEL_MAX_DB,
                 latency_s=60e-6, seal_loss_db=0.0, seed=0):
        self.fs = fs
        self.noise_dbfs = noise_dbfs
        self.full_scale_level_db = full_scale_level_db
        self.latency_s = latency_s
        self.seal_loss_db = seal_loss_db
        self.rng = np.random.default_rng(seed)
        self.tone = None  # (f0_hz, level_db) or None
        self.bands = []  # [(f0, Q, atten_db)]
        self.bypass = False
        self.in_ear = True

    # what the "board" is doing
    def set_tone(self, f0_hz, level_db):
        self.tone = (f0_hz, level_db)

    def stop_tone(self):
        self.tone = None

    def _noise(self, n):
        return self.rng.standard_normal(n) * 10 ** (self.noise_dbfs / 20) / np.sqrt(2)

    def record(self, seconds):
        n = int(seconds * self.fs)
        x = self._noise(n)
        if self.tone is not None:
            f0, level = self.tone
            amp = 10 ** ((level - self.full_scale_level_db) / 20) * 10 ** (-self.seal_loss_db / 20)
            x = x + sine(self.fs, seconds, f0, amp)[:n]
        return x

    def _apply_bands(self, x):
        from scipy import signal

        y = np.asarray(x, dtype=float)
        if self.bypass:
            return y
        for f0, q, atten in self.bands:
            w0 = 2 * np.pi * f0 / self.fs
            alpha = np.sin(w0) / (2 * q)
            if atten >= 40.0:
                b = np.array([1, -2 * np.cos(w0), 1])
                a = np.array([1 + alpha, -2 * np.cos(w0), 1 - alpha])
            else:
                A = 10 ** (-atten / 40)
                b = np.array([1 + alpha * A, -2 * np.cos(w0), 1 - alpha * A])
                a = np.array([1 + alpha / A, -2 * np.cos(w0), 1 - alpha / A])
            y = signal.lfilter(b / a[0], a / a[0], y)
        return y

    def play_and_record(self, x):
        """Environment sound `x` reaches the mic either open-ear (as is) or
        through the device (filtered, delayed, seal-attenuated)."""
        x = np.asarray(x, dtype=float)
        if not self.in_ear:
            return x + self._noise(len(x))
        y = self._apply_bands(x)
        y = delayed(y, self.fs, self.latency_s, gain=10 ** (-self.seal_loss_db / 20))
        return y + self._noise(len(x))

    def play(self, x):
        pass


def make_io(args):
    """Choose the I/O backend from parsed CLI args (expects .simulate, .fs,
    .input_device, .output_device)."""
    if getattr(args, "simulate", False):
        return Simulator(fs=args.fs)
    return SoundDeviceIO(fs=args.fs, input_device=args.input_device, output_device=args.output_device)


def stimulus_pink(fs, seconds):
    return pink_noise(fs, seconds)
