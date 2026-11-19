/**
 * The wire format is a contract between three files that nothing else ties
 * together: the collar firmware builds the frame, the handheld firmware
 * parses it and republishes it, and the app decodes what the handheld
 * publishes. There is no compiler that sees all three, and no type checker
 * that sees any of them.
 *
 * So the constants are compared here, by reading the firmware sources. A
 * mismatch is a bug that would otherwise show up as "the app shows two cats
 * when there are three" or "the app shows nothing at all", days later, with
 * no clue which end broke.
 *
 * @format
 */

import fs from 'fs';
import path from 'path';

import {
  MAX_COLLARS,
  ROSTER_ENTRY_LEN,
  ROSTER_FLAG_FIND_ACTIVE,
  ROSTER_FLAG_FIND_REQ,
  ROSTER_FLAG_FRESH,
  ROSTER_FLAG_HAVE,
  ROSTER_FLAG_LOW_BATT,
  ROSTER_FLAG_STALE,
  ROSTER_VERSION,
} from '../src/signal';
import {FIND_CHAR_UUID, FIND_START, FIND_STOP} from '../src/find';
import {BATTERY_SAMPLE_MINUTES} from '../src/signal';

const ROOT = path.join(__dirname, '..', '..');

function source(file: string): string {
  return fs.readFileSync(path.join(ROOT, file), 'utf8');
}

/** Read a `const uint8_t NAME = 0xNN;` out of a firmware source. */
function constant(src: string, name: string): number {
  const match = src.match(
    new RegExp(`const\\s+(?:uint8_t|uint16_t|uint32_t)\\s+${name}\\s*=\\s*(0x[0-9a-fA-F]+|\\d+)`),
  );
  if (!match) {
    throw new Error(`${name} not found in the firmware source`);
  }
  return Number(match[1]);
}

const collar = source('collar/collar.ino');
const handheld = source('handheld_ble/handheld_ble.ino');

test('the collar frame magic matches the handheld parser', () => {
  expect(constant(collar, 'FRAME_MAGIC')).toBe(
    constant(handheld, 'FRAME_MAGIC'),
  );
});

test('the collar frame length matches the handheld parser', () => {
  expect(constant(collar, 'FRAME_LEN')).toBe(
    constant(handheld, 'FRAME_LEN'),
  );
});

test('the roster version matches the app decoder', () => {
  expect(constant(handheld, 'ROSTER_VERSION')).toBe(ROSTER_VERSION);
});

test('the roster entry length matches the app decoder', () => {
  expect(constant(handheld, 'ROSTER_ENTRY_LEN')).toBe(ROSTER_ENTRY_LEN);
});

test('the roster flags match the app decoder', () => {
  expect(constant(handheld, 'ROSTER_FLAG_HAVE')).toBe(ROSTER_FLAG_HAVE);
  expect(constant(handheld, 'ROSTER_FLAG_FRESH')).toBe(ROSTER_FLAG_FRESH);
  expect(constant(handheld, 'ROSTER_FLAG_STALE')).toBe(ROSTER_FLAG_STALE);
  expect(constant(handheld, 'ROSTER_FLAG_LOW_BATT')).toBe(ROSTER_FLAG_LOW_BATT);
});

