# Device → app replies over NUS TX

Until now the link was one-way: the app wrote newline-terminated JSON to the
NUS RX characteristic and never heard back (haven-app `docs/ble-protocol.md`:
"TX characteristic is defined but not yet used"). This closes the loop.
Every line the device receives produces exactly one **ack**; a few
**events** are sent unsolicited. The formatter is `src/ack.c` (pure C,
host-tested in `tests/host/test_ack.c`); `src/main.c` wires it to
`ble_transport_send()`.

## Transport

- Characteristic: NUS TX, `6e400003-b5a3-f393-e0a9-e50e24dcca9e` (notify).
- Framing: UTF-8 JSON, one object per line, `\n`-terminated — the mirror of
  the app→device direction. The app should reassemble on `\n` too; a single
  notification always carries a whole line today, but don't rely on it.
- Size: every message is < 100 bytes, i.e. well inside one notification at
  the negotiated MTU (247 − 3 = 244). `ACK_MAX_LEN` (120) is the hard bound
  the formatter enforces; `test_length_bounds` proves the worst case fits.
- Delivery is **best-effort**. If the TX pool is momentarily exhausted
  (`-ENOMEM`/`-EAGAIN`) or the phone has already gone (`-ENOTCONN`) the ack
  is dropped with a `LOG_DBG`, never retried — the ack path runs in the BLE
  RX thread and must not block or spin. The app must treat acks as
  *confirmation*, not as the source of truth for its own state.

## Acks (one per received line)

| App sent | Device replies |
|---|---|
| `{"type":"MULTI_FILTER","bands":[…]}` | `{"ack":"MULTI_FILTER","ok":true,"bands":N}` — N = bands actually applied (after the 5-band cap) |
| `{"type":"BYPASS","enabled":B}` | `{"ack":"BYPASS","ok":true,"enabled":B}` |
| `{"type":"TONE_START","f0":F,"level_db":L}` | `{"ack":"TONE_START","ok":true,"f0":F',"level_db":L'}` — **the values the device applied after its own clamps**, rounded to integers (e.g. a requested 120 dB comes back as 85) |
| `{"type":"TONE_LEVEL","level_db":L}` | `{"ack":"TONE_LEVEL","ok":true,"level_db":L'}` |
| `{"type":"TONE_STOP"}` | `{"ack":"TONE_STOP","ok":true}` |
| anything `protocol_parse_line()` rejects | `{"ack":"?","ok":false,"err":"parse"}` |
| a parsed command the codec driver refused | `{"ack":"<TYPE>","ok":false,"err":"dsp","code":<errno>}` — e.g. `-5` (EIO) when an I2C write to the ADAU1860 fails |

`TONE_*` acks report `ok:true` when the command reached `tone_safety.c`;
they say nothing about whether a sound is audible (that is the codec route
and the not-yet-done acoustic calibration).

## Events (unsolicited)

| Event | When | Why the app needs it |
|---|---|---|
| `{"event":"boot","fw":"0.1.0-dev","fdsp_rate":192000,"dac_source":"fdsp"}` | immediately on BLE connect | Tells the app which firmware it is talking to. `dac_source` is `"fdsp"` for the product path or `"dmic_direct"` for the no-DSP smoke-test build (`CONFIG_HAVEN_DAC_SOURCE_DMIC_DIRECT`) — the app should refuse to run an LDL test against a smoke-test build (no limiter in that path). `fw` comes from the `VERSION` file (`APP_VERSION_STRING`). |
| `{"event":"tone_watchdog"}` | after `tone_safety.c` auto-silenced a tone because no `TONE_LEVEL` keep-alive arrived within 3 s | The app's own ramp state machine has no other way to learn the device stopped on its own; it should mark the LDL step as aborted, not "comfortable up to 85 dB". Sent *after* the tone is already silent — the notification is informational, the silencing never depends on it. |

## Invariants

- An ack echoes **applied** values, never requested ones. The clamp in
  `protocol.c` (`PROTOCOL_TONE_LEVEL_MAX_DB` = 85) remains the firmware's
  independent ceiling; the ack just makes it visible.
- No floats on the wire: `f0` and `level_db` are rounded to the nearest
  integer (`round_to_long`). The app's own values are integers already.
- Adding a message: extend `ack.c`, add a golden string to `test_ack.c`,
  update this file and haven-app's `docs/ble-protocol.md` in the same PR.
