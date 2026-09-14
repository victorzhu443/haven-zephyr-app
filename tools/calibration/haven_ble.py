"""Thin BLE client for the calibration rig: speaks Haven's NUS JSON protocol.

Same wire contract as the phone app and the firmware (`src/protocol.h`,
haven-app `docs/ble-protocol.md`): newline-terminated JSON over the Nordic
UART Service RX characteristic, parameters clamped here to what the firmware
would clamp them to anyway -- so the rig can never hand the board a value it
would silently alter, and every payload the rig *logs* is the payload the
board *applied*.

Two safety behaviours are built in rather than left to callers:
- A tone is kept alive by re-sending TONE_LEVEL every KEEPALIVE_INTERVAL_S
  (well inside the firmware's 3 s watchdog). Stop the keep-alive and the
  firmware silences the tone on its own within 3 s -- which is exactly what
  `ceiling_check.py --disconnect-test` measures.
- `--dry-run` never imports bleak: payloads are appended to `client.sent`
  and printed, so every procedure can be rehearsed with no radio.

Usage (module):
    with HavenSession(dry_run=True) as s:
        s.tone_start(4000, 30); s.tone_level(35); s.tone_stop()

Neither this file nor anything in tools/calibration has been exercised
against a real board: the BLE leg mirrors ml/apply_over_ble.py's bleak usage
and is unit-tested only in dry-run.
"""
import asyncio
import json
import threading
import time

from constants import (
    ATTEN_MAX_DB,
    DEVICE_NAME,
    F0_MAX_HZ,
    F0_MIN_HZ,
    KEEPALIVE_INTERVAL_S,
    LINE_TERMINATOR,
    MAX_BANDS,
    MAX_LINE_BYTES,
    NUS_RX_CHAR_UUID,
    Q_MAX,
    Q_MIN,
    TONE_LEVEL_MAX_DB,
    TONE_LEVEL_MIN_DB,
)


# ── Clamps (mirror src/protocol.c) ───────────────────────────────────────────
def clamp(value, lo, hi):
    return max(lo, min(hi, value))


def clamp_level_db(level_db):
    return clamp(float(level_db), TONE_LEVEL_MIN_DB, TONE_LEVEL_MAX_DB)


def clamp_band(f0_hz, q, atten_db=None):
    band = {"f0": clamp(float(f0_hz), F0_MIN_HZ, F0_MAX_HZ), "Q": clamp(float(q), Q_MIN, Q_MAX)}
    if atten_db is not None:
        band["atten_db"] = clamp(float(atten_db), 0.0, ATTEN_MAX_DB)
    return band


# ── Payload builders ─────────────────────────────────────────────────────────
def encode_line(payload):
    """dict -> bytes exactly as the firmware's line assembler expects."""
    line = json.dumps(payload, separators=(",", ":")) + LINE_TERMINATOR
    data = line.encode("utf-8")
    if len(data) > MAX_LINE_BYTES:
        raise ValueError(f"payload is {len(data)} bytes; firmware drops lines over {MAX_LINE_BYTES}")
    return data


def tone_start_payload(f0_hz, level_db):
    return {"type": "TONE_START", "f0": clamp(float(f0_hz), F0_MIN_HZ, F0_MAX_HZ),
            "level_db": clamp_level_db(level_db)}


def tone_level_payload(level_db):
    return {"type": "TONE_LEVEL", "level_db": clamp_level_db(level_db)}


def tone_stop_payload():
    return {"type": "TONE_STOP"}


def multi_filter_payload(bands):
    """bands: iterable of (f0_hz, q) or (f0_hz, q, atten_db). Truncated to
    MAX_BANDS like the firmware does."""
    out = []
    for band in list(bands)[:MAX_BANDS]:
        out.append(clamp_band(*band))
    if not out:
        raise ValueError("MULTI_FILTER needs at least one band")
    return {"type": "MULTI_FILTER", "bands": out}


def bypass_payload(enabled):
    return {"type": "BYPASS", "enabled": bool(enabled)}


# ── Transport ────────────────────────────────────────────────────────────────
class DryRunTransport:
    """Records payloads instead of sending them."""

    def __init__(self, verbose=True):
        self.sent = []
        self.verbose = verbose

    def connect(self):
        if self.verbose:
            print("(--dry-run: not connecting over BLE)")

    def send(self, data):
        self.sent.append(data)
        if self.verbose:
            print(f"  -> {data.decode().rstrip()}")

    def disconnect(self):
        pass


