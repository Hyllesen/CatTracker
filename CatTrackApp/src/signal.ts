/**
 * Roster decoded from the handheld's BLE notification.
 *
 * Wire format (20 bytes, little-endian) produced by
 * handheld_ble/handheld_ble.ino:
 *   [0]     version   0x10
 *   [1]     count     how many collars follow
 *   then 6 bytes per collar, in id order - the position IS the id:
 *     +0     flags    bit0 HAVE, bit1 FRESH, bit2 STALE, bit3 LOW_BATT
 *     +1..2  RSSI * 10, int16 dBm
 *     +3     SNR * 2, int8 dB (0.5 dB steps)
 *     +4     age      seconds since that collar's last packet (capped 255)
 *     +5     battery  0..100, or 255 when unwired/unknown
 *
 * The whole roster is 2 + 6 * 3 = 20 bytes, which is exactly what fits the
 * DEFAULT ATT MTU of 23. NimBLE truncates anything longer to MTU-3 rather
 * than failing, so an oversized roster would arrive silently mangled. This
 * format therefore needs no MTU negotiation at all - the phone works with
 * the handshake or without it.
 *
 * The signal strength percentage is NOT on the wire: it is a straight
 * function of the RSSI, so the phone derives it from RSSI_CEIL/RSSI_FLOOR
 * below and the two never have to agree about it.
 */

export const SERVICE_UUID = '8e7a1b20-3f4d-4c8e-9a1f-2b6d5e4c3a21';
export const SIGNAL_CHAR_UUID = '8e7a1b21-3f4d-4c8e-9a1f-2b6d5e4c3a21';
export const DEVICE_NAME = 'CatTracker';

/**
 * Roster format version.
 *
 * 0x10 rather than the more natural 0x02: the legacy single-collar packet
 * started with a flags byte that only ever used bits 0-2, so a value with
 * bit 4 set cannot be one. The two formats are therefore told apart by the
 * first byte alone, with no guessing - which matters because a phone running
 * this app may still be talking to a handheld that has not been reflashed.
 */
export const ROSTER_VERSION = 0x10;

/** Collars the handheld tracks at once. Bounded by the 20-byte roster. */
export const MAX_COLLARS = 3;

/** Bytes per collar in the roster. */
export const ROSTER_ENTRY_LEN = 6;

export const ROSTER_FLAG_HAVE = 0x01;
export const ROSTER_FLAG_FRESH = 0x02;
export const ROSTER_FLAG_STALE = 0x04;
export const ROSTER_FLAG_LOW_BATT = 0x08;

/** Byte value the firmware sends for "battery pin not wired / no reading". */
export const BATTERY_UNKNOWN = 255;

/**
 * RSSI range mapped onto the signal bar.
 *
 * These used to live in the handheld firmware, which also computed the
 * percentage. The mapping is a display concern, so it belongs here, where
 * the bar is drawn - one copy, no cross-firmware agreement needed.
 *
 * The floor is deliberately far below what has actually been measured: field
 * testing decoded packets down to -140 dBm, and an earlier -75 floor mapped
 * the entire far field to 0% and therefore to silence - exactly the range
 * where guidance matters most.
 */
export const RSSI_CEIL = -45;
export const RSSI_FLOOR = -180;

export type Collar = {
  /** Collar id, 0..MAX_COLLARS-1. Set at flash time on the collar. */
  id: number;
  /** True once a packet has been heard from this collar. */
  have: boolean;
  /** True when this collar's entry was updated by the packet just received. */
  fresh: boolean;
  /** True when the collar was heard but has since gone quiet. */
  stale: boolean;
  /** True when the collar reports a low battery. */
  lowBattery: boolean;
  /** Smoothed RSSI in dBm, or null before the first packet arrives. */
  rssi: number | null;
  /** Demodulator SNR in dB, or null before the first packet arrives. */
  snr: number | null;
  /** 0..100, mapped from the measured RSSI range. */
  percent: number;
  /** Seconds since this collar's last packet was heard. */
  ageSeconds: number;
  /** Collar cell charge 0..100, or null until the ADC pin is wired. */
  batteryPercent: number | null;
};

export type Roster = {
  /** One entry per collar slot, in id order. */
  collars: Collar[];
  /** Wall-clock time the update was received, for UI freshness. */
  at: number;
};

export function rssiToPercent(rssi: number): number {
  if (rssi <= RSSI_FLOOR) {
    return 0;
  }
  if (rssi >= RSSI_CEIL) {
    return 100;
  }
  return Math.round(((rssi - RSSI_FLOOR) * 100) / (RSSI_CEIL - RSSI_FLOOR));
}

function toInt16(bytes: Uint8Array, offset: number): number {
  const u = bytes[offset] | (bytes[offset + 1] << 8);
  return u >= 0x8000 ? u - 0x10000 : u;
}

function toInt8(byte: number): number {
  return byte >= 0x80 ? byte - 0x100 : byte;
}

