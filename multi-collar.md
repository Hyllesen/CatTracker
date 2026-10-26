# Multi-collar setup — as built

Extending the tracker from one collar to three, with the app showing all cats
at once.

**Status: implemented.** The firmware, the app and the tools are all in
place. This document records what was decided and why, so the reasoning
survives the code that was written from it. Where the plan below changed
during implementation, the change is called out.

## The system

A three-tier point-to-point chain with no backend, no server, and no database:

```
collar 0 (nRF52840) --LoRa 433 MHz--> handheld (ESP32-C3) --BLE GATT notify--> phone app
collar 1 (nRF52840) --                                     (one roster of all three)
collar 2 (nRF52840) --
```

The collars are one-way: a collar transmits and never hears. It has no idea
how many other collars exist, which is what makes the whole design possible —
every collar has to be independently valid, with nothing to negotiate.

There was no addressing to invent. Raw SX1278 LoRa has no node addressing
primitive and none is emulated. The "pairing" mechanism is a table of
air-interface constants that must be byte-identical in three firmware files
(`collar/README.md`, "Air settings"), plus the packet frame described below,
plus a copy-paste obligation documented in `AGENTS.md`.

## Design decisions

### 1. Collar ID is a small integer, not a hardware-derived one

The ID is `0`, `1`, or `2`, set at flash time with `-DCOLLAR_ID=0|1|2`.

**Why not derive from `NRF_FICR->DEVICEID`?** With 3 collars in 3 slots,
computing the slot as `id % MAX_COLLARS` is a birthday problem — a 1-in-3
chance two collars share a slot and collide forever. Hardware-derived IDs
cannot avoid this because collars cannot coordinate to avoid it.

**What this buys:**
- The array index *is* the ID — no map, no find-or-create, no lookup
- A name the app can show without a provisioning table

**The cost** is reflashing to reassign a collar to a different cat. Partly
paid back by logging the chip's factory `DEVICEID` at boot — so you can
confirm "I just flashed Miso's collar, not the spare" by reading one line of
serial, and writing it on the enclosure.

### Keeping track of which collar is which

With three boards it stops being obvious which physical collar carries which
id, and "I flashed the wrong one" is invisible until two cats merge into one
row on the phone.

The chip's factory device id settles it, and it is free:

```
Device id: 0x8258FA98      <- DEVICEID[0], low
            0x43241A31      <- DEVICEID[1], high
```

Those same two words, **in the opposite order**, are the board's **USB serial
number** — the nRF derives one from the other. So a collar can be identified
*before the firmware is on it*, from the USB serial number alone, and the two
can be checked against each other afterwards. If the `v` output and the
operating system disagree about the device id, they are not the same board.

Fill this table in as each collar is flashed. It is the only record of which
id went onto which board that survives a reflashing:

| Collar id | USB serial number | Device id (low / high) | Cat | Enclosure |
|------------|-------------------|------------------------|-----|-----------|
| 0 | `43241A318258FA98` | `0x8258FA98` / `0x43241A31` | Chichi | |
| 1 | `0D9C35E528ADA721` | `0x28ADA721` / `0x0D9C35E5` | Onanda | |
| 2 | `3E31C88249ACB575` | `0x49ACB575` / `0x3E31C882` | Chacha | |

The USB serial number is what the operating system shows, so it is also what
`system_profiler SPUSBDataType` prints — no serial port, no firmware, no
power cycle needed.

The two boards above have both been flashed and verified against the golden
register values in `collar/README.md`. Collar 1 arrived already running the
old single-collar firmware (ASCII payload, hardcoded `CAT-01`), so it was
flashed over USB CDC to move it onto the binary frame; the reflash preserved
the radio settings, and the register dump after it was identical to the dump
before it.

### 2. Binary wire frame, not extended ASCII

Replacing the ASCII payload with a 6-byte binary frame.