test('every firmware agrees on how many collars there are', () => {
  // MAX_COLLARS is a #define in the collar and a const in the handheld.
  const collarDefines = collar.match(/#define\s+MAX_COLLARS\s+(\d+)/);
  expect(collarDefines).not.toBeNull();
  expect(Number(collarDefines![1])).toBe(MAX_COLLARS);
  expect(constant(handheld, 'MAX_COLLARS')).toBe(MAX_COLLARS);
});

test('the collar writes the id where the handheld reads it', () => {
  // The single most dangerous field to get wrong: an id in the wrong byte
  // merges two cats into one row, or hides one of them entirely.
  expect(collar).toMatch(/out\[2\]\s*=\s*COLLAR_ID/);
  expect(handheld).toMatch(/id\s*=\s*buf\[2\]/);
});

test('the collar and the handheld agree on the frame byte order', () => {
  const writes = collar.match(/out\[(\d)\]\s*=\s*([^;]+);/g) ?? [];
  const reads = handheld.match(/(\w+)\s*=\s*buf\[(\d)\]/g) ?? [];

  // seq, id, battery and flags all have to sit in the same byte on both
  // sides.
  const writeAt = (byte: string) => {
    const hit = writes.find(w => w.startsWith(`out[${byte}]`));
    return hit ? hit.split('=')[1].trim() : '';
  };
  const readAt = (byte: string) => {
    const hit = reads.find(r => r.endsWith(`buf[${byte}]`));
    return hit ? hit.split('=')[0].trim() : '';
  };

  expect(writeAt('1')).toContain('seq');
  expect(readAt('1')).toBe('seq');
  expect(writeAt('3')).toContain('battery');
  expect(readAt('3')).toBe('batt');
  expect(writeAt('4')).toContain('LOW_BATT');
  expect(readAt('4')).toBe('flags');
});

test('the roster the handheld publishes fits the default ATT payload', () => {
  // NimBLE truncates a notification to MTU-3 rather than failing, so a
  // roster that is too long reaches the phone mangled and the app shows
  // nothing at all. The default MTU is 23, which leaves 20 bytes.
  const bytes = 2 + MAX_COLLARS * ROSTER_ENTRY_LEN;
  expect(bytes).toBeLessThanOrEqual(20);
});

test('the handheld writes the roster entry in the order the app reads it', () => {
  // flags, rssi low, rssi high, snr, age, battery - six bytes, in that
  // order. The app's decoder reads the same six fields at the same
  // offsets, so the two lists have to stay in step.
  const writes = handheld.match(/\*p\+\+\s*=\s*([^;]+);/g) ?? [];
  const fields = writes.map(w => w.replace(/\*p\+\+\s*=\s*/, '').trim());

  expect(fields).toHaveLength(ROSTER_ENTRY_LEN);
  expect(fields[0]).toContain('flags');
  expect(fields[1]).toContain('r10');
  expect(fields[1]).toContain('0xFF');
  expect(fields[2]).toContain('r10');
  expect(fields[2]).toContain('>> 8');
  expect(fields[3]).toContain('s2');
  expect(fields[4]).toContain('ageS');
  expect(fields[5]).toContain('batteryPct');
});

test('the app reads the roster entry at the offsets the handheld writes', () => {
  const app = source('CatTrackApp/src/signal.ts');
  expect(app).toMatch(/const o = 2 \+ i \* ROSTER_ENTRY_LEN/);
  expect(app).toMatch(/const flags = bytes\[o\]/);
  expect(app).toMatch(/toInt16\(bytes, o \+ 1\)/);
  expect(app).toMatch(/toInt8\(bytes\[o \+ 3\]\)/);
  expect(app).toMatch(/bytes\[o \+ 4\]/);
  expect(app).toMatch(/bytes\[o \+ 5\]/);
});

/** Read a `#define NAME 123` out of a firmware source. */
function defineNumber(src: string, name: string): number {
  const match = src.match(new RegExp(`#define\\s+${name}\\s+(\\d+)`));
  if (!match) {
    throw new Error(`${name} not found in the firmware source`);
  }
  return Number(match[1]);
}

/** Read a `#define NAME "value"` out of a firmware source. */
function defineString(src: string, name: string): string {
  const match = src.match(new RegExp(`#define\\s+${name}\\s+"([^"]+)"`));
  if (!match) {
    throw new Error(`${name} not found in the firmware source`);
  }
  return match[1];
}

// ---------------------------------------------------------------------------
// Find mode: the newest part of the contract, and the one with the most ends
// ---------------------------------------------------------------------------
// A find command is written by the phone, held by the handheld, transmitted in
// a window the collar controls, and acknowledged in a ping. Four files that no
// compiler sees together.

test('the find flags the app decodes are the ones the handheld sets', () => {
  // The collar's frame flag and the roster flag are different bits in
  // different bytes: the frame is byte 4 bit 1, the roster is byte 0 bit 5.
  expect(constant(collar, 'FLAG_FIND')).toBe(0x02);
  expect(constant(handheld, 'FRAME_FLAG_FIND')).toBe(0x02);
  expect(ROSTER_FLAG_FIND_REQ).toBe(0x10);
  expect(ROSTER_FLAG_FIND_ACTIVE).toBe(0x20);
  // And they must not collide with the four bits that were already taken.
  for (const taken of [
    ROSTER_FLAG_HAVE,
    ROSTER_FLAG_FRESH,
    ROSTER_FLAG_STALE,
    ROSTER_FLAG_LOW_BATT,
  ]) {
    expect(taken & ROSTER_FLAG_FIND_REQ).toBe(0);
    expect(taken & ROSTER_FLAG_FIND_ACTIVE).toBe(0);
  }
});

test('the collar and the handheld agree on the command frame', () => {
  expect(constant(collar, 'CMD_MAGIC')).toBe(0xc2);
  expect(constant(handheld, 'CMD_MAGIC')).toBe(0xc2);
  expect(constant(collar, 'CMD_LEN')).toBe(6);
  expect(constant(handheld, 'CMD_LEN')).toBe(6);
  expect(constant(collar, 'CMD_FIND_START')).toBe(FIND_START);
  expect(constant(collar, 'CMD_FIND_STOP')).toBe(FIND_STOP);
  // A command must never be mistakable for a ping.
  expect(constant(collar, 'CMD_MAGIC')).not.toBe(constant(collar, 'FRAME_MAGIC'));
});

test('the find write characteristic is the same UUID on both sides', () => {
  expect(defineString(handheld, 'FIND_UUID')).toBe(FIND_CHAR_UUID);
  expect(FIND_CHAR_UUID).not.toBe(defineString(handheld, 'SIGNAL_UUID'));
});

test("the handheld's receive window covers the collar's slow interval", () => {
  // The collar is the one that decides how quiet it goes, and a handheld window
  // shorter than that is deaf for most of every cycle - which looks exactly
  // like a dead collar.
  const slowMs = 1000 * defineNumber(collar, 'SLOW_INTERVAL_S');
  expect(constant(handheld, 'COLLAR_SLOW_MS')).toBe(slowMs);
  // Both windows are written as expressions built from that number.
  expect(handheld).toMatch(/COLLAR_PING_MS\s*=\s*COLLAR_SLOW_MS\s*\+/);
  expect(handheld).toMatch(/RX_TIMEOUT_MS\s*=\s*COLLAR_PING_MS\s*\+/);
  // STALE_MS is written as an expression rather than a literal, so compare the
  // expression: it has to be derived from the same number, not typed in.
  expect(handheld).toMatch(/STALE_MS\s*=\s*\(\s*COLLAR_SLOW_MS\s*\*\s*5\s*\)\s*\/\s*2/);
});

test('the battery figure the app labels as slow really is slow', () => {
  // The app tells the owner "10-min sample" next to the percentage. That is only
  // honest while the collar keeps BATT_SAMPLE_MS at 600000; a DEV_MODE build
  // samples every 5 s, and a future build may change it again.
  expect(collar).toContain('const uint32_t BATT_SAMPLE_MS = 600000;');
  expect(BATTERY_SAMPLE_MINUTES).toBe(600000 / 60000);
});
