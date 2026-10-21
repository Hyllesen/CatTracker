/**
 * The big line under the roster.
 *
 * It used to be a proximity phrase ("Very close - check the bushes"). It is
 * now the dBm reading, because the phrase could not be compared against a
 * number from another walk and two people looking at the same screen could
 * not tell whether "getting warmer" was better or worse than last time.
 *
 * The colour still carries the three-way read - green, amber, red - so the
 * glanceable part survived the change.
 *
 * @format
 */

import {Collar, ROSTER_FLAG_HAVE} from '../src/signal';
import {headline} from '../App';

function collar(overrides: Partial<Collar> = {}): Collar {
  return {
    id: 0,
    have: true,
    fresh: false,
    stale: false,
    lowBattery: false,
    rssi: -25.4,
    snr: 12,
    percent: 74,
    ageSeconds: 3,
    batteryPercent: 95,
    ...overrides,
  };
}

test('a collar to follow shows its reading in dBm', () => {
  const h = headline(collar({rssi: -25.4}), 1, true);
  expect(h.reading).toBe(true);
  expect(h.text).toBe('-25.4');
});

test('the reading is coloured by strength, not by the wording it replaced', () => {
  expect(headline(collar({percent: 90}), 1, true).tone).toBe('#2ecc71');
  expect(headline(collar({percent: 50}), 1, true).tone).toBe('#f39c12');
  expect(headline(collar({percent: 10}), 1, true).tone).toBe('#e74c3c');
});

test('with every collar gone quiet it says so instead of a reading', () => {
  const h = headline(null, 2, true);
  expect(h.reading).toBe(false);
  expect(h.text).toBe('All the collars have gone quiet');
});

test('with nothing heard it says what to do', () => {
  expect(headline(null, 0, false).text).toBe('Press Connect to start');
  expect(headline(null, 0, true).text).toBe('Listening for the collars...');
});

test('a collar that has never spoken is not a reading', () => {
  const never = collar({have: false, rssi: null, percent: 0});
  expect(headline(never, 0, true).reading).toBe(false);
  // The flag the firmware sends means the same thing.
  expect(ROSTER_FLAG_HAVE).toBe(0x01);
});
