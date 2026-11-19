/**
 * Find mode: the phone's half of the protocol.
 *
 * The command itself is three bytes, but the interesting part is what the owner
 * sees while those bytes travel: the phone can only ask, and the collar only
 * answers in the window after its next ping. The states here are the ones that
 * make that honest - "waiting for ping" is not "finding", and a request that
 * was never heard has to end up saying "find failed" rather than spinning
 * forever.
 *
 * @format
 */

import {
  DEFAULT_FIND_MINUTES,
  FIND_STOP,
  FIND_TIMEOUT_MS,
  FIND_START,
  encodeFindCommand,
  findStatus,
  findToggle,
} from '../src/find';
import {Collar} from '../src/signal';

function collar(overrides: Partial<Collar> = {}): Collar {
  return {
    id: 2,
    have: true,
    fresh: true,
    stale: false,
    lowBattery: false,
    findRequested: false,
    findActive: false,
    rssi: -80,
    snr: 10,
    percent: 60,
    ageSeconds: 2,
    batteryPercent: 80,
    ...overrides,
  };
}

test('the command is id, action, minutes - nothing else', () => {
  expect(Array.from(encodeFindCommand(2, 'start'))).toEqual([
    2,
    FIND_START,
    DEFAULT_FIND_MINUTES,
  ]);
  expect(Array.from(encodeFindCommand(0, 'stop'))).toEqual([0, FIND_STOP, 0]);
  // Clamped, because a byte is all the collar reads and 999 would arrive as
  // 231 minutes of a cat being told to shout.
  expect(encodeFindCommand(1, 'start', 999)[2]).toBe(255);
  expect(encodeFindCommand(1, 'start', -5)[2]).toBe(0);
});

test('idle when nothing has been asked for', () => {
  const s = findStatus(collar(), undefined, 1000);
  expect(s.kind).toBe('idle');
  expect(s.label).toBe('');
  expect(findToggle(collar(), undefined, 1000).action).toBe('start');
});

test('requested but not yet delivered says exactly that', () => {
  const c = collar({findRequested: true});
  const s = findStatus(c, {action: 'start', minutes: 15, at: 1000}, 5000);
  expect(s.kind).toBe('waiting');
  expect(s.label).toBe('waiting for ping');
  // The button must offer "stop" - an owner who changed their mind should not
  // have to wait for the ping before they can cancel.
  expect(findToggle(c, {action: 'start', minutes: 15, at: 1000}, 5000)).toEqual({
    action: 'stop',
    label: 'stop',
    kind: 'waiting',
  });
});

test('active counts the asked-for duration down', () => {
  const c = collar({findActive: true});
  const request = {action: 'start' as const, minutes: 15, at: 0};
  expect(findStatus(c, request, 60_000, 60_000).label).toBe('finding · 15 min');
  expect(findStatus(c, request, 60_000 + 6 * 60_000, 60_000).label).toBe(
    'finding · 9 min',
  );
  // Never negative, and never a stale "finding": once the duration is used up
  // the collar reverts on its own, and the roster keeps repeating the last
  // ping's flags until the next slow ping arrives a minute later.
  expect(findStatus(c, request, 60_000 + 60 * 60_000, 60_000).label).toBe(
    'ending',
  );
});

test('active without a local request does not invent a countdown', () => {
  const s = findStatus(collar({findActive: true}), undefined, 1000);
  expect(s.kind).toBe('active');
  expect(s.label).toBe('finding');
});

test('a request that is never heard ends as failed, not as waiting forever', () => {
  const request = {action: 'start' as const, minutes: 15, at: 1000};
  expect(findStatus(collar(), request, 1000 + FIND_TIMEOUT_MS - 1).kind).toBe(
    'waiting',
  );
  const late = findStatus(collar(), request, 1000 + FIND_TIMEOUT_MS + 1);
  expect(late.kind).toBe('failed');
  expect(late.label).toBe('find failed');
  // A failed start offers "find" again, so retrying is one tap.
  expect(
    findToggle(collar(), request, 1000 + FIND_TIMEOUT_MS + 1).action,
  ).toBe('start');
});

test('stop in flight reads as stopping, not as finding', () => {
  const c = collar({findRequested: true});
  const request = {action: 'stop' as const, minutes: 15, at: 0};
  expect(findStatus(c, request, 10).label).toBe('stopping');
  expect(findToggle(c, request, 10).action).toBe('stop');
});

test('a queued stop outranks the collar still reporting active', () => {
  // The roster carries the flags of the last ping received, so right after a
  // stop is queued it still says FIND_ACTIVE. Answering the tap with "still
  // finding" would read as the command having been ignored.
  const c = collar({findActive: true, findRequested: true});
  const request = {action: 'stop' as const, minutes: 15, at: 0};
  expect(findStatus(c, request, 10).label).toBe('stopping');
});
