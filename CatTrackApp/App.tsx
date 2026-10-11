/**
 * Cat Tracker - handheld signal display.
 *
 * Shows the signal strength broadcast over BLE by the ESP32-C3 handheld and
 * plays a discrete tone for it, so you can walk toward the cat without looking
 * at the screen. The tone is the primary signal while tracking; the readout is
 * there for when you stop and want the detail.
 */

import React, {useCallback, useEffect, useRef, useState} from 'react';
import {
  ActivityIndicator,
  Permission,
  PermissionsAndroid,
  Platform,
  Pressable,
  StatusBar,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import {Device} from 'react-native-ble-plx';
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
import {EMPTY_SIGNAL, Signal, toneStepFor} from './src/signal';
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

function verdict(signal: Signal): {text: string; tone: string} {
  if (!signal.rssi) {
    return {text: 'Listening for the collar...', tone: '#7f8c8d'};
  }
  if (signal.stale) {
    return {text: 'Signal lost - go back the way you came', tone: '#e74c3c'};
  }
  if (signal.percent >= 80) {
    return {text: 'Very close - check the bushes', tone: '#2ecc71'};
  }
  if (signal.percent >= 55) {
    return {text: 'Getting warmer', tone: '#2ecc71'};
  }
  if (signal.percent >= 30) {
    return {text: 'Faint signal - keep walking', tone: '#f39c12'};
  }
  return {text: 'Very weak - move closer', tone: '#e74c3c'};
}

function Tracker() {
  const insets = useSafeAreaInsets();
  const [status, setStatus] = useState<string>('idle');
  const [signal, setSignal] = useState<Signal>(EMPTY_SIGNAL);
  const [error, setError] = useState<string | null>(null);
  const [seen, setSeen] = useState<ScanResult['seen']>([]);
  const [toneOn, setToneOn] = useState(tonesEnabled());
  const [rec, setRec] = useState({on: false, rows: 0, secs: 0});
  const [csvNote, setCsvNote] = useState<string | null>(null);
  const deviceRef = useRef<Device | null>(null);

  const stop = useCallback(async () => {
    if (deviceRef.current) {
      await disconnect(deviceRef.current);
      deviceRef.current = null;
    }
    setStatus('idle');
    setSignal(EMPTY_SIGNAL);
  }, []);

  const start = useCallback(async () => {
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
      await connectAndSubscribe(device, s => {
        setSignal(s);
        setStatus('ready');
        // The handheld also publishes on receive timeout, so the stream
        // arrives far faster than one packet per beacon. `fresh` marks the
        // updates that carry a newly received packet, which is what the tone
        // should follow - otherwise the beep rate would track notification
        // rate rather than signal rate.
        if (s.fresh) {
          playToneStep(toneStepFor(s.percent));
          // Fire-and-forget: a logging failure must never delay the tone or
          // stall the BLE notification stream.
          addSample(s);
          setRec(r => (r.on ? {...r, rows: rowCount()} : r));
        }
      });
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      setStatus('error');
    }
  }, []);

  useEffect(() => {
    prepareTones();
    return () => {
      if (deviceRef.current) {
        disconnect(deviceRef.current);
      }
      releaseTones();
    };
  }, []);

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

  const v = verdict(signal);
  const step = toneStepFor(signal.percent);

  return (
    <View style={[styles.container, {paddingTop: insets.top + 16, paddingBottom: insets.bottom + 16}]}>
      <StatusBar barStyle="light-content" />
      <Text style={styles.title}>Cat Tracker</Text>
      <Text style={styles.subtitle}>Handheld signal</Text>

      <View style={styles.readout}>
        <Text style={[styles.rssi, {color: v.tone}]}>
          {signal.rssi === null ? '--.-' : signal.rssi.toFixed(1)}
        </Text>
        <Text style={styles.rssiUnit}>dBm</Text>
      </View>

      <SignalBar percent={signal.percent} />

      <Text style={[styles.verdict, {color: v.tone}]}>{v.text}</Text>

      <Text style={styles.meta}>
        signal {signal.percent}% · tone step {step} · age {signal.ageSeconds}s
      </Text>

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
        <Pressable style={styles.button} onPress={start}>
          <Text style={styles.buttonText}>Connect</Text>
        </Pressable>
        <Pressable style={[styles.button, styles.buttonGhost]} onPress={stop}>
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
  readout: {
    flexDirection: 'row',
    alignItems: 'baseline',
    justifyContent: 'center',
    marginTop: 48,
  },
  rssi: {
    fontSize: 72,
    fontWeight: '700',
    fontVariant: ['tabular-nums'],
  },
  rssiUnit: {
    color: '#7f8c8d',
    fontSize: 20,
    marginLeft: 6,
  },
  bar: {
    flexDirection: 'row',
    height: 40,
    marginTop: 24,
  },
  segment: {
    flex: 1,
    marginHorizontal: 3,
    borderRadius: 4,
  },
  verdict: {
    textAlign: 'center',
    fontSize: 18,
    fontWeight: '600',
    marginTop: 28,
  },
  meta: {
    textAlign: 'center',
    color: '#5d6d7e',
    fontSize: 12,
    marginTop: 12,
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
});

export default App;