**Why:** at SF9/BW125 the payload rounds to whole symbols, so 5 and 6 bytes
cost exactly the same airtime. The binary frame is **140 ms** vs **~230 ms**
for the old 13-byte ASCII — a 90 ms saving that drops collar average draw
to roughly 1.9 mA, because transmit now dominates the collar's power budget
after the fixed draws were removed.

**Why a magic byte instead of a CRC:** LoRa already has a mandatory payload
CRC. Adding another buys nothing. Byte 0 instead rejects stray decoded frames
so garbage never becomes a phantom cat.

**Backward compatibility:** an old ASCII collar sends `'C'` = `0x43` ≠ `0xC1`
and is ignored. The handheld additionally accepts legacy ASCII for one
release, mapped to collar 0, so cats can be swapped one at a time instead of
opening all three enclosures at once.

### 3. A different ping period per collar, not a fixed phase

**This replaced the plan's TDMA phase stagger, and the reason matters.**

The original plan had every collar ping every 8 s, with collar *n* waiting
`n × 8/3` seconds before its first ping. That is only collision-free if every
collar happens to boot at the right moment relative to the others — and in
practice the collars are flashed and installed one at a time, days apart. The
phases are then whatever the installation order happened to produce.

The failure mode is not "a few lost packets". If two collars land within one
140 ms airtime of each other, they collide **every cycle**, and those two
cats are invisible to the handheld *permanently*. The chance of that is
roughly 280 ms / 8000 ms ≈ 3.5% per pair, so about 10% across three collars
— and it does not announce itself: the app would just show two cats and the
third one missing.

The fix is to give each collar its own period:

```
period(id) = 8000 ms + id × 137 ms
```

The relative phase then drifts by 137 ms every cycle, so any accidental
alignment moves apart on its own and no pair can be stuck. The price is a
constant ~1.75% collision probability per pair per cycle — about 5% of
packets lost across three collars — instead of a 0% chance of success and a
3.5% chance of catastrophe. The cadences differ by at most 0.3 s between
cats, which is imperceptible.

137 ms is not a round number and does not divide 8000, so the three periods
do not re-align on a short cycle.

### 4. App shows all cats at once

The app renders a list with a signal bar per cat, closest highlighted. This
answers "which one ran off" directly — the actual jungle problem.

**Tapping a row follows that cat.** The big number above the list becomes
*that* cat's signal, and the tone follows it too — the beep and the reading
have to agree about which cat you are walking towards, or the app sends you
the wrong way. With nothing focused, the app follows the closest cat, which
is what you want while wandering around; tap a row to pin it to one cat, tap
it again to let go.

**Tone policy:** the beep follows the cat being followed — the focused one,
or the closest one if nothing is focused. With one collar a beep per packet
is a beep per beacon, which is a useful cue. With three collars it would be
three times as often — and an audio cue that fires constantly stops meaning
anything. The pitch still maps to distance, so the existing 10-WAV tone bank
is untouched.

**Renaming is on a pencil at the end of each row**, not on the row itself.
Following a cat is something you do constantly; renaming is something you do
rarely. A tap that meant two different things depending on which part of the
row you hit would be a trap.

### 5. Cat names in the app

No AsyncStorage dependency, but `react-native-file-access` is already present
— so a small `cats.json` in DocumentDir costs zero new deps
(`CatTrackApp/src/cats.ts`). Tap a row to rename it; ids default to
"Cat 1/2/3" so it's usable with zero setup.

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

Airtime: **140 ms** at SF9/BW125 (symbol time 2⁹/125 kHz = 4.096 ms; payload
symbols = ceil((8×6 − 4×9 + 28 + 16)/36) = 2; ToA = (8 + 4.25 + 2×11) × 4.096
= 140.3 ms).

The `seq` counter enables a **collision counter on the handheld** — the
number that tells you whether three collars is actually working in the
field. It counts packets we never heard, which is an upper bound on
collisions: a packet can also be lost for other reasons.

### Handheld → app (20 bytes)

