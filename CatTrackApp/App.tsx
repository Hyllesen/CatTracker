/**
 * Cat Tracker - handheld signal display.
 *
 * Shows the signal strength broadcast over BLE by the ESP32-C3 handheld and
 * plays a discrete tone for it, so you can walk toward the cat without looking
 * at the screen. The tone is the primary signal while tracking; the readout is
 * there for when you stop and want the detail.
 *
 * Several collars at once: the handheld publishes a roster - one row per cat
 * - and this screen shows them all, so "which one ran off" is answered by a
 * glance rather than by walking to each cat in turn.
 */

import React, {useCallback, useEffect, useRef, useState} from 'react';
import {
  ActivityIndicator,
  Modal,
  Permission,
  PermissionsAndroid,
  Platform,
  Pressable,
  StatusBar,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import {Device, Subscription} from 'react-native-ble-plx';
import {
  SafeAreaProvider,
  useSafeAreaInsets,
} from 'react-native-safe-area-context';

import {
  connectAndSubscribe,
  disconnect,
  isBluetoothEnabled,
  ScanResult,
  scanForHandheld,
  sendFindCommand,
} from './src/ble';
import {
  BATTERY_SAMPLE_MINUTES,
  Collar,
  MAX_COLLARS,
  Roster,
  pickLeader,
  snrColor,
  toneStepFor,
} from './src/signal';
import {CatNames, defaultName, loadNames, nameFor, saveName} from './src/cats';
import {
  DEFAULT_FIND_MINUTES,
  FindAction,
  FindKind,
  FindRequest,
  findStatus,
  findToggle,
} from './src/find';
import {
  playToneStep,
  prepareTones,
  releaseTones,
} from './src/tone';

/** Preset distance waypoints. Labels are written into the CSV next to each
 *  marker so the exported file is self-describing on its own. */

/**
 * Android 12+ grants BLE scanning via BLUETOOTH_SCAN, which the manifest
 * declares with neverForLocation so location is not required. Older releases
 * still gate BLE on location, so request it only where it is actually needed.
 */
function requestPermissions(): Promise<boolean> {
  if (Platform.OS !== 'android') {
    return Promise.resolve(true);
  }

  const needsLocation = Platform.Version < 31;
  const wanted: Permission[] = [
    PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
    PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
    ...(needsLocation ? [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION] : []),
  ];

  return PermissionsAndroid.requestMultiple(wanted).then(result => {
    return wanted.every(perm => result[perm] === PermissionsAndroid.RESULTS.GRANTED);
  });
}

// Green when close, amber in the middle, red when weak. Deliberately not a
// rainbow - two hues plus neutral reads faster than many.
function signalColor(percent: number): string {
  if (percent >= 66) {
    return '#2ecc71';
  }
  if (percent >= 33) {
    return '#f39c12';
  }
  return '#e74c3c';
}

/**
 * The big line under the roster.
 *
 * When there is a collar to follow, this is its signal level in dBm - the
 * number the radio actually measured, for the packet that just arrived. It
 * used to be a proximity phrase ("Very close - check the bushes") instead,
 * which says less: the phrase cannot be compared against a reading from
 * another walk, and two people looking at the same screen cannot tell whether
 * "getting warmer" means better or worse than last time.
 *
 * The dBm is the largest type on the screen and it belongs to the cat being
 * followed; every row carries the same two numbers in a smaller size, so the
 * roster can be compared at a glance and the big one says where to walk.
 *
 * The colour still carries the same three-way read - green, amber, red - so
 * the glanceable part survives the change.
 *
 * When there is nothing to show, this falls back to saying why.
 */
export function headline(
  collar: Collar | null,
  heard: number,
  connected: boolean,
): {text: string; snr: number | null; tone: string; reading: boolean} {
  if (collar && collar.rssi !== null) {
    return {
      text: collar.rssi.toFixed(1),
      snr: collar.snr,
      // A collar that has gone quiet turns the reading red whatever the last
      // value was. The number is still the last thing the radio heard, but
      // it is not a live reading any more, and a frozen green number would
      // say "you are there" when the cat has walked off.
      tone: collar.stale ? '#e74c3c' : signalColor(collar.percent),
      reading: true,
    };
  }
  if (heard > 0) {
    // Every cat we have heard from has gone quiet. That is a different
    // thing from "nothing heard yet", and it is the situation that
    // matters: a collar that has stopped answering is a cat that has
    // stopped answering.
    return {
      text: 'All the collars have gone quiet',
      snr: null,
      tone: '#e74c3c',
      reading: false,
    };
  }
  if (!connected) {
    return {text: 'Press Connect to start', snr: null, tone: '#7f8c8d', reading: false};
  }
  return {
    text: 'Listening for the collars...',
    snr: null,
    tone: '#7f8c8d',
    reading: false,
  };
}

/** Fallback beacon period before any have been observed, matches the collar's
 *  PING_INTERVAL_MS. Replaced by the measured cadence once two beeps land. */
const DEFAULT_BEEP_MS = 8000;
/** How long the indicator stays lit after a beep. */
const PULSE_MS = 500;
/** Intervals kept for the median. A median rather than a mean because the
 *  occasional missed beacon doubles one sample and would skew an average. */
const INTERVAL_SAMPLES = 7;

/**
 * Silence after which the BLE link is presumed dead and reconnected.
 *
 * This is measured against ANY roster notification, keep-alive included - not
 * against a collar packet. The handheld notifies every 2 s whether or not a
 * cat has pinged, while a collar's slow interval is 60 s (SLOW_INTERVAL_S).
 * Keying this off FRESH packets instead would disconnect a perfectly healthy
 * link 25 s after every beacon, which is the bug this constant used to have.
 *
 * Twelve missed keep-alives is far beyond jitter, and still short enough that
 * a real drop is noticed while the user is standing there.
 */
const STALL_RECONNECT_MS = 25000;

/**
 * How many scans an automatic reconnect tries before giving up.
 *
 * A handheld that was reset or walked out of range takes a moment to
 * advertise again, and the first scan can start in that gap. Manual Connect
 * still tries once, so a bad glue code or a phone holding the link reports
 * its reason immediately rather than after a minute of silence.
 */
const RECONNECT_SCANS = 3;
/** Gap between reconnect scans, so the first one can overlap the handheld's
 *  own re-advertising rather than racing it. */
const RECONNECT_GAP_MS = 2000;

function median(values: number[]): number {
  if (values.length === 0) {
    return 0;
  }
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

/** Beacon timing, kept per collar so each cat's own cadence is measured. */
type Timing = {last: number; intervals: number[]};

/**
 * Shows how long since the last beacon, and how long until the next one is due.
 *
 * The tone alone cannot distinguish "did not hear it" from "there was nothing
 * to hear", which is exactly the ambiguity that made a quiet tone look like a
 * broken link. The bar draining makes a missing beacon obvious instead.
 */
function BeepIndicator({
  elapsedMs,
  expectedMs,
  pulse,
}: {
  elapsedMs: number | null;
  expectedMs: number;
  pulse: boolean;
}) {
  if (elapsedMs === null) {
    return <Text style={styles.beepWaiting}>waiting for the first beacon...</Text>;
  }
  const remain = Math.max(0, expectedMs - elapsedMs);
  // Two missed beacons before flagging as late; the cadence is not exact and a
  // single dropped packet is normal, not a fault worth alarming about.
  const late = elapsedMs > expectedMs * 2;
  const fill = Math.min(100, (elapsedMs / expectedMs) * 100);

  return (
    <View style={styles.beepBox}>
      <View style={styles.beepRow}>
        <View style={[styles.beepDot, pulse ? styles.beepDotLit : null]} />
        <Text style={styles.beepText}>beep {(elapsedMs / 1000).toFixed(1)}s ago</Text>
        <Text style={[styles.beepNext, late ? styles.beepLate : null]}>
          {late ? 'no beacon' : `next in ${(remain / 1000).toFixed(1)}s`}
        </Text>
      </View>
      <View style={styles.beepTrack}>
        <View
          style={[styles.beepFill, late ? styles.beepFillLate : null, {width: `${fill}%`}]}
        />
      </View>
    </View>
  );
}

/**
 * One cat: name, the two radio numbers, and the details worth reading when you
 * stop.
 *
 * dBm and SNR sit side by side because they answer different questions about
 * the same packet - how much signal arrived, and how cleanly it could be
 * demodulated - and neither can be inferred from the other. There is no
 * percentage and no bar: a 0-100 scale has no meaning to compare against a
 * datasheet or another walk, and two numbers large enough to read say more
 * than a bar that has to be squinted at anyway.
 *
 * Tapping the row follows this cat - the big reading above becomes its
 * signal, and the tone follows it too, so the beep and the display always
 * agree on which cat you are walking towards. Tap it again to go back to
 * following the closest one.
 *
 * Renaming lives on the pencil at the end of the row, because a name is not
 * something you change often and following a cat is something you do
 * constantly - and a tap that meant two different things depending on how
 * long you held it would be a trap.
 */
function CollarRow({
  collar,
  name,
  isLeader,
  isFocused,
  onPress,
  onRename,
  findLabel,
  findAction,
  findKind,
  onToggleFind,
}: {
  collar: Collar;
  name: string;
  isLeader: boolean;
  isFocused: boolean;
  onPress: () => void;
  onRename: () => void;
  findLabel: string;
  findAction: FindAction;
  findKind: FindKind;
  onToggleFind: () => void;
}) {
  // Same rule as the big reading: a lost collar goes red whatever its last
  // signal was, because a green number would say "the cat is here" when the
  // cat stopped answering.
  const tone = collar.stale ? '#e74c3c' : signalColor(collar.percent);
  // The SNR keeps its own colour: it is a reading of the same packet, not a
  // copy of the dBm one, and a strong-but-dirty signal is a real state where
  // the two disagree.
  const snrTone = collar.stale ? '#5d6d7e' : snrColor(collar.snr);

  return (
    <Pressable
      style={[styles.row, isFocused ? styles.rowFocused : null]}
      onPress={onPress}
      accessibilityRole="button"
      accessibilityLabel={
        collar.rssi === null
          ? `${name}, no reading`
          : `${name}, ${collar.rssi.toFixed(1)} dBm`
      }>
      <View style={styles.rowHead}>
        <Text style={[styles.rowName, isFocused ? styles.rowNameFocused : null]}>
          {name}
        </Text>
        {isFocused ? <Text style={styles.tagFocused}>following</Text> : null}
        {isLeader && !isFocused ? (
          <Text style={styles.tagLeader}>closest</Text>
        ) : null}
        {collar.stale ? <Text style={styles.tagLost}>lost</Text> : null}
        {collar.lowBattery ? (
          <Text style={styles.tagLowBatt}>low battery</Text>
        ) : null}
        {findLabel ? (
          <Text
            style={[
              styles.tagFind,
              findKind === 'failed' ? styles.tagLost : null,
            ]}>
            {findLabel}
          </Text>
        ) : null}
        {/* Per-collar, like everything else in this row: turning find mode on
            for one cat must not touch the others, and pressing again turns it
            off for that cat alone. */}
        <Pressable
          style={[
            styles.findButton,
            findAction === 'stop' ? styles.findButtonStop : null,
          ]}
          onPress={onToggleFind}
          accessibilityRole="button"
          accessibilityLabel={
            findAction === 'stop'
              ? `Stop find mode for ${name}`
              : `Start find mode for ${name}`
          }
          hitSlop={{top: 8, bottom: 8, left: 8, right: 8}}>
          <Text
            style={[
              styles.findButtonText,
              findAction === 'stop' ? styles.findButtonTextStop : null,
            ]}>
            {findAction === 'stop' ? 'stop' : 'find'}
          </Text>
        </Pressable>
        <Pressable
          style={styles.editButton}
          onPress={onRename}
          accessibilityRole="button"
          accessibilityLabel={`Rename ${name}`}
          hitSlop={{top: 10, bottom: 10, left: 10, right: 10}}>
          <Text style={styles.editIcon}>✎</Text>
        </Pressable>
      </View>
      <View style={styles.rowReadings}>
        <Text style={[styles.rowDbm, {color: tone}]}>
          {collar.rssi === null ? '--' : collar.rssi.toFixed(1)}
        </Text>
        <Text style={styles.rowDbmUnit}>dBm</Text>
        <Text style={[styles.rowSnr, {color: snrTone}]}>
          {collar.snr === null ? '--' : collar.snr.toFixed(1)}
        </Text>
        <Text style={styles.rowSnrUnit}>dB snr</Text>
        {/* The age belongs with the readings it ages: it is the third number
            about the same packet, not a footnote about the battery. */}
        {collar.have ? (
          <Text style={styles.rowAge}>{collar.ageSeconds}s ago</Text>
        ) : null}
      </View>
      {/* The battery gets its own line, and says how stale it can be: the
          collar samples the cell every ten minutes and every ping carries that
          sample, so this number is always older than the one above it. */}
      {collar.have ? (
        <Text style={styles.rowDetail}>
          <Text style={styles.rowBatt}>
            {collar.batteryPercent === null
              ? 'batt --'
              : `batt ${collar.batteryPercent}%`}
          </Text>
          <Text style={styles.rowBattNote}>
            {collar.batteryPercent === null
              ? '  no divider fitted'
              : `  ${BATTERY_SAMPLE_MINUTES}-min sample`}
          </Text>
        </Text>
      ) : (
        <Text style={styles.rowDetail}>not heard yet</Text>
      )}
    </Pressable>
  );
}

function Tracker() {
  const insets = useSafeAreaInsets();
  const [status, setStatus] = useState<string>('idle');
  const [collars, setCollars] = useState<Collar[]>([]);
  const [names, setNames] = useState<CatNames>({});
  const [error, setError] = useState<string | null>(null);
  const [seen, setSeen] = useState<ScanResult['seen']>([]);
  const [renamingId, setRenamingId] = useState<number | null>(null);
  // Find requests this app made, per collar. Only used for the "waiting" and
  // "failed" wording and for the countdown: the authority on what a collar is
  // actually doing is the roster's FIND_REQ / FIND_ACTIVE bits, which come back
  // from the handheld and the collar itself.
  const [findReqs, setFindReqs] = useState<Record<number, FindRequest>>({});
  // When FIND_ACTIVE was first seen for each collar, so the app can count the
  // asked-for duration down without a wire byte for it.
  const findActiveSinceRef = useRef<Record<number, number>>({});
  const [draftName, setDraftName] = useState('');
  // Which cat the app is following. Null means "whichever is closest", which
  // is what you want while walking around; tapping a row pins it to that cat
  // for when you know which one you are going after.
  const [focusId, setFocusId] = useState<number | null>(null);
  // The BLE callback is set up once, at connect time, so it cannot see the
  // focus through the state - it would keep the value from when it was
  // connected. The ref is the live one; the state is only there to trigger a
  // redraw.
  const focusRef = useRef<number | null>(null);
  const setFocus = useCallback((id: number | null) => {
    focusRef.current = id;
    setFocusId(id);
  }, []);
  const deviceRef = useRef<Device | null>(null);
  const subRef = useRef<Subscription | null>(null);
  /**
   * Bumped whenever a connection attempt starts, and by Disconnect.
   *
   * A scan waits eight seconds, so without this a tap on Connect followed by
   * Disconnect still connects when the scan returns - and the user has to
   * disconnect twice. Every await in the connect path checks it, and an
   * attempt that has been overtaken tears its own device down.
   */
  const sessionRef = useRef(0);
  /** True while an automatic reconnect is running, so the watchdog cannot
   *  start a second one while the first is still scanning. */
  const reconnectingRef = useRef(false);
  /**
   * True between Disconnect and the next connection attempt. A notification
   * already in flight when the user pressed Disconnect can still reach
   * onRoster after the subscription was removed; without this it would put
   * the status back to 'ready' on a link the user just closed.
   */
  const stoppedRef = useRef(false);
  // Beacon timing, per collar so each cat's own cadence is measured rather
  // than one average over all of them. Kept in refs so the BLE callback
  // never needs re-binding, and so reading them does not re-render on every
  // packet.
  const timingRef = useRef<Record<number, Timing>>({});
  /** Last fresh collar packet, for the beep pulse and the cadence display. */
  const lastPacketRef = useRef<number>(0);
  /**
   * Last roster notification of any kind, keep-alive included. This is the
   * liveness clock for the link: 2 s of keep-alive versus a 60 s collar slow
   * interval, so it is the only one of the two that can tell "the link is
   * gone" from "no cat has pinged yet".
   */
  const lastLinkRef = useRef<number>(0);
  const stallRef = useRef(false);
  // Ticks every 200 ms while connected. It is the clock the countdown reads,
  // so the beacon timer does not depend on when React happens to re-render.
  const [now, setNow] = useState(() => Date.now());
  const [pulse, setPulse] = useState(false);

  /** Drop the current subscription and device without touching the UI. */
  const dropLink = useCallback(async () => {
    if (subRef.current) {
      try {
        subRef.current.remove();
      } catch {
        // Already gone with the link.
      }
      subRef.current = null;
    }
    const dev = deviceRef.current;
    deviceRef.current = null;
    if (dev) {
      await disconnect(dev);
    }
  }, []);

  const stop = useCallback(async () => {
    // Abandon any scan or connect that is still in flight, so a Connect tap
    // followed by Disconnect does not connect behind the user's back.
    sessionRef.current += 1;
    stoppedRef.current = true;
    await dropLink();
    timingRef.current = {};
    lastPacketRef.current = 0;
    lastLinkRef.current = 0;
    stallRef.current = false;
    findActiveSinceRef.current = {};
    setFindReqs({});
    setCollars([]);
    setFocus(null);
    setNow(Date.now());
    setStatus('idle');
  }, [dropLink, setFocus]);

  const onRoster = useCallback((r: Roster) => {
    if (stoppedRef.current) {
      // A notification that was already in flight when Disconnect was pressed.
      return;
    }
    // Every notification proves the link is alive, and the handheld sends one
    // every two seconds even with no collars in range. Take the liveness clock
    // here, before the fresh-packet early return below, or a quiet collar
    // looks exactly like a dead connection.
    lastLinkRef.current = Date.now();
    stallRef.current = false;
    setCollars(r.collars);
    setStatus('ready');

    // Start the countdown when a collar first says it is in find mode, and drop
    // it when the collar stops - which is how "finding · 3 min" becomes
    // "finding · 2 min" without the collar sending a minutes byte.
    const seenAt = Date.now();
    for (const c of r.collars) {
      if (c.findActive && findActiveSinceRef.current[c.id] === undefined) {
        findActiveSinceRef.current[c.id] = seenAt;
      } else if (!c.findActive && findActiveSinceRef.current[c.id] !== undefined) {
        delete findActiveSinceRef.current[c.id];
      }
    }

    // The handheld also publishes on receive timeout and as a keep-alive, so
    // the stream arrives far faster than one packet per beacon. `fresh` marks
    // the entries that carry a newly received packet, which is what the tone
    // and the log should follow - otherwise the beep rate would track
    // notification rate rather than signal rate.
    const fresh = r.collars.filter(c => c.fresh);
    if (fresh.length === 0) {
      return;
    }

    const now = Date.now();
    lastPacketRef.current = now;

    for (const c of fresh) {
      const timing = timingRef.current[c.id] ?? {last: 0, intervals: []};
      if (timing.last > 0) {
        timing.intervals.push(now - timing.last);
        if (timing.intervals.length > INTERVAL_SAMPLES) {
          timing.intervals.shift();
        }
      }
      timing.last = now;
      timingRef.current[c.id] = timing;

    }

    // Tone policy: only the cat you are following beeps.
    //
    // With one collar a beep per packet is a beep per beacon, which is a
    // useful cue. With three collars it would be three times as often - and
    // an audio cue that fires constantly stops meaning anything. So the beep
    // follows one cat: the one you have focused, or the closest one if you
    // have not focused anything. The pitch still maps to distance, so the
    // tone bank is unchanged.
    //
    // The beep and the big reading have to agree about which cat that is. A
    // beep for one cat while the screen shows another would send you the
    // wrong way, so both read the same focus.
    const focus = focusRef.current;
    const followed =
      focus === null
        ? pickLeader(r.collars)
        : r.collars.find(c => c.id === focus) ?? null;
    if (followed && fresh.some(c => c.id === followed.id)) {
      playToneStep(toneStepFor(followed.percent));
      setPulse(true);
    }
    setNow(now);
  }, []);

  /**
   * Scan, connect and subscribe, trying `tries` scans before giving up.
   *
   * Manual Connect passes 1 so a failure is reported at once. An automatic
   * reconnect passes RECONNECT_SCANS: a handheld that was reset needs a moment
   * to advertise again, and the first scan can start in that gap.
   */
  const connectOnce = useCallback(
    async (tries = 1): Promise<boolean> => {
      const gen = ++sessionRef.current;
      const overtaken = () => gen !== sessionRef.current;
      // A new attempt is live again: accept its notifications from here on.
      stoppedRef.current = false;

      setError(null);

      if (!(await isBluetoothEnabled())) {
        if (overtaken()) {
          return false;
        }
        setError('Bluetooth is off. Turn it on and try again.');
        setStatus('error');
        return false;
      }
      if (!(await requestPermissions())) {
        if (overtaken()) {
          return false;
        }
        setError('Bluetooth permission is required to find the handheld.');
        setStatus('error');
        return false;
      }

      for (let attempt = 0; attempt < tries; attempt++) {
        if (overtaken()) {
          return false;
        }
        if (attempt > 0) {
          await new Promise(res => setTimeout(res, RECONNECT_GAP_MS));
          if (overtaken()) {
            return false;
          }
        }

        try {
          setStatus('scanning');
          setSeen([]);
          const {device, seen: nearby} = await scanForHandheld();
          if (overtaken()) {
            return false;
          }
          setSeen(nearby);

          if (!device) {
            // Try again only if this was an automatic reconnect and there are
            // scans left; otherwise this is the failure to report.
            if (attempt + 1 < tries) {
              continue;
            }
            setError(
              nearby.length === 0
                ? 'No BLE devices seen at all. Bluetooth permission is the usual cause.'
                : `Saw ${nearby.length} device(s), none named CatTracker. ` +
                  'Is another phone still connected to it?',
            );
            setStatus('idle');
            return false;
          }

          setStatus('connecting');
          deviceRef.current = device;
          const subscription = await connectAndSubscribe(device, onRoster);

          if (overtaken()) {
            // Disconnect landed while we were connecting. Undo it rather than
            // leaving the user attached to a link they asked to drop.
            try {
              subscription.remove();
            } catch {
              // Nothing to release.
            }
            deviceRef.current = null;
            await disconnect(device);
            return false;
          }

          subRef.current = subscription;
          // Start the liveness clock now as well as on the first notification,
          // so a handheld that connects and then goes silent is still noticed.
          lastLinkRef.current = Date.now();
          stallRef.current = false;
          setStatus('ready');
          return true;
        } catch (e) {
          if (overtaken()) {
            return false;
          }
          setError(e instanceof Error ? e.message : String(e));
          setStatus('error');
          const dev = deviceRef.current;
          deviceRef.current = null;
          if (dev) {
            await disconnect(dev);
          }
          return false;
        }
      }
      return false;
    },
    [onRoster],
  );

  const start = useCallback(async () => {
    // Already connected, or an automatic reconnect is already scanning. A
    // connected peripheral stops advertising, so a second scan cannot find it -
    // and starting a device scan while a GATT client is live can tear the
    // existing connection down. Ignore the tap instead of reporting a failure
    // the user can do nothing about.
    if (subRef.current || reconnectingRef.current) {
      return;
    }
    await connectOnce();
  }, [connectOnce]);

  /**
   * Put a dead link back together.
   *
   * The old watchdog only disconnected: it cleared the device, left the
   * subscription ref set so Connect was ignored, and sat on a spinner. That
   * made every false positive permanent. Tear down first, then scan, so the
   * peripheral is advertising by the time we look for it.
   */
  const reconnect = useCallback(async () => {
    if (reconnectingRef.current) {
      return;
    }
    reconnectingRef.current = true;
    try {
      await dropLink();
      await connectOnce(RECONNECT_SCANS);
    } finally {
      reconnectingRef.current = false;
    }
  }, [connectOnce, dropLink]);

  useEffect(() => {
    prepareTones();
    loadNames().then(setNames).catch(() => setNames({}));
    return () => {
      if (deviceRef.current) {
        disconnect(deviceRef.current);
      }
      releaseTones();
    };
  }, []);

  useEffect(() => {
    // Only tick while connected: this re-renders 5x a second, and there is no
    // reason to burn the phone's battery when nothing is being tracked.
    if (status !== 'ready') {
      setPulse(false);
      return;
    }
    const id = setInterval(() => {
      const now = Date.now();

      // Stall detection. A BLE link that dies quietly leaves the phone
      // believing it is still connected, so without this the app sits on a
      // frozen last reading forever and the user has to reconnect by hand.
      // The handheld's keep-alive (2 s) makes the link survive, but not every
      // drop is preventable, so notice this one and recover.
      //
      // The clock is the last notification of ANY kind - a fresh collar packet
      // updates it too, but so does a keep-alive. Using only fresh packets here
      // disconnected a healthy link 25 s after each 60 s beacon, which is why a
      // quiet collar used to look like a dead handheld.
      //
      // Only once per episode - reconnecting sets status away from 'ready',
      // which stops this ticker, and a new link resets the flag.
      if (
        lastLinkRef.current > 0 &&
        now - lastLinkRef.current > STALL_RECONNECT_MS &&
        !stallRef.current
      ) {
        stallRef.current = true;
        setStatus('connecting');
        void reconnect();
      }
      setNow(now);
      setPulse(p => {
        const since = now - lastPacketRef.current;
        if (since >= PULSE_MS && p) {
          return false;
        }
        return p;
      });
    }, 200);
    return () => clearInterval(id);
  }, [status, reconnect]);

  const openRename = useCallback(
    (id: number) => {
      setDraftName(names[String(id)] ?? '');
      setRenamingId(id);
    },
    [names],
  );

  const commitRename = useCallback(async () => {
    const id = renamingId;
    if (id === null) {
      return;
    }
    setRenamingId(null);
    const saved = await saveName(id, draftName);
    setNames(saved);
  }, [renamingId, draftName]);

  // Only the collars that have been said something about are listed. A collar
  // that has never been heard is a collar that is switched off, on a cat in
  // the other room, or not flashed yet - and a row saying "not heard" for
  // every one of those is noise. One that WAS heard and then went quiet stays
  // in the list, marked lost: that is the cat that ran off.
  const heard = collars.filter(c => c.have);
  const leader = pickLeader(collars);
  // The cat being followed: the one that was focused, or the closest one if
  // nothing is focused. Everything below - the reading, the tone, the beacon
  // countdown - follows this one cat.
  const followed =
    focusId === null
      ? leader
      : collars.find(c => c.id === focusId) ?? leader;
  const head = headline(followed, heard.length, status === 'ready');
  const step = toneStepFor(followed?.percent ?? 0);
  const followedTiming = followed ? timingRef.current[followed.id] : undefined;
  // Read via tick so the countdown recomputes on each tick.
  const measured = median(followedTiming?.intervals ?? []);
  const expectedMs = measured > 0 ? measured : DEFAULT_BEEP_MS;
  const beepElapsed =
    followedTiming && followedTiming.last > 0 ? now - followedTiming.last : null;

  // Tapping a row focuses that cat; tapping the one already focused lets go
  // of it and goes back to following the closest.
  const toggleFocus = useCallback(
    (id: number) => {
      setFocus(focusRef.current === id ? null : id);
    },
    [setFocus],
  );

  /**
   * Ask one collar to start or stop find mode.
   *
   * Deliberately local: the request is remembered here so the row can say
   * "waiting", and the write is fire-and-report. The collar only listens in a
   * short window after each of its own pings, so nothing can happen
   * immediately - and pretending otherwise ("Finding!") would be a lie for up
   * to a minute.
   */
  const toggleFind = useCallback(
    async (collar: Collar) => {
      const device = deviceRef.current;
      if (!device) {
        setError('Not connected to the handheld.');
        return;
      }
      const at = Date.now();
      const request = findReqs[collar.id];
      const {action} = findToggle(
        collar,
        request,
        at,
        findActiveSinceRef.current[collar.id] ?? null,
      );
      setError(null);
      setFindReqs(prev => ({
        ...prev,
        [collar.id]: {action, minutes: DEFAULT_FIND_MINUTES, at},
      }));
      try {
        await sendFindCommand(device, collar.id, action, DEFAULT_FIND_MINUTES);
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
      }
    },
    [findReqs],
  );

  return (
    <View style={[styles.container, {paddingTop: insets.top + 16, paddingBottom: insets.bottom + 16}]}>
      <StatusBar barStyle="light-content" />
      <Text style={styles.title}>Cat Tracker</Text>
      <Text style={styles.subtitle}>
        {heard.length} of {MAX_COLLARS} collars heard
      </Text>

      <View style={styles.roster}>
        {heard.length === 0 ? (
          // A different message from the verdict line below, which carries
          // the state of the signal. This one is about the list itself.
          <Text style={styles.rosterEmpty}>No collars heard yet</Text>
        ) : (
          heard.map(c => {
            const request = findReqs[c.id];
            const activeSince = findActiveSinceRef.current[c.id] ?? null;
            const findState = findStatus(c, request, now, activeSince);
            const toggle = findToggle(c, request, now, activeSince);
            return (
              <CollarRow
                key={c.id}
                collar={c}
                name={nameFor(names, c.id)}
                isLeader={leader?.id === c.id}
                isFocused={focusId === c.id}
                onPress={() => toggleFocus(c.id)}
                onRename={() => openRename(c.id)}
                findLabel={findState.label}
                findAction={toggle.action}
                findKind={toggle.kind}
                onToggleFind={() => toggleFind(c)}
              />
            );
          })
        )}
      </View>

      {head.reading ? (
        <View style={styles.headlineRow}>
          <Text style={[styles.headline, {color: head.tone}]}>{head.text}</Text>
          <Text style={[styles.headlineUnit, {color: head.tone}]}>dBm</Text>
          {head.snr !== null ? (
            <Text style={[styles.headlineSnr, {color: snrColor(head.snr)}]}>
              {`snr ${head.snr.toFixed(1)} dB`}
            </Text>
          ) : null}
        </View>
      ) : (
        <Text style={[styles.headlineStatus, {color: head.tone}]}>
          {head.text}
        </Text>
      )}

      <Text style={styles.meta}>
        {followed && followed.rssi !== null
          ? `${nameFor(names, followed.id)} · tone step ${step}`
          : 'no signal yet'}
      </Text>

      {status === 'ready' && (
        <BeepIndicator
          elapsedMs={beepElapsed}
          expectedMs={expectedMs}
          pulse={pulse}
        />
      )}

      {status !== 'ready' && status !== 'idle' && (
        <ActivityIndicator style={styles.spinner} color="#3498db" />
      )}
      {error && <Text style={styles.error}>{error}</Text>}

      {seen.length > 0 && (
        <View style={styles.scanBox}>
          <Text style={styles.scanTitle}>Devices seen nearby</Text>
          {seen.slice(0, 6).map(d => (
            <Text key={d.id} style={styles.scanItem}>
              {d.name ?? '(no name)'} · {d.rssi ?? '--'} dBm
            </Text>
          ))}
        </View>
      )}

      <View style={styles.buttonRow}>
        <Pressable
          style={styles.button}
          onPress={start}
          accessibilityRole="button"
          accessibilityLabel="Connect">
          <Text style={styles.buttonText}>Connect</Text>
        </Pressable>
        <Pressable
          style={[styles.button, styles.buttonGhost]}
          onPress={stop}
          accessibilityRole="button"
          accessibilityLabel="Disconnect">
          <Text style={styles.buttonText}>Disconnect</Text>
        </Pressable>
      </View>

      <Modal
        transparent
        animationType="fade"
        visible={renamingId !== null}
        onRequestClose={() => setRenamingId(null)}>
        <View style={styles.modalBackdrop}>
          <View style={styles.modalCard}>
            <Text style={styles.modalTitle}>Name this cat</Text>
            <Text style={styles.modalSub}>
              Collar {renamingId === null ? '' : renamingId + 1}
            </Text>
            <TextInput
              style={styles.modalInput}
              value={draftName}
              onChangeText={setDraftName}
              placeholder={
                renamingId === null ? '' : defaultName(renamingId)
              }
              placeholderTextColor="#5d6d7e"
              autoFocus
              returnKeyType="done"
              onSubmitEditing={commitRename}
            />
            <View style={styles.modalButtons}>
              <Pressable
                style={[styles.button, styles.buttonGhost]}
                onPress={() => setRenamingId(null)}>
                <Text style={styles.buttonText}>Cancel</Text>
              </Pressable>
              <Pressable style={styles.button} onPress={commitRename}>
                <Text style={styles.buttonText}>Save</Text>
              </Pressable>
            </View>
          </View>
        </View>
      </Modal>
    </View>
  );
}

function App() {
  return (
    <SafeAreaProvider>
      <Tracker />
    </SafeAreaProvider>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#0d1117',
    paddingHorizontal: 24,
  },
  title: {
    color: '#ecf0f1',
    fontSize: 28,
    fontWeight: '700',
  },
  subtitle: {
    color: '#7f8c8d',
    fontSize: 14,
    marginTop: 2,
  },
  roster: {
    marginTop: 20,
  },
  rosterEmpty: {
    color: '#5d6d7e',
    fontSize: 13,
    textAlign: 'center',
    marginTop: 24,
  },
  row: {
    backgroundColor: '#16202b',
    borderRadius: 10,
    paddingVertical: 12,
    paddingHorizontal: 14,
    marginBottom: 8,
    borderLeftWidth: 3,
    borderLeftColor: '#243040',
  },
  rowLeader: {
    borderLeftColor: '#2ecc71',
    backgroundColor: '#1a2634',
  },
  // The cat being followed. Blue rather than green, so "the one you are
  // following" and "the closest one" stay distinguishable when they are
  // different cats.
  rowFocused: {
    borderLeftColor: '#2980b9',
    backgroundColor: '#1a2430',
  },
  rowHead: {
    flexDirection: 'row',
    alignItems: 'center',
  },
  rowName: {
    color: '#ecf0f1',
    fontSize: 18,
    fontWeight: '600',
  },
  rowNameLeader: {
    color: '#2ecc71',
  },
  rowNameFocused: {
    color: '#5dade2',
  },
  tagFocused: {
    color: '#5dade2',
    fontSize: 11,
    marginLeft: 8,
    fontWeight: '600',
  },
  editButton: {
    marginLeft: 'auto',
    paddingLeft: 12,
    paddingVertical: 2,
  },
  tagFind: {
    color: '#f39c12',
    fontSize: 11,
    marginLeft: 8,
    fontWeight: '600',
  },
  // Deliberately small and quiet: this is a control you press once at the
  // start of a hunt, not the thing you read while walking. The reading above
  // stays the biggest thing on the screen.
  findButton: {
    marginLeft: 10,
    paddingHorizontal: 10,
    paddingVertical: 3,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: '#f39c12',
  },
  findButtonStop: {
    borderColor: '#e74c3c',
  },
  findButtonText: {
    color: '#f39c12',
    fontSize: 11,
    fontWeight: '700',
  },
  findButtonTextStop: {
    color: '#e74c3c',
  },
  editIcon: {
    color: '#5d6d7e',
    fontSize: 16,
  },
  tagLeader: {
    color: '#2ecc71',
    fontSize: 11,
    marginLeft: 8,
    fontWeight: '600',
  },
  tagLost: {
    color: '#e74c3c',
    fontSize: 11,
    marginLeft: 8,
    fontWeight: '600',
  },
  tagLowBatt: {
    color: '#f39c12',
    fontSize: 11,
    marginLeft: 8,
    fontWeight: '600',
  },
  rowReadings: {
    flexDirection: 'row',
    alignItems: 'baseline',
    marginTop: 6,
  },
  // The roster's own numbers. Big enough to read at arm's length while
  // walking, small enough that the followed cat's reading above stays the
  // biggest thing on the screen.
  rowDbm: {
    fontSize: 26,
    fontWeight: '700',
    fontVariant: ['tabular-nums'],
  },
  rowDbmUnit: {
    color: '#5d6d7e',
    fontSize: 12,
    marginLeft: 5,
  },
  rowSnr: {
    color: '#95a5a6',
    fontSize: 18,
    marginLeft: 20,
    fontVariant: ['tabular-nums'],
  },
  rowSnrUnit: {
    color: '#5d6d7e',
    fontSize: 12,
    marginLeft: 5,
  },
  rowAge: {
    color: '#7f8c8d',
    fontSize: 12,
    marginLeft: 'auto',
    fontVariant: ['tabular-nums'],
  },
  rowDetail: {
    color: '#5d6d7e',
    fontSize: 11,
    marginTop: 6,
    fontVariant: ['tabular-nums'],
  },
  // The battery is a real reading, not a footnote, so it is bold and white -
  // and the note beside it says how old it can be, because the collar samples
  // the cell every ten minutes while it pings every sixty seconds.
  rowBatt: {
    color: '#ecf0f1',
    fontSize: 12,
    fontWeight: '700',
  },
  rowBattNote: {
    color: '#5d6d7e',
    fontSize: 11,
  },
  headlineRow: {
    flexDirection: 'row',
    alignItems: 'baseline',
    justifyContent: 'center',
    marginTop: 18,
  },
  headline: {
    fontSize: 40,
    fontWeight: '700',
    fontVariant: ['tabular-nums'],
  },
  headlineUnit: {
    fontSize: 18,
    marginLeft: 6,
  },
  headlineSnr: {
    color: '#95a5a6',
    fontSize: 18,
    marginLeft: 18,
    fontVariant: ['tabular-nums'],
  },
  headlineStatus: {
    textAlign: 'center',
    fontSize: 18,
    fontWeight: '600',
    marginTop: 18,
  },
  meta: {
    textAlign: 'center',
    color: '#5d6d7e',
    fontSize: 12,
    marginTop: 10,
  },
  spinner: {
    marginTop: 20,
  },
  error: {
    color: '#e74c3c',
    textAlign: 'center',
    marginTop: 16,
    fontSize: 13,
  },
  scanBox: {
    marginTop: 16,
    padding: 12,
    backgroundColor: '#16202b',
    borderRadius: 8,
  },
  scanTitle: {
    color: '#7f8c8d',
    fontSize: 12,
    marginBottom: 6,
  },
  scanItem: {
    color: '#95a5a6',
    fontSize: 12,
    lineHeight: 18,
  },
  buttonRow: {
    flexDirection: 'row',
    marginTop: 'auto',
  },
  button: {
    flex: 1,
    backgroundColor: '#2980b9',
    paddingVertical: 14,
    borderRadius: 8,
    alignItems: 'center',
    marginHorizontal: 4,
  },
  buttonGhost: {
    backgroundColor: '#2c3e50',
  },
  buttonText: {
    color: '#fff',
    fontWeight: '600',
  },
  beepWaiting: {
    color: '#5d6d7e',
    fontSize: 12,
    textAlign: 'center',
    marginTop: 12,
  },
  beepBox: {
    marginTop: 14,
  },
  beepRow: {
    flexDirection: 'row',
    alignItems: 'center',
  },
  beepDot: {
    width: 12,
    height: 12,
    borderRadius: 6,
    backgroundColor: '#243040',
    marginRight: 8,
  },
  beepDotLit: {
    backgroundColor: '#2ecc71',
  },
  beepText: {
    color: '#95a5a6',
    fontSize: 13,
  },
  beepNext: {
    color: '#5d6d7e',
    fontSize: 13,
    marginLeft: 'auto',
    fontVariant: ['tabular-nums'],
  },
  beepLate: {
    color: '#e74c3c',
    fontWeight: '600',
  },
  beepTrack: {
    height: 5,
    borderRadius: 3,
    backgroundColor: '#243040',
    marginTop: 8,
    overflow: 'hidden',
  },
  beepFill: {
    height: 5,
    borderRadius: 3,
    backgroundColor: '#2980b9',
  },
  beepFillLate: {
    backgroundColor: '#e74c3c',
  },
  modalBackdrop: {
    flex: 1,
    backgroundColor: 'rgba(0,0,0,0.7)',
    justifyContent: 'center',
    paddingHorizontal: 32,
  },
  modalCard: {
    backgroundColor: '#16202b',
    borderRadius: 12,
    padding: 20,
  },
  modalTitle: {
    color: '#ecf0f1',
    fontSize: 18,
    fontWeight: '700',
  },
  modalSub: {
    color: '#7f8c8d',
    fontSize: 12,
    marginTop: 2,
    marginBottom: 12,
  },
  modalInput: {
    backgroundColor: '#0d1117',
    borderRadius: 8,
    color: '#ecf0f1',
    paddingHorizontal: 12,
    paddingVertical: 10,
    fontSize: 16,
  },
  modalButtons: {
    flexDirection: 'row',
    marginTop: 16,
  },
});

export default App;