/**
 * Decode a roster notification.
 *
 * Returns null for anything that is not a roster - including the legacy
 * single-collar packet, which the caller can then try to decode the old way.
 */
export function decodeRoster(bytes: Uint8Array | null): Roster | null {
  if (!bytes || bytes.length < 2) {
    return null;
  }
  if (bytes[0] !== ROSTER_VERSION) {
    return null;
  }

  const count = bytes[1];
  if (count > MAX_COLLARS) {
    return null;
  }
  // Length validation. Without it a truncated notification would be read as
  // a short roster and the collars that did not fit would silently vanish -
  // which looks exactly like those cats having gone quiet.
  if (2 + count * ROSTER_ENTRY_LEN > bytes.length) {
    return null;
  }

  const collars: Collar[] = [];
  for (let i = 0; i < count; i++) {
    const o = 2 + i * ROSTER_ENTRY_LEN;
    const flags = bytes[o];
    const have = (flags & ROSTER_FLAG_HAVE) !== 0;
    const rssi = have ? toInt16(bytes, o + 1) / 10 : null;

    collars.push({
      id: i,
      have,
      fresh: (flags & ROSTER_FLAG_FRESH) !== 0,
      stale: (flags & ROSTER_FLAG_STALE) !== 0,
      lowBattery: (flags & ROSTER_FLAG_LOW_BATT) !== 0,
      rssi,
      // SNR only means anything alongside a real packet.
      snr: have ? toInt8(bytes[o + 3]) / 2 : null,
      percent: rssi === null ? 0 : rssiToPercent(rssi),
      ageSeconds: bytes[o + 4],
      batteryPercent:
        have && bytes[o + 5] !== BATTERY_UNKNOWN ? bytes[o + 5] : null,
    });
  }

  return {collars, at: Date.now()};
}

/**
 * The collar to follow: the strongest signal that is not stale.
 *
 * Stale collars are excluded rather than ranked last, because a collar that
 * has gone quiet is telling you nothing about where the cat is - it is
 * telling you the cat is not answering, which is a different problem and
 * gets its own row in the UI.
 */
export function pickLeader(collars: Collar[]): Collar | null {
  let best: Collar | null = null;
  let bestRssi = -Infinity;
  for (const c of collars) {
    if (!c.have || c.stale || c.rssi === null) {
      continue;
    }
    if (c.rssi > bestRssi) {
      best = c;
      bestRssi = c.rssi;
    }
  }
  return best;
}

/**
 * Map signal strength onto a discrete tone step (0..9).
 *
 * Discrete steps rather than a continuous pitch sweep: a smooth sweep is easy
 * to miss while walking, while clearly distinct levels are easier to read and
 * let each level carry its own character.
 */
export function toneStepFor(percent: number, steps = 10): number {
  if (percent <= 0) {
    return 0;
  }
  const step = Math.ceil((percent / 100) * steps);
  return Math.min(steps, Math.max(1, step));
}

// ---------------------------------------------------------------------------
// Legacy single-collar packet
// ---------------------------------------------------------------------------
// The first handheld firmware published one collar's state as an 8-byte
// packet. A phone running this app may still be paired with a handheld that
// has not been reflashed, so the old format is still understood - it is
// simply reported as a one-collar roster.
//
//   [0]     flags  bit0 fresh, bit1 stale, bit2 have
//   [1..2]  RSSI * 10, int16 dBm
//   [3..4]  SNR * 10, int16 dB
//   [5]     signal 0..100
//   [6]     age seconds
//   [7]     battery 0..100, or 255 unknown
//
// The formats cannot be confused: the legacy flags byte only ever uses bits
// 0-2, so it can never equal ROSTER_VERSION.
export const LEGACY_FLAG_FRESH = 0x01;
export const LEGACY_FLAG_STALE = 0x02;
export const LEGACY_FLAG_HAVE = 0x04;
export const LEGACY_PACKET_LEN = 8;

export function decodeLegacy(bytes: Uint8Array | null): Roster | null {
  if (!bytes || bytes.length < LEGACY_PACKET_LEN) {
    return null;
  }

  const flags = bytes[0];
  const have = (flags & LEGACY_FLAG_HAVE) !== 0;
  const rssiRaw = toInt16(bytes, 1);
  const snrRaw = toInt16(bytes, 3);
  const rssi = have ? rssiRaw / 10 : null;

  return {
    collars: [
      {
        id: 0,
        have,
        fresh: (flags & LEGACY_FLAG_FRESH) !== 0,
        stale: (flags & LEGACY_FLAG_STALE) !== 0,
        lowBattery: false,
        rssi,
        snr: rssi === null ? null : snrRaw / 10,
        percent: bytes[5],
        ageSeconds: bytes[6],
        batteryPercent:
          bytes[7] !== BATTERY_UNKNOWN ? bytes[7] : null,
      },
    ],
    at: Date.now(),
  };
}
