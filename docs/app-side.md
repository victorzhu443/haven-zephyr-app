# What the app needs to do with the new acks (not implemented in this repo)

Firmware-side counterpart: `docs/nus-acks.md`. This note is the hand-off to
`haven-app`; nothing here changes the app in this PR.

## Subscribe

In `BleConnectionManager` after service discovery (where the bench GATT
`monitorCharacteristicForService` calls already live), add:

```ts
this.nusTxSub = device.monitorCharacteristicForService(
  UART_SERVICE_UUID, UART_TX_CHAR_UUID,
  (error, characteristic) => {
    if (error || !characteristic?.value) return;
    const text = Buffer.from(characteristic.value, 'base64').toString('utf8');
    this.rxBuffer += text;
    let nl;
    while ((nl = this.rxBuffer.indexOf('\n')) >= 0) {
      const line = this.rxBuffer.slice(0, nl);
      this.rxBuffer = this.rxBuffer.slice(nl + 1);
      this.handleDeviceLine(line);
    }
  });
```

Cancel the subscription on disconnect; clear `rxBuffer` on (re)connect.

## Parse

```ts
type DeviceAck =
  | { ack: 'MULTI_FILTER'; ok: true; bands: number }
  | { ack: 'BYPASS'; ok: true; enabled: boolean }
  | { ack: 'TONE_START'; ok: true; f0: number; level_db: number }
  | { ack: 'TONE_LEVEL'; ok: true; level_db: number }
  | { ack: 'TONE_STOP'; ok: true }
  | { ack: string; ok: false; err: 'parse' | 'dsp' | 'unknown'; code?: number };
type DeviceEvent =
  | { event: 'boot'; fw: string; fdsp_rate: number; dac_source: 'fdsp' | 'dmic_direct' }
  | { event: 'tone_watchdog' };
```

`JSON.parse` each line; ignore lines that fail to parse (forward-compatible).

## Act — the minimum that adds value (Paul's roadmap: a quiet toast, not a JSON dump)

1. **`ok:false` after a filter change** → a quiet toast ("Device didn't apply
   that change") and leave the UI showing the *previous* band state, since
   `FilterContext` currently assumes writes succeed.
2. **`event:"tone_watchdog"`** → in `useLdlTone`/`usePreviewTone`, treat as an
   external stop: clear timers, set state `idle`, and record the LDL step as
   *aborted* (do not save a result — the ramp was cut off for a non-user
   reason).
3. **`event:"boot"` with `dac_source !== "fdsp"`** → disable the Hearing tab's
   tests with an explanation ("Bench firmware without an output limiter —
   hearing tests disabled"). This is a safety gate, not cosmetics.
4. **`event:"boot"`.`fw`** → show in the connection bar's detail/About; log
   with LDL and match-run history so results are attributable to a firmware.
5. **TONE acks echoing clamped values** → if `level_db` in the ack differs
   from what was sent, the app's level meter should show the acked value;
   today the two can only differ if the app's own cap were ever loosened,
   so this is a cheap consistency check.

## Test hooks

`tools/ble_bench_test.html` (Web Bluetooth) can subscribe to NUS TX with
`await txChar.startNotifications()` and log lines — the quickest way to see
acks on a DK without the phone build.
