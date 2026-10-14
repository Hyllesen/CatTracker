/**
 * Signal model decoded from the handheld's BLE notification packet.
 *
 * Wire format (8 bytes, little-endian) produced by handheld_ble/handheld_ble.ino:
 *   [0]     flags      bit0 = fresh packet, bit1 = stale (>15s since last),
 *                      bit2 = at least one packet heard
 *   [1..2]  RSSI * 10  int16 dBm   (real reading whenever bit2 is set)
 *   [3..4]  SNR * 10   int16 dB
 *   [5]     signal     0..100, spanning -45 dBm (close) to -180 dBm (floor)
 *   [6]     age        seconds since last packet (capped 255)
 *   [7]     battery    collar cell 0..100, or 255 when unwired/unknown
 */

export const SIGNAL_CHAR_UUID = '8e7a1b21-3f4d-4c8e-9a1f-2b6d5e4c3a21';
export const SERVICE_UUID = '8e7a1b20-3f4d-4c8e-9a1f-2b6d5e4c3a21';
export const DEVICE_NAME = 'CatTracker';

export const FLAG_FRESH = 0x01;
export const FLAG_STALE = 0x02;
export const FLAG_HAVE = 0x04;

/** Byte value the firmware sends for "battery pin not wired / no reading". */
export const BATTERY_UNKNOWN = 255;

export type Signal = {
  /** Raw RSSI in dBm, or null before the first packet arrives. */
  rssi: number | null;
  /** Demodulator SNR in dB, or null before the first packet arrives. */
  snr: number | null;
  /** 0..100, mapped from measured RSSI range (-40 close .. -75 edge). */
  percent: number;
  /** Seconds since the last packet was heard. */
  ageSeconds: number;
  /** Collar cell charge 0..100, or null until the ADC pin is wired. */
  batteryPercent: number | null;
  /** True when a packet arrived in this update. */
  fresh: boolean;
  /** True when we have heard the collar before but have not recently. */
  stale: boolean;
  /** Wall-clock time the update was received, for UI freshness. */
  at: number;
};

export const EMPTY_SIGNAL: Signal = {
  rssi: null,
  snr: null,
  percent: 0,
  ageSeconds: 0,
  batteryPercent: null,
  fresh: false,
  stale: false,
  at: 0,
};

function toInt16(bytes: Uint8Array, offset: number): number {
  const u = bytes[offset] | (bytes[offset + 1] << 8);
  return u >= 0x8000 ? u - 0x10000 : u;
}

export function decodeSignal(bytes: Uint8Array | null): Signal | null {
  if (!bytes || bytes.length < 7) {
    return null;
  }

  const flags = bytes[0];
  const rssiRaw = toInt16(bytes, 1);
  const snrRaw = toInt16(bytes, 3);
  const percent = bytes[5];
  const ageSeconds = bytes[6];

  // "Never heard the collar" arrives as FLAG_HAVE, not as a magic RSSI value.
  // The firmware used to send -127 dBm as that sentinel, which sat in the
  // middle of the range the radio genuinely decodes to and silently swallowed
  // every weak reading - the handheld reached -140 dBm in field testing and
  // the app treated all of it as "no data".
  const rssi = (flags & FLAG_HAVE) !== 0 ? rssiRaw / 10 : null;

  return {
    rssi,
    // SNR is only meaningful alongside a real packet. The firmware seeds it at
    // 0, which would otherwise look like a genuine 0 dB reading.
    snr: rssi === null ? null : snrRaw / 10,
    percent,
    ageSeconds,
    batteryPercent:
      bytes.length >= 8 && bytes[7] !== BATTERY_UNKNOWN ? bytes[7] : null,
    fresh: (flags & FLAG_FRESH) !== 0,
    stale: (flags & FLAG_STALE) !== 0,
    at: Date.now(),
  };
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