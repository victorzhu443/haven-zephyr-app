"""Every constant the calibration rig relies on, in one place.

Everything here is NOMINAL: it is what the firmware and app *command*, not a
measured sound pressure. The whole point of this directory is to produce the
measurements that give these numbers physical meaning (see README.md and
haven-app docs/calibration.md). Values mirror the firmware/app sources named
in the comments -- if those change, change these with them.
"""

# ── BLE transport (src/ble_transport.c, prj.conf) ────────────────────────────
DEVICE_NAME = "Haven"  # CONFIG_BT_DEVICE_NAME; the app scans by exact name too
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # phone -> device (write)
NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device -> phone (notify)
LINE_TERMINATOR = "\n"  # firmware assembles NUS writes until '\n'
MAX_LINE_BYTES = 512  # firmware drops-and-warns above this

# ── Protocol clamps (src/protocol.h) ────────────────────────────────────────
MAX_BANDS = 5
F0_MIN_HZ = 200.0
F0_MAX_HZ = 8000.0
Q_MIN = 1.0
Q_MAX = 20.0
ATTEN_MAX_DB = 40.0  # >= this is treated as a full notch on-device
TONE_LEVEL_MIN_DB = 0.0
TONE_LEVEL_MAX_DB = 85.0  # PROTOCOL_TONE_LEVEL_MAX_DB -- nominal until calibrated

# ── Tone watchdog (src/tone_safety.c) ───────────────────────────────────────
TONE_WATCHDOG_MS = 3000  # no TONE_LEVEL within this window => firmware auto-silences
KEEPALIVE_INTERVAL_S = 1.0  # comfortably inside the watchdog, like the app's 700 ms ramp

# ── LDL test (haven-app src/constants/safety.ts) ────────────────────────────
LDL_TEST_FREQUENCIES_HZ = [1000, 2000, 3000, 4000, 6000, 8000]
LDL_START_LEVEL_DB = 30
SWEEP_STEP_DB = 5  # calibration.md procedure 1 steps 30 -> 85 in 5 dB
SWEEP_LEVELS_DB = list(range(LDL_START_LEVEL_DB, int(TONE_LEVEL_MAX_DB) + 1, SWEEP_STEP_DB))

# ── Hear-through checks (calibration.md procedure 3) ────────────────────────
HEAR_THROUGH_BANDS = [
    # (f0_hz, Q, atten_db, why)
    (4500.0, 10.0, 20.0, "the app's default band"),
    (200.0, 20.0, 40.0, "numerically worst case per tools/dsp (low f0, high Q, full notch)"),
    (8000.0, 20.0, 20.0, "top of the protocol range"),
]
INSERTION_GAIN_BAND_HZ = (200.0, 8000.0)
FULL_NOTCH_MIN_DEPTH_DB = 30.0  # a 40 dB-commanded notch is fixture/FFT-limited; this is the pass bar

# ── Acoustic references ─────────────────────────────────────────────────────
MIC_CAL_DEFAULT_REF_DBSPL = 94.0  # 1 Pa, what pistonphones/calibrators emit
LATENCY_TARGET_S = 1e-3  # hear-through target; comb filtering audible well above this
