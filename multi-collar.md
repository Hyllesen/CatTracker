# Multi-collar plan

Extending the tracker from one collar to three, with the app showing all cats at once.

## Current state

The system is a three-tier point-to-point chain with no backend, no server, and no database:

```
collar (nRF52840) --LoRa 433 MHz--> handheld (ESP32-C3) --BLE GATT notify--> phone app (React Native)
```

There is exactly one collar. Its identity is a hardcoded ASCII literal `"CAT-01"` in the
payload (`collar/collar.ino:258`). The handheld receives that payload and **throws the
`CAT-01` away** — `parseBattery()` (`handheld_ble/handheld_ble.ino:113`) does
`strstr(payload, "bat=")` and never looks at the prefix.

There is no addressing to invent. Raw SX1278 LoRa has no node addressing primitive and
none is emulated. The "pairing" mechanism is a table of air-interface constants that
must be byte-identical in three firmware files (`collar/README.md:120-133`), plus a
copy-paste obligation documented in `AGENTS.md`.

## Design decisions

### 1. Collar ID is a small integer, not a hardware-derived one

The ID is `0`, `1`, or `2`, set at flash time with `-DCOLLAR_ID=0|1|2`.

**Why not derive from `NRF_FICR->DEVICEID`?** With 3 collars in 3 slots, computing the
slot as `id % MAX_COLLARS` is a birthday problem — a 1-in-3 chance two collars share a
slot and collide forever. Hardware-derived IDs cannot avoid this because collars cannot
coordinate to avoid it.

**What this buys:**
- A perfect TDMA grid with zero collisions (slot = ID, phase = ID × slot width)
- The array index *is* the ID — no map, no find-or-create, no lookup
- A name the app can show without a provisioning table

**The cost** is reflashing to reassign a collar to a different cat. Partly paid back by
logging the chip's factory `DEVICEID` at boot — so you can confirm "I just flashed
Miso's collar, not the spare" by reading one line of serial, and writing it on the
enclosure.

### 2. Binary wire frame, not extended ASCII

Replacing the ASCII payload with a 6-byte binary frame.

**Why:** at SF9/BW125 the payload rounds to whole symbols, so 5 and 6 bytes cost
exactly the same airtime. The binary frame is **140 ms** vs **~230 ms** for today's
13-byte ASCII — a 90 ms saving that drops collar average draw to roughly 1.9 mA
(→ ~55-60 h battery life instead of ~39 h), because transmit now dominates the
collar's power budget after the fixed draws were removed.

**Why a magic byte instead of a CRC:** LoRa already has a mandatory payload CRC.
Adding another buys nothing. Byte 0 instead rejects stray decoded frames so garbage
never becomes a phantom cat.

**Backward compatibility:** an old ASCII collar sends `'C'` = `0x43` ≠ `0xC1` and is
ignored. The handheld will additionally accept legacy ASCII for one release, mapped to
collar 0, so cats can be swapped one at a time instead of opening all three enclosures
at once.

### 3. Phase stagger for collision avoidance

Each collar pings at `t ≡ id × (PING_INTERVAL_MS / MAX_COLLARS)`. No negotiation, no
discovery, no shared clock. This works only because the link is strictly one-way —
the collar never learns the roster, so every collar must be independently valid.

Implementation: one `delay()` at boot to acquire the phase, then free-run at
`PING_INTERVAL_MS`. Collars brought up in the same session lock to separate slots.

### 4. App shows all cats at once

The app renders a list with a signal bar per cat, closest highlighted. This answers
"which one ran off" directly — the actual jungle problem.

**Tone policy:** `publishSignal(true)` fires on every packet (`handheld_ble.ino:451`).
With 3 collars the app would beep 3× as often, and an audio cue that fires constantly
stops meaning anything. Fix: keep FRESH meaning "a real packet, not a keepalive" in
firmware, and move the beeping policy into `App.tsx` — tone only when the cat that
updated is the current leader. Pitch still maps to distance, so the existing 10-WAV
tone bank is untouched.

Pinning a specific cat to follow instead is a cheap later addition (one control
characteristic).

### 5. Cat names in the app

No AsyncStorage dependency, but `react-native-file-access` is already present — so a
small `cats.json` in DocumentDir costs zero new deps. First sight of an unknown ID
prompts "name this cat"; ids default to "Cat 1/2/3" so it's usable with zero setup.

## Wire formats

### Collar → handheld (6 bytes)

```
byte 0   0xC1        magic / protocol version
byte 1   seq         wrapping 0..255, increments every ping
byte 2   id          0..MAX_COLLARS-1
byte 3   battery     0..100, 0xFF = unknown (ADC pin unwired)
byte 4   flags       bit0 = LOW_BATT (<15%), rest reserved
byte 5   reserved
```

