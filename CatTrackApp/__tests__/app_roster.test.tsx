/**
 * The app, driven end to end with a mocked handheld.
 *
 * The BLE stack is mocked so the scan finds a "CatTracker", the roster read
 * returns real 20-byte roster bytes, and the notifications carry a fresh
 * collar. That covers the whole path a packet takes - decode, pick the
 * leader, decide what to show - which is where the display decisions
 * actually live.
 *
 * The roster here is the same shape the handheld publishes: version 0x10,
 * count 3, and three six-byte entries in id order.
 *
 * @format
 */

import React from 'react';
import ReactTestRenderer from 'react-test-renderer';

import App from '../App';

/** Roster bytes, as the handheld sends them. */
function rosterBytes(entries: Array<{
  have: boolean;
  fresh?: boolean;
  rssi?: number;
  snr?: number;
  age?: number;
  battery?: number;
}>): number[] {
  const out = [0x10, entries.length];
  entries.forEach(e => {
    const flags = (e.have ? 0x01 : 0x00) | (e.fresh ? 0x02 : 0x00);
    const rssi = Math.round((e.rssi ?? -100) * 10);
    const snr = Math.round((e.snr ?? 10) * 2);
    out.push(
      flags,
      rssi & 0xff,
      (rssi >> 8) & 0xff,
      snr & 0xff,
      e.age ?? 0,
      e.battery ?? 0xff,
    );
  });
  return out;
}

// Jest only lets a mock factory reference variables whose names start with
// 'mock'.
let mockRoster: number[] = [];

jest.mock('react-native-ble-plx', () => {
  const State = {PoweredOn: 'PoweredOn'};
  // Computed per call: the factory runs at import time, before the
  // test has chosen which roster to send.
  const value = () => Buffer.from(mockRoster).toString('base64');

  const connected: any = {
    id: 'mock',
    name: 'CatTracker',
    localName: 'CatTracker',
    rssi: -50,
    connect: jest.fn(async () => connected),
    cancelConnection: jest.fn(async () => undefined),
    discoverAllServicesAndCharacteristics: jest.fn(async () => undefined),
    requestMTU: jest.fn(async () => 64),
    readCharacteristicForService: jest.fn(async () => ({value: value()})),
    monitorCharacteristicForService: jest.fn(async (_s: any, _c: any, cb: any) => {
      setTimeout(() => cb(null, {value: value()}), 10);
      return {remove: jest.fn()};
    }),
  };

  return {
    State,
    BleManager: jest.fn(() => ({
      state: jest.fn(async () => State.PoweredOn),
      startDeviceScan: jest.fn(async (_f: any, _o: any, cb: any) => cb(null, connected)),
      stopDeviceScan: jest.fn(async () => undefined),
    })),
  };
});

function textsOf(tree: ReactTestRenderer.ReactTestRenderer): string[] {
  const out: string[] = [];
  const walk = (node: any) => {
    if (node == null) {
      return;
    }
    if (typeof node === 'string') {
      out.push(node);
      return;
    }
    if (Array.isArray(node)) {
      node.forEach(walk);
      return;
    }
    if (node.children) {
      walk(node.children);
    }
  };
  walk(tree.toJSON());
  return out;
}

/** Render the app and press Connect, letting the scan and the roster land. */
async function connect(): Promise<ReactTestRenderer.ReactTestRenderer> {
  let tree!: ReactTestRenderer.ReactTestRenderer;
  await ReactTestRenderer.act(async () => {
    tree = ReactTestRenderer.create(<App />);
  });

  // deep: true is needed - findAll only looks at host components by default,
  // and the onPress lives on the composite Pressable.
  const connectButton = tree.root.findAll(
    n =>
      n.props.accessibilityLabel === 'Connect' &&
      typeof n.props.onPress === 'function',
    {deep: true},
  );
  expect(connectButton.length).toBeGreaterThan(0);

  await ReactTestRenderer.act(async () => {
    connectButton[0].props.onPress();
    // The scan waits out its full timeout before it returns, and the start
    // path has a few awaits before it even gets there. The async variant of
    // advanceTimersByTime is the one that lets promises settle between
    // timers; the plain one runs them all inside the act scope and React
    // complains about it.
    await jest.advanceTimersByTimeAsync(12000);
  });
  return tree;
}