Same characteristic UUID (the app's UUIDs do not change), readable *and*
notifiable:

```
byte 0      version = 0x10
byte 1      count   (0..3)
bytes 2..19 per collar, 6 bytes, in id order - the position IS the id:
  +0  flags   bit0 HAVE, bit1 FRESH, bit2 STALE, bit3 LOW_BATT
  +1  rssi*10 int16 LE (dBm)
  +3  snr*2   int8 (dB, 0.5 dB steps)
  +4  age     seconds since last packet, capped 255
  +5  battery 0..100 or 255
```

**This is smaller than the plan's 26 bytes, on purpose.** 26 bytes exceeds
the 20-byte payload of the *default* ATT MTU of 23, so it would only work if
MTU negotiation succeeded on the phone. NimBLE truncates an oversized
notification to MTU-3 rather than failing, so the phone would receive a
mangled packet and show nothing at all — the same class of invisible failure
this project keeps hitting. Fitting the default MTU means the roster works
whether or not the handshake succeeded.

That is what the six bytes per collar bought:

- **The id is the position**, not a byte. Carrying it explicitly would have
  cost a byte per collar and pushed the roster to 26.
- **The signal percentage is not on the wire.** It is a straight function of
  the RSSI, so the app derives it from `RSSI_CEIL`/`RSSI_FLOOR` in
  `CatTrackApp/src/signal.ts`. The mapping is a display concern, so it lives
  with the display — and the two firmwares never have to agree about it.
- **The seq counter stays in the handheld.** The app has no use for it; the
  handheld's serial diagnostics do.

The version byte is `0x10` rather than `0x02` because the legacy
single-collar packet started with a flags byte that only ever used bits 0-2,
so a value with bit 4 set cannot be one. The two formats are told apart by
the first byte alone, which is what lets the app talk to a handheld that has
not been reflashed yet.

Two further safety measures:

- **Readable characteristic.** NimBLE characteristics are readable by default,
  so a phone that connects late does `read()` for a full roster snapshot and
  *then* subscribes. Fixes "I only ever see collar 2" for free.
- **Length validation in `decodeRoster()`** — return `null` if
  `2 + count×6 > bytes.length`, so a truncated notification is rejected
  outright rather than read as a roster that is missing a cat.

## File-by-file changes

| File | Change |
|---|---|
| `collar/collar.ino` | Binary frame builder, `COLLAR_ID` build flag, per-collar period, `DEVICEID` in the `v` log |
| `handheld_ble/handheld_ble.ino` | `CollarState[3]`, binary + legacy parser, per-collar latest-packet RSSI/staleness, 20-byte publisher, `RX_TIMEOUT_MS` 8200 → 8474 |
| `transmitter/transmitter.ino` | Binary frames + `-DFAKE_COLLARS=n` to fake 3 cats from one spare board |
| `CatTrackApp/src/signal.ts` | `decodeSignal` → `decodeRoster` returning `Roster { collars: Collar[] }`; `decodeLegacy` kept for the old packet |
| `CatTrackApp/src/ble.ts` | `readRoster()` on connect |
| `CatTrackApp/src/cats.ts` | New: read/write `cats.json` in DocumentDir |
| `CatTrackApp/App.tsx` | Roster state, one row per cat, per-collar tone policy + stall detection |
| `CatTrackApp/src/logging.ts` | CSV gains an `id` column (one file, mixed IDs) |
| `CatTrackApp/__tests__/signal.test.ts` | Decoder tests ported to the roster format |
| `tools/analyze_walk.py` | Reads the `id` column, reports per collar |
| `collar/README.md` | Documents the frame + the ID/period rule |
| `AGENTS.md` | "Must match" rule now covers the frame, not just air settings |

The serial-only `handheld/` sketch was deleted before this work: the app
talks to `handheld_ble/`, and keeping two receivers meant two places for the
frame to drift out of sync.

### Handheld state

```cpp
struct CollarState {
  bool     have;
  uint8_t  id;
  uint8_t  seq;
  bool     seqValid;
  uint8_t  batteryPct;
  uint8_t  flags;
  float    rssiHistory[SMOOTH_N];   // 12
  uint8_t  idx, count;
  float    rssiSmoothed;
  float    snrLast;
  uint32_t lastMs;
  uint32_t packets;
  uint32_t missed;     // seq gaps = packets never heard
};
CollarState collars[MAX_COLLARS];  // index == id
```

~700 bytes total. Negligible on a C3 with 400 KB SRAM. Unknown ID (≥
MAX_COLLARS) → drop + count.

### RX_TIMEOUT_MS relaxation

`RX_TIMEOUT_MS` is now derived from the **slowest** collar's period:

```cpp
const uint32_t COLLAR_PING_MS = 8000 + (MAX_COLLARS - 1) * 137;
const uint32_t RX_TIMEOUT_MS  = COLLAR_PING_MS + 200;
```

The window no longer has to cover a full period for a single collar, because
with three collars there is always something arriving — but it still has to
cover the slowest one, or that cat loses half its packets.

## Verification

1. **Bench demux:** `-DFAKE_COLLARS=3` on the spare ESP32 → watch all 3 slots
   fill on the handheld serial, distinct smoothed RSSI per slot.
2. **No cross-contamination:** two transmitters at known, different distances →
   confirm each slot's RSSI is stable and different.
3. **Collision rate:** 30-minute idle run → read the per-collar `missed`
   counter. A few percent is expected; tens of percent means two collars are
   stuck on the same phase and their periods need checking.
4. **Real walk:** 3 collars + app on a real phone (validates the roster read
   and the 20-byte notification against the phone's real MTU).

### Checking the handheld's half without a phone

`tools/ble_roster.py` connects to the handheld as a BLE client and decodes
the roster exactly as the phone does:

```bash
python3 tools/ble_roster.py 30
```

It is worth having as a separate tool because the phone is a poor instrument
for this. An app that shows nothing looks identical to an app that is not
connected, and there is no way to tell "the handheld is not publishing" from
"the phone is not listening" — the whole class of failure this project keeps
hitting. Here the bytes are visible.

A working roster, measured with one collar in range:

```
READ (the snapshot the app fetches on connect): 20 bytes
  10 03 00 00 00 00 00 ff 03 04 ff 1b 00 5f 00 00 00 00 00 ff
  id=0  (never heard)
  id=1  rssi -25.2 dBm  snr 13.5 dB  age 0s  battery 95%  FRESH
  id=2  (never heard)
```

Twenty bytes exactly, version `0x10`, count `0x03`, and the three six-byte
entries in id order — the unheard collars carry flags `0x00` and battery
`0xFF`. The number of FRESH collars over a minute is the beacon rate, and it
should match the packet count on the handheld's own serial.

## Migration order

1. Flash the collars with the binary frame and `-DCOLLAR_ID=0/1/2`, and label
   each one.
2. Flash the handheld.
3. Flash the app.

Until the handheld is reflashed, old collars keep working via the legacy ASCII
path mapped to collar 0. Until the app is reflashed, the new handheld's roster
does not look like the old packet and the app shows nothing — so do the
handheld and the app together.

### Flashing a set of collars

Two rules, both of which exist because the failure is invisible:

- **Compile before every upload, and pin the build.** The id is a compile-time
  flag, and `arduino-cli upload` does not compile — it sends the artifacts of
  the last build of that sketch. So `compile --build-property ... --output-dir`
  followed by `upload --input-dir` is the form that cannot go wrong; without
  the pinning, a stray compile in between puts the wrong id on a board with no
  error at all.
- **A board with the same id twice is a phone-level mystery**, not a radio
  one: both collars transmit perfectly and the handheld cannot tell them
  apart, so the app shows one cat where there are two. The id/device-id table
  above is what catches it.

The per-board recipe, including the bootloader route for a blank board and
the DTR trap that makes a working collar look dead, is in
`collar/README.md` under "Bringing up a new collar".

## Future work (not in this plan)

- Per-cat tones (distinct pitch per cat, volume = strength)
- Channel-activity detection on the collar, to cut the ~5% collision rate
  further at the cost of a more complicated collar firmware
- Base stations / triangulation (part 3, not yet built)
