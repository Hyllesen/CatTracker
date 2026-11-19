/**
 * Find mode - the phone's half.
 *
 * The phone is what turns "I am looking for this cat" into a LoRa command. It
 * cannot do that directly and must not try: the collar only listens for a
 * fraction of a second after each of its own pings, so the handheld has to hold
 * the request and transmit it inside that window. So the phone's whole job is
 * to write three bytes - which collar, start or stop, for how long - and then
 * read the truth back out of the roster:
 *
 *   FIND_REQ     set by the handheld while it is holding the request
 *   FIND_ACTIVE  set by the collar itself, in its pings
 *
 * Everything in this file is pure so the states can be tested without a radio.
 *
 * @format
 */

import {Collar} from './signal';

/**
 * Write characteristic for commands, alongside the roster notify.
 *
 * Must match FIND_UUID in handheld_ble/handheld_ble.ino.
 */
export const FIND_CHAR_UUID = '8e7a1b22-3f4d-4c8e-9a1f-2b6d5e4c3a21';

/** Command bytes, as parsed by collar/collar.ino. */
export const FIND_START = 0x01;
export const FIND_STOP = 0x02;

/** Duration offered by the app; the collar has its own default of 15. */
export const DEFAULT_FIND_MINUTES = 15;

/**
 * How long to keep saying "waiting" before admitting failure.
 *
 * The handheld retries on the next three pings, and a slow ping is a minute
 * apart, so a request that will never be heard takes up to ~3 minutes to give
 * up. Under that it is still worth waiting; over it the cat is out of range and
 * the honest thing to show is "failed".
 */
export const FIND_TIMEOUT_MS = 200000;

export type FindAction = 'start' | 'stop';

/**
 * The three-byte command.
 *
 * Little-endian is irrelevant here; what matters is that the collar's parser
 * reads [id, command, minutes] in that order and that a stray fourth byte is
 * ignored, so this can never be mistaken for a ping (0xC1) even if the BLE
 * layer pads it.
 */
export function encodeFindCommand(
  id: number,
  action: FindAction,
  minutes: number = DEFAULT_FIND_MINUTES,
): Uint8Array {
  const clamped = Math.max(0, Math.min(255, Math.round(minutes)));
  return new Uint8Array([
    id & 0xff,
    action === 'start' ? FIND_START : FIND_STOP,
    action === 'start' ? clamped : 0,
  ]);
}

/** A request the app made, so it can say "waiting" and later "failed". */
export type FindRequest = {
  action: FindAction;
  minutes: number;
  /** When the app wrote the command. */
  at: number;
};

export type FindKind = 'idle' | 'waiting' | 'active' | 'stopping' | 'failed';

export type FindStatus = {
  kind: FindKind;
  /** Short text for the row. Empty when there is nothing to say. */
  label: string;
  /** True when row-level emphasis (colour) is warranted. */
  busy: boolean;
};

/**
 * What this collar's find state should read as.
 *
 * `activeSince` is when the app first saw FIND_ACTIVE - the countdown is
 * derived from the duration the owner asked for, not from a wire byte, because
 * a full 20-byte roster has no room for one and the app already knows the
 * answer.
 */
export function findStatus(
  collar: Collar | null,
  request: FindRequest | undefined,
  now: number,
  activeSince: number | null = null,
): FindStatus {
  if (!collar) {
    return {kind: 'idle', label: '', busy: false};
  }

  // A stop that is in flight outranks the collar's last report. The roster
  // carries the flags of the last ping *received*, so for up to one slow
  // interval after a stop is queued it still says "active" - and a screen that
  // answers a tap with "still finding" is worse than one that says "stopping".
  if (collar.findRequested && request?.action === 'stop') {
    return {kind: 'stopping', label: 'stopping', busy: true};
  }

  if (collar.findActive) {
    if (!request || request.action !== 'start') {
      // Active without a local request: another phone (or a serial session)
      // started it. Say so without inventing a countdown.
      return {kind: 'active', label: 'finding', busy: true};
    }
    const elapsed = activeSince === null ? 0 : now - activeSince;
    const left = Math.max(0, Math.ceil((request.minutes * 60000 - elapsed) / 60000));
    // The collar reverts by itself when the duration runs out, and the roster
    // keeps reporting the last ping's flags until the next slow ping arrives -
    // so "ending" is the honest word for that minute, not "finding · 0 min".
    if (left === 0) {
      return {kind: 'active', label: 'ending', busy: true};
    }
    return {kind: 'active', label: `finding · ${left} min`, busy: true};
  }

  if (collar.findRequested) {
    return {kind: 'waiting', label: 'waiting for ping', busy: true};
  }

  if (request && now - request.at > FIND_TIMEOUT_MS) {
    return request.action === 'start'
      ? {kind: 'failed', label: 'find failed', busy: false}
      : {kind: 'idle', label: '', busy: false};
  }

  if (request) {
    // Written but not yet picked up by the handheld's roster.
    return request.action === 'stop'
      ? {kind: 'stopping', label: 'stopping', busy: true}
      : {kind: 'waiting', label: 'waiting for ping', busy: true};
  }

  return {kind: 'idle', label: '', busy: false};
}

/**
 * What the row's button should say, and what pressing it will do.
 *
 * The button is a toggle on purpose: one cat, one control, and pressing it
 * again asks that collar to stop - which is the thing an owner wants when they
 * have found the cat.
 */
export function findToggle(
  collar: Collar | null,
  request: FindRequest | undefined,
  now: number,
  activeSince: number | null = null,
): {action: FindAction; label: string; kind: FindKind} {
  const status = findStatus(collar, request, now, activeSince);
  if (status.kind === 'active' || status.kind === 'waiting' || status.kind === 'stopping') {
    return {action: 'stop', label: 'stop', kind: status.kind};
  }
  return {action: 'start', label: 'find', kind: status.kind};
}
