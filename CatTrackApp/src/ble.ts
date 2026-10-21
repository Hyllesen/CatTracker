import {
  BleManager,
  Device,
  State,
  Subscription,
} from 'react-native-ble-plx';

import {
  DEVICE_NAME,
  Roster,
  SERVICE_UUID,
  SIGNAL_CHAR_UUID,
  decodeLegacy,
  decodeRoster,
} from './signal';

export type Status =
  | 'idle'
  | 'scanning'
  | 'connecting'
  | 'ready'
  | 'error';

export type TrackerState = {
  status: Status;
  roster: Roster | null;
  error: string | null;
};

const ble = new BleManager();

/**
 * BLE characteristic values arrive base64-encoded. Decode without pulling in
 * node's Buffer, which is not available in the React Native runtime.
 */
function base64ToBytes(b64: string): Uint8Array {
  const chars =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const clean = b64.replace(/[^A-Za-z0-9+/]/g, '');
  const out = new Uint8Array(Math.floor((clean.length * 3) / 4));

  let bits = 0;
  let value = 0;
  let index = 0;
  for (let i = 0; i < clean.length; i++) {
    const pos = chars.indexOf(clean[i]);
    if (pos === -1) {
      continue;
    }
    value = (value << 6) | pos;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[index++] = (value >> bits) & 0xff;
    }
  }
  return out.subarray(0, index);
}

/**
 * Decode whatever the handheld sent.
 *
 * The roster is the current format; the legacy single-collar packet is still
 * accepted so the app keeps working against a handheld that has not been
 * reflashed yet. The two are told apart by the first byte, so this cannot
 * guess wrong.
 */
export function decodeHandheld(bytes: Uint8Array | null): Roster | null {
  return decodeRoster(bytes) ?? decodeLegacy(bytes);
}

export type ScanResult = {
  device: Device | null;
  /** Every device seen during the scan, for diagnosing "not found". */
  seen: Array<{id: string; name: string | null; rssi: number | null}>;
};

/**
 * Scan for the handheld.
 *
 * Deliberately scans *unfiltered* rather than passing the name to
 * startDeviceScan(). Filtering by name is done client-side so that a device
 * whose advertised local name is missing or odd still gets matched, and so the
 * caller can show what else was nearby when nothing matches - far more useful
 * than a bare "not found".
 */
export async function scanForHandheld(timeoutMs = 8000): Promise<ScanResult> {
  let device: Device | null = null;
  const seen: ScanResult['seen'] = [];
  const byId = new Map<string, {id: string; name: string | null; rssi: number | null}>();

  await ble.startDeviceScan(null, {allowDuplicates: true}, (error, d) => {
    if (error || !d) {
      return;
    }
    const name = d.name ?? d.localName ?? null;
    const prior = byId.get(d.id);
    if (prior) {
      // Keep the strongest reading so the list is not full of duplicates.
      prior.rssi = d.rssi ?? prior.rssi;
      prior.name = name ?? prior.name;
      return;
    }
    const entry = {id: d.id, name, rssi: d.rssi ?? null};
    byId.set(d.id, entry);
    seen.push(entry);

    if (name === DEVICE_NAME && !device) {
      device = d;
    }
  });

  await new Promise<void>(res => setTimeout(res, timeoutMs));
  await ble.stopDeviceScan();
  seen.sort((a, b) => (b.rssi ?? -999) - (a.rssi ?? -999));
  return {device, seen};
}

/**
 * Connect, discover the GATT tree and subscribe to the signal characteristic.
 *
 * The listener is supplied to monitorCharacteristicForService directly; this
 * library does not use a separate onValue() builder.
 */
export async function connectAndSubscribe(
  device: Device,
  onRoster: (roster: Roster) => void,
): Promise<Subscription> {
  const connected = await device.connect();
  await connected.discoverAllServicesAndCharacteristics();

  // The roster is 20 bytes, which fits the default ATT payload, so this is
  // belt and braces rather than a requirement. Some Android stacks default
  // high and fail to negotiate, so ask explicitly and carry on either way.
  try {
    await connected.requestMTU(64);
  } catch {
    // Keep the negotiated default; 20 bytes fits anything above 23.
  }

  // Read the roster once before subscribing. Without this the phone only
  // learns about the cats that happen to ping after it connected, which is
  // how you end up convinced that one cat is missing when it is simply
  // quiet. The characteristic is readable for exactly this reason.
  try {
    const snapshot = await connected.readCharacteristicForService(
      SERVICE_UUID,
      SIGNAL_CHAR_UUID,
    );
    if (snapshot?.value) {
      const roster = decodeHandheld(base64ToBytes(snapshot.value));
      if (roster) {
        onRoster(roster);
      }
    }
  } catch {
    // Not fatal: the notifications carry the same information a moment
    // later. A handheld that does not support reads simply has no snapshot.
  }

  return connected.monitorCharacteristicForService(
    SERVICE_UUID,
    SIGNAL_CHAR_UUID,
    (error, characteristic) => {
      if (error || !characteristic?.value) {
        return;
      }
      const roster = decodeHandheld(base64ToBytes(characteristic.value));
      if (roster) {
        onRoster(roster);
      }
    },
  );
}

export async function disconnect(device: Device): Promise<void> {
  try {
    await device.cancelConnection();
  } catch {
    // Already disconnected - nothing to recover.
  }
}

export function isBluetoothEnabled(): Promise<boolean> {
  return ble.state().then(s => s === State.PoweredOn);
}
