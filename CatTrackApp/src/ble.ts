import {
  BleManager,
  Device,
  State,
  Subscription,
} from 'react-native-ble-plx';

import {
  DEVICE_NAME,
  SERVICE_UUID,
  SIGNAL_CHAR_UUID,
  Signal,
  decodeSignal,
} from './signal';

export type Status =
  | 'idle'
  | 'scanning'
  | 'connecting'
  | 'ready'
  | 'error';

export type TrackerState = {
  status: Status;
  signal: Signal | null;
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
  onSignal: (signal: Signal) => void,
): Promise<Subscription> {
  const connected = await device.connect();
  await connected.discoverAllServicesAndCharacteristics();
  // The firmware's notification payload is 7 bytes, so a modest MTU is plenty
  // and keeps the connection cheap. Some Android stacks default high and fail
  // to negotiate, so ask explicitly.
  try {
    await connected.requestMTU(64);
  } catch {
    // Keep the negotiated default; 7 bytes fits anything above 10.
  }

  return connected.monitorCharacteristicForService(
    SERVICE_UUID,
    SIGNAL_CHAR_UUID,
    (error, characteristic) => {
      if (error || !characteristic?.value) {
        return;
      }
      const signal = decodeSignal(base64ToBytes(characteristic.value));
      if (signal) {
        onSignal(signal);
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