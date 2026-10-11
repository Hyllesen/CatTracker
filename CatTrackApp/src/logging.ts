/**
 * Range-test recording.
 *
 * Appends one CSV row per received packet to a session file in the app's
 * documents directory, so a walk produces data to analyse rather than a set of
 * numbers to remember. The column layout is the one tools/analyze_walk.py
 * already expects, so an exported file drops straight into that script:
 *
 *     t_ms,rssi_dbm,snr_db,flags,raw
 *
 * flags is a bitfield: 0x01 fresh packet, 0x02 marker, 0x08 receive error.
 * Note this is NOT the same bit layout as the BLE packet's flags byte.
 *
 * Rows are appended to disk as they arrive rather than buffered and written at
 * the end, so a walk survives the app being backgrounded, the screen locking,
 * or the phone dying mid-test.
 */

import {Dirs, FileSystem} from 'react-native-file-access';
import Share from 'react-native-share';

import {Signal} from './signal';

/** Sentinel the analyzer uses for "no packet heard". */
const NO_DATA = -127.0;

const FLAG_FRESH = 0x01;
const FLAG_MARKER = 0x02;

let path: string | null = null;
let rows = 0;
let startMs = 0;

export function isRecording(): boolean {
  return path !== null;
}

export function rowCount(): number {
  return rows;
}

export function csvPath(): string | null {
  return path;
}

export function elapsedSeconds(): number {
  return startMs === 0 ? 0 : Math.round((Date.now() - startMs) / 1000);
}

function timestamp(): string {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  return (
    `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}` +
    `-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`
  );
}

/**
 * Notes about the data, written as comment lines the analyzer skips. Kept here
 * so the caveats travel with the file instead of living only in someone's
 * head.
 */
const HEADER = [
  '# Cat Tracker range test',
  '# Note: rssi_dbm is the handheld SMOOTHED value (moving average). The raw',
  '#       column duplicates it rather than carrying an unsmoothed sample, so',
  '#       the analyzer\'s "raw noise" figure describes the firmware averaging',
  '#       window, not the RF noise floor. Fixing that needs a firmware change',
  '#       to put the unsmoothed reading on the wire.',
  't_ms,rssi_dbm,snr_db,flags,raw',
].join('\n');

export async function startRecording(): Promise<void> {
  if (path !== null) {
    return;
  }
  const target = `${Dirs.DocumentDir}/walk-${timestamp()}.csv`;
  await FileSystem.writeFile(target, `${HEADER}\n`, 'utf8');
  path = target;
  rows = 0;
  startMs = Date.now();
}

export async function stopRecording(): Promise<void> {
  // The file is deliberately left on disk. A walk is often stopped and started
  // again while walking a boundary, and overwriting would lose the first leg.
  path = null;
  startMs = 0;
}

async function append(line: string): Promise<void> {
  if (path === null) {
    return;
  }
  try {
    await FileSystem.appendFile(path, line, 'utf8');
    rows += 1;
  } catch (e) {
    // Losing one row should not interrupt the walk or the audio, but the user
    // needs to know the file is incomplete rather than silently truncated.
    console.warn('log: append failed', e);
  }
}

function formatRow(tMs: number, rssi: number, snr: number, flags: number): string {
  return `${tMs},${rssi.toFixed(1)},${snr.toFixed(1)},${flags},${rssi.toFixed(1)}\n`;
}

/** Record one received packet. Called only for fresh packets. */
export async function addSample(signal: Signal): Promise<void> {
  if (path === null) {
    return;
  }
  const rssi = signal.rssi === null ? NO_DATA : signal.rssi;
  const snr = signal.snr === null ? NO_DATA : signal.snr;
  await append(formatRow(Date.now() - startMs, rssi, snr, FLAG_FRESH));
}

/**
 * Drop a labelled waypoint. The label goes in a comment line so the analyzer
 * still parses the row for timing, while the file itself records what the
 * distance was meant to be.
 */
export async function addMarker(label: string): Promise<void> {
  if (path === null) {
    return;
  }
  const tMs = Date.now() - startMs;
  await append(`# MARKER ${tMs} ${label}\n`);
  await append(formatRow(tMs, NO_DATA, NO_DATA, FLAG_MARKER | FLAG_FRESH));
}

/** Hand the CSV to the Android share sheet. */
export async function exportCsv(): Promise<string> {
  if (path === null) {
    throw new Error('Nothing recorded yet');
  }
  const file = path;
  await Share.open({
    url: `file://${file}`,
    type: 'text/csv',
    filename: file.split('/').pop(),
    failOnCancel: false,
  });
  return file;
}