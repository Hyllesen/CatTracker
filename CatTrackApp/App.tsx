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
} from './src/ble';
import {
  Collar,
  MAX_COLLARS,
  Roster,
  pickLeader,
  toneStepFor,
} from './src/signal';
import {CatNames, defaultName, loadNames, nameFor, saveName} from './src/cats';
import {
  addMarker,
  addSample,
  elapsedSeconds,
  exportCsv,
  isRecording,
  rowCount,
  startRecording,
  stopRecording,
} from './src/logging';
import {
  playToneStep,
  prepareTones,
  releaseTones,
  setTonesEnabled,
  tonesEnabled,
} from './src/tone';

const BAR_SEGMENTS = 10;

/** Preset distance waypoints. Labels are written into the CSV next to each
 *  marker so the exported file is self-describing on its own. */
const MARKERS = ['5m', '10m', '25m', '50m', '100m', 'clear'];

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

function SignalBar({percent}: {percent: number}) {
  const lit = Math.round((percent / 100) * BAR_SEGMENTS);
  return (
    <View style={styles.bar} accessibilityLabel={`Signal ${percent} percent`}>
      {Array.from({length: BAR_SEGMENTS}).map((_, i) => (
        <View
          key={i}
          style={[
            styles.segment,
            {
              backgroundColor: i < lit ? barColor(percent) : '#243040',
            },
          ]}
        />
      ))}
    </View>
  );
}

