/**
 * Decoder tests for the handheld's BLE roster notification.
 *
 * The important one is the HAVE case. "Never heard the collar" used to be
 * encoded as a sentinel RSSI of -127 dBm, and the decoder treated any value at
 * or below that as null. With the signal floor moved to -180 dBm that sentinel
 * sat inside the range the radio genuinely decodes to, so every weak reading
 * was reported as "no data" and rendered as silence - which is what field
 * testing at -140 dBm actually produced. The sentinel is gone; presence is
 * now carried by the HAVE flag.
 *
 * @format
 */

import {
  BATTERY_UNKNOWN,
  LEGACY_FLAG_HAVE,
  MAX_COLLARS,
  ROSTER_ENTRY_LEN,
  ROSTER_FLAG_FRESH,
  ROSTER_FLAG_HAVE,
  ROSTER_FLAG_LOW_BATT,
  ROSTER_FLAG_STALE,
  ROSTER_VERSION,
  decodeLegacy,
  decodeRoster,
  pickLeader,
  rssiToPercent,
} from '../src/signal';

type Entry = {
  flags?: number;
  rssi?: number;
  snr?: number;
  age?: number;
  battery?: number;
};

/** Build a roster the way handheld_ble.ino does. */
function roster(entries: Entry[]): Uint8Array {
  const bytes = new Uint8Array(2 + entries.length * ROSTER_ENTRY_LEN);
  bytes[0] = ROSTER_VERSION;
  bytes[1] = entries.length;
  const view = new DataView(bytes.buffer);
  entries.forEach((e, i) => {
    const o = 2 + i * ROSTER_ENTRY_LEN;
    const flags = e.flags ?? ROSTER_FLAG_HAVE;
    bytes[o] = flags;
    view.setInt16(o + 1, Math.round((e.rssi ?? -100) * 10), true);
    bytes[o + 3] = ((e.snr ?? 10) * 2) & 0xff;
    bytes[o + 4] = e.age ?? 0;
    bytes[o + 5] = e.battery ?? BATTERY_UNKNOWN;
  });
  return bytes;
}

/** Build a legacy single-collar packet the way the first firmware did. */
function legacyPacket(opts: {
  flags?: number;
  rssi?: number;
  snr?: number;
  percent?: number;
  age?: number;
  battery?: number;
}): Uint8Array {
  const {
    flags = LEGACY_FLAG_HAVE,
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
  // -140 dBm decoded fine in field testing. Before HAVE this came back null
  // because of the -127 sentinel, and the UI showed nothing at all.
  const r = decodeRoster(roster([{flags: ROSTER_FLAG_HAVE, rssi: -140, snr: -6}]));
  expect(r).not.toBeNull();
  expect(r!.collars[0].rssi).toBe(-140);
  expect(r!.collars[0].snr).toBe(-6);
});

test('a reading below the old sentinel survives', () => {
  // The whole reason the sentinel was replaced. -160 would have been null.
  const r = decodeRoster(roster([{rssi: -160}]));
  expect(r!.collars[0].rssi).toBe(-160);
});

test('without HAVE the RSSI is null whatever the bytes say', () => {
  const r = decodeRoster(roster([{flags: 0, rssi: -140}]));
  expect(r).not.toBeNull();
  expect(r!.collars[0].rssi).toBeNull();
  expect(r!.collars[0].snr).toBeNull();
  expect(r!.collars[0].percent).toBe(0);
});

test('fresh and stale are read independently of have', () => {
  expect(
    decodeRoster(roster([{flags: ROSTER_FLAG_HAVE | ROSTER_FLAG_FRESH}]))!
      .collars[0].fresh,
  ).toBe(true);
  expect(
    decodeRoster(roster([{flags: ROSTER_FLAG_HAVE | ROSTER_FLAG_STALE}]))!
      .collars[0].stale,
  ).toBe(true);
  expect(
    decodeRoster(roster([{flags: ROSTER_FLAG_HAVE}]))!.collars[0].stale,
  ).toBe(false);
});

test('the low battery flag is carried through', () => {
  expect(
    decodeRoster(roster([{flags: ROSTER_FLAG_HAVE | ROSTER_FLAG_LOW_BATT}]))!
      .collars[0].lowBattery,
  ).toBe(true);
});

test('percent is derived from the rssi, not taken from the wire', () => {
  const r = decodeRoster(roster([{rssi: -180}]));
  expect(r!.collars[0].percent).toBe(0);
  expect(r!.collars[0].percent).toBe(rssiToPercent(-180));
  expect(decodeRoster(roster([{rssi: -45}]))!.collars[0].percent).toBe(100);
});

test('age and battery pass through', () => {
  const r = decodeRoster(roster([{age: 7, battery: 87}]));
  expect(r!.collars[0].ageSeconds).toBe(7);
  expect(r!.collars[0].batteryPercent).toBe(87);
});

test('battery is null when the firmware reports unknown', () => {
  expect(
    decodeRoster(roster([{battery: BATTERY_UNKNOWN}]))!.collars[0]
      .batteryPercent,
  ).toBeNull();
});

test('a truncated roster is rejected rather than read as a short one', () => {
  // Losing a collar from the end of the roster would look exactly like that
  // cat having gone quiet, so a short packet must not be decoded at all.
  const full = roster([
    {rssi: -100},
    {rssi: -110},
    {rssi: -120},
  ]);
  expect(decodeRoster(full.slice(0, full.length - 3))).toBeNull();
  expect(decodeRoster(new Uint8Array(4))).toBeNull();
  expect(decodeRoster(null)).toBeNull();
});

test('a roster that claims more collars than it carries is rejected', () => {
  const bytes = roster([{rssi: -100}]);
  bytes[1] = MAX_COLLARS;   // claims three, carries one
  expect(decodeRoster(bytes)).toBeNull();
});

test('the legacy packet is not mistaken for a roster', () => {
  // The legacy flags byte only ever used bits 0-2, so it can never collide
  // with the roster version. This is the check that keeps a phone running
  // the new app working against a handheld that has not been reflashed.
  expect(decodeRoster(legacyPacket({}))).toBeNull();
  expect(decodeLegacy(legacyPacket({}))!.collars[0].rssi).toBe(-100);
});

test('the legacy packet still decodes as a one-collar roster', () => {
  const r = decodeLegacy(legacyPacket({flags: LEGACY_FLAG_HAVE, rssi: -140, snr: -6}));
  expect(r!.collars).toHaveLength(1);
  expect(r!.collars[0].id).toBe(0);
  expect(r!.collars[0].rssi).toBe(-140);
  expect(r!.collars[0].have).toBe(true);
});

test('the leader is the strongest collar that has not gone stale', () => {
  const r = decodeRoster(
    roster([
      {rssi: -100, flags: ROSTER_FLAG_HAVE},
      {rssi: -60, flags: ROSTER_FLAG_HAVE | ROSTER_FLAG_STALE},
      {rssi: -90, flags: ROSTER_FLAG_HAVE},
    ]),
  );
  expect(pickLeader(r!.collars)!.id).toBe(2);
});

test('no leader when nothing has been heard', () => {
  const r = decodeRoster(roster([{flags: 0}]));
  expect(pickLeader(r!.collars)).toBeNull();
});