class BleakTransport:
    """Real radio via bleak, run on a private event loop so the procedure
    scripts can stay plain synchronous code. Mirrors ml/apply_over_ble.py."""

    def __init__(self, device_name=DEVICE_NAME, timeout_s=10.0):
        from bleak import BleakClient, BleakScanner  # deferred: dry-run needs no bleak

        self._BleakClient = BleakClient
        self._BleakScanner = BleakScanner
        self.device_name = device_name
        self.timeout_s = timeout_s
        self.loop = asyncio.new_event_loop()
        self.client = None

    def _run(self, coro):
        return self.loop.run_until_complete(coro)

    def connect(self):
        async def go():
            print(f'Scanning for "{self.device_name}" ({self.timeout_s}s)...')
            device = await self._BleakScanner.find_device_by_name(self.device_name, timeout=self.timeout_s)
            if device is None:
                raise RuntimeError(f'No device advertising as "{self.device_name}" within {self.timeout_s}s')
            print(f"Connecting to {device.address}...")
            client = self._BleakClient(device)
            await client.connect()
            return client

        self.client = self._run(go())

    def send(self, data):
        if self.client is None:
            raise RuntimeError("not connected")
        self._run(self.client.write_gatt_char(NUS_RX_CHAR_UUID, data, response=True))

    def disconnect(self):
        if self.client is not None:
            try:
                self._run(self.client.disconnect())
            finally:
                self.client = None


# ── Session ──────────────────────────────────────────────────────────────────
class HavenSession:
    """Context manager owning one connection and, at most, one live tone."""

    def __init__(self, dry_run=False, device_name=DEVICE_NAME, timeout_s=10.0, verbose=True,
                 keepalive_interval_s=KEEPALIVE_INTERVAL_S):
        self.transport = DryRunTransport(verbose) if dry_run else BleakTransport(device_name, timeout_s)
        self.dry_run = dry_run
        self.keepalive_interval_s = keepalive_interval_s
        self._level_db = None
        self._keepalive = None
        self._stop_evt = threading.Event()
        self._lock = threading.Lock()

    # lifecycle
    def __enter__(self):
        self.transport.connect()
        return self

    def __exit__(self, *exc):
        try:
            self.tone_stop()
        finally:
            self.transport.disconnect()

    @property
    def sent(self):
        return getattr(self.transport, "sent", None)

    def _send(self, payload):
        with self._lock:
            self.transport.send(encode_line(payload))

    # commands
    def tone_start(self, f0_hz, level_db, keepalive=True):
        self.tone_stop()
        payload = tone_start_payload(f0_hz, level_db)
        self._level_db = payload["level_db"]
        self._send(payload)
        if keepalive:
            self._start_keepalive()
        return payload

    def tone_level(self, level_db):
        payload = tone_level_payload(level_db)
        self._level_db = payload["level_db"]
        self._send(payload)
        return payload

    def tone_stop(self):
        self._stop_keepalive()
        if self._level_db is not None:
            self._level_db = None
            self._send(tone_stop_payload())

    def drop_keepalive(self):
        """Stop refreshing the tone WITHOUT sending TONE_STOP -- simulates a
        frozen app / dropped link so the firmware watchdog has to act."""
        self._stop_keepalive()
        self._level_db = None

    def multi_filter(self, bands):
        payload = multi_filter_payload(bands)
        self._send(payload)
        return payload

    def bypass(self, enabled):
        payload = bypass_payload(enabled)
        self._send(payload)
        return payload

    # keep-alive
    def _start_keepalive(self):
        self._stop_evt.clear()

        def loop():
            while not self._stop_evt.wait(self.keepalive_interval_s):
                level = self._level_db
                if level is None:
                    return
                self._send(tone_level_payload(level))

        self._keepalive = threading.Thread(target=loop, daemon=True)
        self._keepalive.start()

    def _stop_keepalive(self):
        self._stop_evt.set()
        if self._keepalive is not None and self._keepalive.is_alive():
            self._keepalive.join(timeout=self.keepalive_interval_s * 2)
        self._keepalive = None


def hold(seconds):
    """Sleep helper the procedures use between commands (kept here so tests
    can monkeypatch one place)."""
    time.sleep(seconds)
