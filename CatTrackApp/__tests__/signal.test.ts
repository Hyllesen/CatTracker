/**
 * Decoder tests for the handheld's BLE notification packet.
 *
 * The important one is the FLAG_HAVE case. "Never heard the collar" used to be
 * encoded as a sentinel RSSI of -127 dBm, and the decoder treated any value at
 * or below that as null. With the signal floor moved to -180 dBm that sentinel
 * sat inside the range the radio genuinely decodes to, so every weak reading
 * was reported as "no data" and rendered as silence - which is what field
 * testing at -140 dBm actually produced. The sentinel is gone; presence is
 * now carried by FLAG_HAVE.
 *
 * @format
 */

import {
  BATTERY_UNKNOWN,
  decodeSignal,
  FLAG_FRESH,
  FLAG_HAVE,
  FLAG_STALE,
} from '../src/signal';

/** Build a wire-format packet the way handheld_ble.ino does. */
function packet(opts: {
  flags?: number;
  rssi?: number;
  snr?: number;
  percent?: number;
  age?: number;
  battery?: number;
}): Uint8Array {
  const {
    flags = FLAG_HAVE,
    rssi = -100,
    snr = 10,
    percent = 50,
    age = 0,
    battery = BATTERY_UNKNOWN,
  } = opts;
  const bytes = new Uint8Array(8);
  bytes[0] = flags;
  const view = new DataView(bytes.buffer);
  view.setInt16(1, Math.round(rssi * 10), true);
  view.setInt16(3, Math.round(snr * 10), true);
  bytes[5] = percent;
  bytes[6] = age;
  bytes[7] = battery;
  return bytes;
}

test('a weak but real reading is not mistaken for no data', () => {
  // -140 dBm decoded fine in field testing. Before FLAG_HAVE this came back
  // null because of the -127 sentinel, and the UI showed nothing at all.
  const signal = decodeSignal(packet({flags: FLAG_HAVE, rssi: -140, snr: -6}));
  expect(signal).not.toBeNull();
  expect(signal!.rssi).toBe(-140);
  expect(signal!.snr).toBe(-6);
});

test('a reading below the old sentinel survives', () => {
  // The whole reason the sentinel was replaced. -160 would have been null.
  const signal = decodeSignal(packet({flags: FLAG_HAVE, rssi: -160}));
  expect(signal!.rssi).toBe(-160);
});

test('without FLAG_HAVE the RSSI is null whatever the bytes say', () => {
  const signal = decodeSignal(packet({flags: 0, rssi: -140}));
  expect(signal).not.toBeNull();
  expect(signal!.rssi).toBeNull();
  expect(signal!.snr).toBeNull();
});

test('fresh and stale are read independently of have', () => {
  expect(
    decodeSignal(packet({flags: FLAG_HAVE | FLAG_FRESH}))!.fresh,
  ).toBe(true);
  expect(
    decodeSignal(packet({flags: FLAG_HAVE | FLAG_STALE}))!.stale,
  ).toBe(true);
  expect(decodeSignal(packet({flags: FLAG_HAVE}))!.stale).toBe(false);
});

test('percent and age pass through', () => {
  const signal = decodeSignal(packet({percent: 29, age: 7}));
  expect(signal!.percent).toBe(29);
  expect(signal!.ageSeconds).toBe(7);
});

test('battery is null when the firmware reports unknown', () => {
  expect(decodeSignal(packet({battery: BATTERY_UNKNOWN}))!.batteryPercent)
    .toBeNull();
  expect(decodeSignal(packet({battery: 87}))!.batteryPercent).toBe(87);
});

test('a 7-byte packet is accepted and yields no battery', () => {
  const short = packet({}).slice(0, 7);
  expect(decodeSignal(short)).not.toBeNull();
  expect(decodeSignal(short)!.batteryPercent).toBeNull();
});

test('a packet too short to hold a reading is rejected', () => {
  expect(decodeSignal(new Uint8Array(4))).toBeNull();
  expect(decodeSignal(null)).toBeNull();
});