// Green when close, amber in the middle, red when weak. Deliberately not a
// rainbow - two hues plus neutral reads faster than many.
function barColor(percent: number): string {
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
 * number the radio actually measured. It used to be a proximity phrase
 * ("Very close - check the bushes") instead, which says less: the phrase
 * cannot be compared against a reading from another walk, and two people
 * looking at the same screen cannot tell whether "getting warmer" means
 * better or worse than last time.
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
): {text: string; tone: string; reading: boolean} {
  if (collar && collar.rssi !== null) {
    return {
      text: collar.rssi.toFixed(1),
      // A collar that has gone quiet turns the reading red whatever the last
      // value was. The number is still the last thing the radio heard, but
      // it is not a live reading any more, and a frozen green number would
      // say "you are there" when the cat has walked off.
      tone: collar.stale ? '#e74c3c' : barColor(collar.percent),
      reading: true,
    };
  }
  if (heard > 0) {
    // Every cat we have heard from has gone quiet. That is a different
    // thing from "nothing heard yet", and it is the situation that
    // matters: a collar that has stopped answering is a cat that has
    // stopped answering.
    return {text: 'All the collars have gone quiet', tone: '#e74c3c', reading: false};
  }
  if (!connected) {
    return {text: 'Press Connect to start', tone: '#7f8c8d', reading: false};
  }
  return {text: 'Listening for the collars...', tone: '#7f8c8d', reading: false};
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
 * Comfortably longer than the handheld's keep-alive (2 s) and longer than the
 * worst expected beacon gap (one interval plus the 200 ms receive margin), so
 * a healthy link never trips this - it only fires when nothing at all has
 * arrived for a long time, which is the signature of a dead connection.
 */
const STALL_RECONNECT_MS = 25000;

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
 * One cat: name, signal bar, and the details worth reading when you stop.
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
}: {
  collar: Collar;
  name: string;
  isLeader: boolean;
  isFocused: boolean;
  onPress: () => void;
  onRename: () => void;
}) {
  const detail: string[] = [];
  if (collar.have) {
    detail.push(
      collar.batteryPercent === null
        ? 'batt --'
        : `batt ${collar.batteryPercent}%`,
    );
    if (collar.snr !== null) {
      detail.push(`snr ${collar.snr.toFixed(1)} dB`);
    }
    detail.push(`${collar.ageSeconds}s ago`);
  }

  return (
    <Pressable
      style={[styles.row, isFocused ? styles.rowFocused : null]}
      onPress={onPress}
      accessibilityRole="button"
      accessibilityLabel={`${name}, ${collar.percent} percent signal`}>
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
        <Pressable
          style={styles.editButton}
          onPress={onRename}
          accessibilityRole="button"
          accessibilityLabel={`Rename ${name}`}
          hitSlop={{top: 10, bottom: 10, left: 10, right: 10}}>
          <Text style={styles.editIcon}>✎</Text>
        </Pressable>
      </View>
      <View style={styles.rowBar}>
        <View style={styles.rowBarTrack}>
          <SignalBar percent={collar.percent} />
        </View>
        <Text style={styles.rowPercent}>{collar.percent}%</Text>
      </View>
      <Text style={styles.rowDetail}>
        {detail.length ? detail.join(' · ') : 'not heard yet'}
      </Text>
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
  const [toneOn, setToneOn] = useState(tonesEnabled());
  const [rec, setRec] = useState({on: false, rows: 0, secs: 0});
  const [csvNote, setCsvNote] = useState<string | null>(null);
  const [renamingId, setRenamingId] = useState<number | null>(null);
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
  // Beacon timing, per collar so each cat's own cadence is measured rather
  // than one average over all of them. Kept in refs so the BLE callback
  // never needs re-binding, and so reading them does not re-render on every
  // packet.
  const timingRef = useRef<Record<number, Timing>>({});
  const lastPacketRef = useRef<number>(0);
  const stallRef = useRef(false);
  // Ticks every 200 ms while connected. It is the clock the countdown reads,
  // so the beacon timer does not depend on when React happens to re-render.
  const [now, setNow] = useState(() => Date.now());
  const [pulse, setPulse] = useState(false);

  const stop = useCallback(async () => {
    if (subRef.current) {
      subRef.current.remove();
      subRef.current = null;
    }
    if (deviceRef.current) {
      await disconnect(deviceRef.current);
      deviceRef.current = null;
    }
    timingRef.current = {};
    lastPacketRef.current = 0;
    setCollars([]);
    setFocus(null);
    setNow(Date.now());
    setStatus('idle');
  }, [setFocus]);

  const onRoster = useCallback((r: Roster) => {
    setCollars(r.collars);
    setStatus('ready');

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
    stallRef.current = false;
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

      // Fire-and-forget: a logging failure must never delay the tone or
      // stall the BLE notification stream.
      addSample(c);
    }
    setRec(r2 => (r2.on ? {...r2, rows: rowCount()} : r2));

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

  const start = useCallback(async () => {
    // Already connected. A connected peripheral stops advertising, so a second
    // scan cannot find it - and starting a device scan while a GATT client is
    // live can tear the existing connection down. Ignore the tap instead of
    // reporting a failure the user can do nothing about.
    if (subRef.current) {
      return;
    }

    setError(null);

    if (!(await isBluetoothEnabled())) {
      setError('Bluetooth is off. Turn it on and try again.');
      return;
    }
    if (!(await requestPermissions())) {
      setError('Bluetooth permission is required to find the handheld.');
      return;
    }

    try {
      setStatus('scanning');
      setSeen([]);
      const {device, seen: nearby} = await scanForHandheld();
      setSeen(nearby);

      if (!device) {
        setError(
          nearby.length === 0
            ? 'No BLE devices seen at all. Bluetooth permission is the usual cause.'
            : `Saw ${nearby.length} device(s), none named CatTracker. ` +
              'Is another phone still connected to it?',
        );
        setStatus('idle');
        return;
      }

      setStatus('connecting');
      deviceRef.current = device;
      subRef.current = await connectAndSubscribe(device, onRoster);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      setStatus('error');
    }
  }, [onRoster]);

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
      // The keep-alive on the handheld makes the link survive, but not every
      // drop is preventable, so notice this one and recover.
      //
      // Only counts silence with no fresh packet from ANY collar, and only
      // once per episode - reconnecting sets status back to 'connecting',
      // which stops this.
      if (
        lastPacketRef.current > 0 &&
        now - lastPacketRef.current > STALL_RECONNECT_MS &&
        !stallRef.current
      ) {
        stallRef.current = true;
        setStatus('connecting');
        const dev = deviceRef.current;
        deviceRef.current = null;
        if (dev) {
          disconnect(dev);
        }
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
  }, [status]);

  const toggleTones = useCallback(() => {
    const next = !tonesEnabled();
    setTonesEnabled(next);
    setToneOn(next);
  }, []);

  const toggleRecording = useCallback(async () => {
    if (isRecording()) {
      await stopRecording();
      setRec(r => ({on: false, rows: r.rows, secs: 0}));
      setCsvNote('Saved. Export when you are done walking.');
      return;
    }
    await startRecording();
    setCsvNote(null);
    setRec({on: true, rows: 0, secs: 0});
  }, []);

  // Elapsed time only needs to tick while recording; driving it from the
  // notification stream would leave it frozen whenever the signal drops out.
  useEffect(() => {
    if (!rec.on) {
      return;
    }
    const id = setInterval(() => {
      setRec(r => (r.on ? {...r, secs: elapsedSeconds()} : r));
    }, 1000);
    return () => clearInterval(id);
  }, [rec.on]);

  const mark = useCallback(async (label: string) => {
    await addMarker(label);
    setRec(r => ({...r, rows: rowCount()}));
  }, []);

  const doExport = useCallback(async () => {
    try {
      const file = await exportCsv();
      setCsvNote(`Exported ${file.split('/').pop()}`);
    } catch (e) {
      setCsvNote(e instanceof Error ? e.message : String(e));
    }
  }, []);

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
          heard.map(c => (
            <CollarRow
              key={c.id}
              collar={c}
              name={nameFor(names, c.id)}
              isLeader={leader?.id === c.id}
              isFocused={focusId === c.id}
              onPress={() => toggleFocus(c.id)}
              onRename={() => openRename(c.id)}
            />
          ))
        )}
      </View>

      {head.reading ? (
        <View style={styles.headlineRow}>
          <Text style={[styles.headline, {color: head.tone}]}>{head.text}</Text>
          <Text style={[styles.headlineUnit, {color: head.tone}]}>dBm</Text>
        </View>
      ) : (
        <Text style={[styles.headlineStatus, {color: head.tone}]}>
          {head.text}
        </Text>
      )}

      <Text style={styles.meta}>
        {followed && followed.rssi !== null
          ? `${nameFor(names, followed.id)} · signal ${followed.percent}% · tone step ${step}`
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

      <Pressable
        style={[styles.toneToggle, toneOn ? null : styles.toneToggleOff]}
        onPress={toggleTones}
        accessibilityRole="button"
        accessibilityLabel={toneOn ? 'Mute tones' : 'Unmute tones'}>
        <Text style={styles.toneToggleText}>
          {toneOn ? 'Tone on' : 'Tone muted'}
        </Text>
      </Pressable>

      <View style={styles.recBox}>
        <Pressable
          style={[styles.recButton, rec.on ? styles.recButtonOn : null]}
          onPress={toggleRecording}>
          <Text style={styles.buttonText}>
            {rec.on ? 'Stop recording' : 'Record range test'}
          </Text>
        </Pressable>
        {rec.on && (
          <>
            <Text style={styles.recStatus}>
              {rec.rows} rows · {rec.secs}s
            </Text>
            <View style={styles.markerRow}>
              {MARKERS.map(m => (
                <Pressable
                  key={m}
                  style={styles.markerButton}
                  onPress={() => mark(m)}>
                  <Text style={styles.markerText}>{m}</Text>
                </Pressable>
              ))}
            </View>
          </>
        )}
        {!rec.on && rec.rows > 0 && (
          <Pressable style={styles.exportButton} onPress={doExport}>
            <Text style={styles.buttonText}>Export CSV ({rec.rows} rows)</Text>
          </Pressable>
        )}
        {csvNote && <Text style={styles.recNote}>{csvNote}</Text>}
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
  rowBar: {
    flexDirection: 'row',
    alignItems: 'center',
    marginTop: 8,
  },
  rowBarTrack: {
    flex: 1,
  },
  rowPercent: {
    color: '#95a5a6',
    fontSize: 13,
    width: 44,
    textAlign: 'right',
    fontVariant: ['tabular-nums'],
  },
  rowDetail: {
    color: '#5d6d7e',
    fontSize: 11,
    marginTop: 6,
    fontVariant: ['tabular-nums'],
  },
  bar: {
    flexDirection: 'row',
    height: 14,
  },
  segment: {
    flex: 1,
    marginHorizontal: 2,
    borderRadius: 3,
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
  toneToggle: {
    marginTop: 10,
    paddingVertical: 10,
    borderRadius: 8,
    alignItems: 'center',
    backgroundColor: '#1f6f4a',
  },
  toneToggleOff: {
    backgroundColor: '#3a2a2a',
  },
  toneToggleText: {
    color: '#ecf0f1',
    fontSize: 14,
    fontWeight: '600',
  },
  recBox: {
    marginTop: 10,
  },
  recButton: {
    paddingVertical: 12,
    borderRadius: 8,
    alignItems: 'center',
    backgroundColor: '#2c3e50',
  },
  recButtonOn: {
    backgroundColor: '#c0392b',
  },
  recStatus: {
    color: '#7f8c8d',
    fontSize: 12,
    textAlign: 'center',
    marginTop: 8,
  },
  markerRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'center',
    marginTop: 8,
  },
  markerButton: {
    paddingVertical: 8,
    paddingHorizontal: 12,
    margin: 3,
    borderRadius: 6,
    backgroundColor: '#1f6f4a',
  },
  markerText: {
    color: '#ecf0f1',
    fontSize: 12,
    fontWeight: '600',
  },
  exportButton: {
    marginTop: 8,
    paddingVertical: 12,
    borderRadius: 8,
    alignItems: 'center',
    backgroundColor: '#2471a3',
  },
  recNote: {
    color: '#5d6d7e',
    fontSize: 11,
    textAlign: 'center',
    marginTop: 6,
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