Airtime: **140 ms** at SF9/BW125 (symbol time 2⁹/125 kHz = 4.096 ms; payload symbols
= ceil((8×6 − 4×9 + 28 + 16)/36) = 2; ToA = (8 + 4.25 + 2×11) × 4.096 = 140.3 ms).

The `seq` counter enables a **collision counter** on the handheld — the number that
tells you whether 3 collars is actually working in the field.

### Handheld → app (26 bytes)

Same characteristic UUID (app's UUIDs don't change), now readable *and* notifiable:

```
byte 0      version = 0x02
byte 1      count   (0..3)
bytes 2..25 per collar, 8 bytes:
  +0  id
  +1  flags   bit0 HAVE, bit1 FRESH, bit2 STALE, bit3 LOW_BATT
  +2  rssi*10 int16 LE
  +4  percent 0..100
  +5  age     seconds since last packet, capped 255
  +6  battery 0..100 or 255
  +7  seq
```

Fits MTU 64 with the app's existing `requestMTU(64)`. Two safety measures:

- **Readable characteristic.** NimBLE characteristics are readable by default, so a
  phone that connects late does `read()` for a full roster snapshot and *then*
  subscribes. Fixes "I only ever see collar 2" for free.
- **Length validation in `decodeRoster()`** — return `null` if
  `2 + count*8 > bytes.length`. 26 bytes exceeds the 20-byte default ATT MTU, so if
  negotiation ever fails you get *no updates* (visible) rather than *another cat's
  data* (invisible).

## File-by-file changes

| File | Change |
|---|---|
| `collar/collar.ino` | Binary frame builder, `COLLAR_ID` build flag, boot phase delay, `DEVICEID` in the `v` log |
| `handheld_ble/handheld_ble.ino` | `CollarState[3]`, binary + legacy parser, per-collar smoothing/staleness, 26-byte publisher, `RX_TIMEOUT_MS` 8200 → ~3000 |
| `handheld/handheld.ino` | Same parse; `packSample()` 12 → 14 bytes (adds id + seq) |
| `transmitter/transmitter.ino` | Binary frames + `-DFAKE_COLLARS=n` to fake 3 cats from one spare board |
| `CatTrackApp/src/signal.ts` | `decodeSignal` → `decodeRoster` returning `Roster { collars: Collar[] }` |
| `CatTrackApp/src/ble.ts` | Add `readRoster()` on connect |
| `CatTrackApp/App.tsx` | Roster state, one row per cat, per-collar tone policy + stall detection |
| `CatTrackApp/src/logging.ts` | CSV gains an `id` column (one file, mixed IDs) |
| `CatTrackApp/src/cats.ts` | New: read/write `cats.json` in DocumentDir, name prompt on unknown ID |
| `tools/analyze_walk.py` | `groupby id` |
| `collar/README.md` | Document the frame + the ID/slot rule |
| `handheld/README.md` | Update payload format section |
| `AGENTS.md` | "Must match" rule now covers the frame, not just air settings |

### Handheld state

```cpp
struct CollarState {
  bool     seen;
  uint8_t  id;
  uint8_t  seq;
  float    rssiHistory[SMOOTH_N];   // 12
  uint8_t  idx, count;
  float    rssiSmoothed;
  uint32_t lastMs;
  uint8_t  batteryPct;
  uint32_t packets;
  uint32_t dropped;     // seq gaps = collisions
};
CollarState collars[MAX_COLLARS];  // index == id
```

~350 bytes total. Negligible on a C3 with 400 KB SRAM. Unknown ID (≥ MAX_COLLARS) →
drop + count.

### RX_TIMEOUT_MS relaxation

Dropping from 8200 to ~3000 is only safe *because* of the stagger — the stagger
guarantees a packet always wakes `receive()`, so the timeout no longer needs to cover
a full period. That's also a free speedup for the app.

## Verification

1. **Bench demux:** `-DFAKE_COLLARS=3` on the spare ESP32 → watch all 3 slots fill on
   the handheld serial, distinct smoothed RSSI per slot.
2. **No cross-contamination:** two transmitters at known, different distances →
   confirm each slot's RSSI is stable and different.
3. **Collision rate:** 30-minute idle run → read the per-collar `dropped` counter.
   Near zero is the pass condition.
4. **Real walk:** 3 collars + app on a real phone (validates MTU + roster read).

## Migration order

1. Flash collars with new binary frame + `-DCOLLAR_ID=0/1/2` (3 boards, labelled)
2. Flash handheld
3. Flash app

Until the handheld is reflashed, old collars keep working via the legacy ASCII path
mapped to collar 0.

## Future work (not in this plan)

- Pin a specific cat to follow (one control characteristic)
- Per-cat tones (distinct pitch per cat, volume = strength)
- Base stations / triangulation (part 3, not yet built)