/** Press something in the roster by its accessibility label. */
async function press(tree: ReactTestRenderer.ReactTestRenderer, label: string) {
  const target = tree.root.findAll(
    n =>
      n.props.accessibilityLabel === label &&
      typeof n.props.onPress === 'function',
    {deep: true},
  );
  expect(target.length).toBeGreaterThan(0);
  await ReactTestRenderer.act(async () => {
    target[0].props.onPress();
    await Promise.resolve();
  });
}

beforeEach(() => {
  jest.useFakeTimers();
});

afterEach(() => {
  jest.useRealTimers();
});

test('shows the dBm of the cat it is following', async () => {
  mockRoster = rosterBytes([
    {have: false},
    {have: true, fresh: true, rssi: -25.4, snr: 12, age: 3, battery: 95},
    {have: false},
  ]);
  const tree = await connect();

  const texts = textsOf(tree);
  expect(texts).toContain('-25.4');
  expect(texts).toContain('dBm');
  // The cat's name moved down to the line under the reading.
  expect(texts.some(t => t.startsWith('Cat 2 ·'))).toBe(true);
  // And the proximity wording it replaced is gone.
  expect(texts).not.toContain('Very close - check the bushes');

  await ReactTestRenderer.act(async () => {
    tree.unmount();
  });
});

test('tapping a row follows that cat instead of the closest one', async () => {
  // Collar 0 is weak, collar 1 is strong. The closest is collar 1, so the
  // reading starts on collar 1.
  mockRoster = rosterBytes([
    {have: true, fresh: true, rssi: -80, snr: 2, age: 1, battery: 40},
    {have: true, fresh: true, rssi: -25.4, snr: 12, age: 1, battery: 95},
    {have: false},
  ]);
  const tree = await connect();

  expect(textsOf(tree)).toContain('-25.4');

  // Follow collar 0 instead.
  await press(tree, 'Cat 1, 74 percent signal');

  const texts = textsOf(tree);
  expect(texts).toContain('-80.0');
  expect(texts).not.toContain('-25.4');
  expect(texts.some(t => t.startsWith('Cat 1 ·'))).toBe(true);
  // The row says which one is being followed.
  expect(texts).toContain('following');

  await ReactTestRenderer.act(async () => {
    tree.unmount();
  });
});

test('tapping the focused row again goes back to the closest one', async () => {
  mockRoster = rosterBytes([
    {have: true, fresh: true, rssi: -80, snr: 2, age: 1, battery: 40},
    {have: true, fresh: true, rssi: -25.4, snr: 12, age: 1, battery: 95},
    {have: false},
  ]);
  const tree = await connect();

  await press(tree, 'Cat 1, 74 percent signal');
  expect(textsOf(tree)).toContain('-80.0');

  await press(tree, 'Cat 1, 74 percent signal');
  expect(textsOf(tree)).toContain('-25.4');

  await ReactTestRenderer.act(async () => {
    tree.unmount();
  });
});

test('the rename button is on the row, not the row itself', async () => {
  mockRoster = rosterBytes([
    {have: true, fresh: true, rssi: -25.4, snr: 12, age: 1, battery: 95},
    {have: false},
    {have: false},
  ]);
  const tree = await connect();

  // Pressing the pencil must not move the reading, and must not focus the
  // row either - it opens the rename dialog.
  const before = textsOf(tree);
  await press(tree, 'Rename Cat 1');

  expect(textsOf(tree)).toContain('-25.4');
  expect(textsOf(tree)).not.toContain('following');
  expect(before).not.toContain('following');

  await ReactTestRenderer.act(async () => {
    tree.unmount();
  });
});
