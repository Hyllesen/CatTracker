# Find mode: two-speed collar beacon with a post-ping listen window

Implementation plan for the cat tracker (collar, handheld, phone app). Written to
be handed to a coding assistant.

**Revision 3.** Phase 0a has been run on the bench, and its measurements changed
two design decisions: the command preamble is 8 symbols (a longer one does not
decode at all), and the listen window must cover RxDone at ~148 ms rather than
just the preamble. See `docs/plans/find-mode-bench-findings.md` for the raw
numbers. §11 lists every correction and why it was made, so none of them are
reintroduced. Where a number is an estimate it says so.

## 1. Goal

- Collars ping slowly (default 60 s) most of the time to save battery, and fast
  (default 2 s - §9 shows why 2 s beats 4 s) while the owner is hunting for a
  cat.
- The owner marks a cat as 'find' in the phone app (or with a serial command on
  the handheld for bench tests). The handheld delivers the command over LoRa,
  the collar speeds up for a limited time, then reverts by itself.
- Battery target, stated honestly. Today's 8 s ping gives ~3 days measured on
  this hardware. §9's table, on a 180 mAh cell with two 15-minute find sessions
  a day, gives **~11 days at a 60 s slow interval (find at 2 s), ~15 days at
  120 s, ~17 days at 180 s**. Three weeks therefore needs **SLOW_INTERVAL_S >=
  180 and find at 4 s rather than 2 s** (~21 days); it does not need the
  ultra-low idle current an earlier draft claimed. Pick the trade deliberately:
  60 s is a responsive tracker and ~2 weeks if the find cadence stays at 4 s,
  180 s is ~3 weeks and up to 3 minutes of latency to enter find mode.
- Non-goals: no change to the uplink air settings (433 MHz, SF9, BW125, CR 4/7,
  sync word 0x12), no GPS, no continuous listening on the collar, no ATtiny port
  in this pass (§5).

## 2. Read first

Read these before changing anything, and trust the real code over the names used
in this plan:

- collar/collar.ino (nRF52840, raw SX1278 register driver, no library)
- handheld\_ble.ino (ESP32-C3, RadioLib SX1278, BLE roster)
- homebase/homebase.ino and homebase/esphome/cat-base2.yaml (both hear collars
  and both have their own freshness windows - see §6a)
- homebase/esphome/collar_frame.h and test_collar_frame.cpp (the base's decoder
  and its host test)
- CatTrackApp/ (React Native), tools/monitor.py, tools/test\_monitor.py
- README.md, collar/README.md, multi-collar.md, GLOSSARY.md, AGENTS.md,
  docs/adr/

Four things this plan depends on that are easy to get wrong:

- **Nothing in this repository has ever received on the collar.** Both collar
  builds are transmit-only, and the RA-02 there has **no DIO0 wire** - so a
  receive window means arming `RegOpMode = 0x85` and polling `REG_IRQ_FLAGS`
  over bit-banged SPI, plus FIFO registers the current register table does not
  even define: `0x0D` `RegFifoAddrPtr`, `0x0F` `RegFifoRxBaseAddr`, `0x10`
  `RegFifoRxCurrentAddr`, `0x13` `RegRxNbBytes`, `0x18` `RegModemStat`, `0x19`
  `RegPktSnrValue`, `0x1A` `RegPktRssiValue`. That is the riskiest part of the
  whole plan and it is proved first, in Phase 0a (`collar_rx_probe/`).
- **The handheld's DIO0 *is* wired** (`PIN_DIO0 = 3` in handheld\_ble.ino), so
  the handheld can use non-blocking `startReceive()` + `digitalRead(DIO0)`
  instead of a blocking `receive()` call. That matters once the receive window
  is a whole slow interval long (§6).
- **The existing 6-byte frame** is magic 0xC1, seq, id, battery, flags,
  reserved; the per-collar period offset is `PING_PERIOD_STEP_MS = 137`, and the
  radio is put to sleep (not standby) between pings, which is why every cycle
  starts with a full `radioInit()`: ~35 register writes at ~40 us per bit-banged
  byte plus three `delay(1)` calls, so a few milliseconds before the ping rather
  than the microsecond hand-off the RX window needs. Do not re-init between TX
  and RX.
- **`radioInit()` writes `REG_PREAMBLE` = 8.** That register sets the preamble
  length for both TX and RX. A longer preamble on the *command* frame is fine
  for a receiver configured with 8, and it is the cheap way to make a slow
  handheld turnaround harmless (§3).

Confirm in the code before relying on them:

- Handheld constants tied to the 8 s ping: `COLLAR_PING_MS`, `STALE_MS`,
  `RX_TIMEOUT_MS`. `STALE_MS = 15000` appears in the handheld **and** the base.
- Handheld flash use. The ESP32-C3 build once reached about 99% of the app
  partition, so check headroom after every change.
- The BLE roster is a 20-byte notification with no spare *length* - but the
  flags byte uses only bits 0-3 (§6).
- `tools/monitor.py` reads an ASCII `RPT` line that **the firmware does not
  currently emit** (§7).

## 3. How it works

The collar stays a transmitter that listens for a short fixed window right after
each ping. The handheld has just heard that ping, so it knows the collar is
listening and replies immediately. There is no continuous receive and no extra
wake-up hardware. This is the design's whole advantage: the ping synchronises
the window for free, so the collar pays ~1.1 mAs per cycle instead of 11 mA
continuous, and there is no CAD or wake-radio to build.

Collar cycle:

1. Transmit the ping.
2. Switch straight to receive.
3. Listen for LISTEN\_MS (default 100 ms) for a command preamble.
4. Put the radio to sleep.
5. Sleep the MCU until the next ping.

Listen window details:

- Set `RegFifoRxBaseAddr = 0`, `RegFifoAddrPtr = 0` (0x0D, not 0x0F),
  clear `REG_IRQ_FLAGS`, then `RegOpMode = 0x85` (LoRa | RX continuous).
- Poll `REG_IRQ_FLAGS (0x12)` with 1 ms sleeps. (`REG_MODEM_STAT` is not used -
  see below.)
- **The window must stay open until RxDone. Catching the preamble is not
  enough.** A command with an 8-symbol preamble occupies 143 ms of air, and the
  measured RxDone lands **148 ms** after the collar's TxDone (bench:
  `wait=148.4ms`, §3 of the findings file). A 100 ms window cuts the packet
  halfway and receives nothing, however early the preamble is detected - which
  is exactly the mistake an earlier draft made.
- So: receive for `LISTEN_MS` = **120 ms**, then keep receiving up to
  `MAX_CMD_RX_MS` = **300 ms** only while IRQ bit 4 (`0x10`, **ValidHeader**) is
  set. ValidHeader is raised when a real header has been demodulated (the bench
  logs show it arriving with RxDone as `irq=0x50`), so the extension is free in
  a quiet window and is the only trustworthy "a packet is genuinely arriving"
  signal - by arithmetic the header completes at ~87 ms (50 ms of preamble plus
  ~33 ms of header, plus the 4 ms key-up), inside the 120 ms window and ~60 ms
  before RxDone at 148 ms, which is the margin the extension has to work with.
  Clear the flags before arming so a stale ValidHeader cannot extend a window
  that has nothing in it.
- **Do not use `REG_MODEM_STAT` for this.** The bench measured `0x04` set from
  the first poll of *every* window, including windows with nothing transmitted
  (`msfirst=0x04@0ms`); bit 2 almost certainly means "RX on-going", so every
  mask containing it (0x04, 0x07, 0x0B, 0x0E, 0x0F) is true unconditionally and
  would extend every window to MAX\_CMD\_RX\_MS - tripling the receive cost for
  nothing.
- Fallback, if the ValidHeader extension turns out to be unreliable: a fixed
  200 ms window. It is simpler, and it costs ~2.2 mAs per ping instead of 1.3
  (about 37 uA average at 60 s, ~0.9 mAh/day). Take it only if the extension
  fails, not by default.
- On RxDone, reject the frame if the payload-CRC-error flag (`0x20`) is set
  before using the data. Both CRC bits are already on in `radioInit()`.
- Always put the radio to sleep (op mode 0x80) at the end.

Timing budget, with the airtimes worked out rather than assumed (SF9 at BW125 is
4.096 ms per symbol):

| frame | symbols | airtime |
|-------|---------|---------|
| ping, 8-symbol preamble, 6 bytes | 8 + 4.25 + 22 | **140 ms** (theory) |
| command, 8-symbol preamble, 6 bytes | same | **143 ms** (measured) |
| command, key-up after the ping | | **~4 ms** (measured) |
| command **RxDone**, after TxDone | | **148 ms** (measured) |

The collar's window opens when its own TxDone fires, which is the instant the
handheld finishes decoding. The handheld's measured key-up is ~4 ms, so the
packet *starts* ~4 ms into the window - but it does not *finish* until ~148 ms,
and only RxDone gives you the payload. Size the window against 148 ms, not
against the preamble.

**Use an 8-symbol preamble on the command frame - the same as a ping.** The
first draft recommended 32 symbols as cheap insurance for a slow handheld. That
is wrong, and the bench proved it lethal rather than merely wasteful: a collar
whose `RegPreambleLsb` is 8 decodes a command with an 8-symbol preamble (3 of 3
at -28 dBm) and **decodes nothing at all** at 16, 24 or 32 (0 of 12). The
receiver appears to expect the sync word at the symbol its preamble register
says, so a longer preamble never locks. See
`docs/plans/find-mode-bench-findings.md` §3.

This now costs nothing, because 8 symbols is also the shortest option: the
measured turnaround is ~4 ms, so no timing margin is needed. If a longer preamble
is ever wanted, the collar must write the same value to `REG_PREAMBLE` before
arming RX, and the pair must be re-verified on the bench. A cheap way to confirm
the hypothesis - rather than prove it - is to set the collar's preamble
registers to 32 before arming and resend a 32-symbol command; it changes nothing
about the design either way.

## 4. Protocol

Ping frame, collar to handheld, 6 bytes, backward compatible:

- byte 0: magic 0xC1
- byte 1: seq
- byte 2: collar id
- byte 3: battery percent (0xFF = unknown)
- byte 4: flags. bit0 LOW\_BATT (existing). bit1 FIND\_ACTIVE (new).
- byte 5: was reserved. Now minutes of find mode left (0 = not in find mode,
  saturate at 255). Must be written as 0 whenever find mode is off, so nothing
  downstream reads a stale number.

Byte 5 (and byte 4 bit1) are safe to reuse - verified across all five copies of
the frame: `buildFrame()` writes 0, the handheld's `applyPacket()` reads only
bytes 1-4, `collar_frame.h` ignores byte 5 and `test_collar_frame.cpp` does not
assert on it, and the app never sees the collar frame. Update the frame comment
in `collar/collar.ino`, `handheld_ble/handheld_ble.ino`,
`homebase/esphome/collar_frame.h` and the table in `multi-collar.md` §"Packet
format", so the five copies still describe the same frame.

Command frame, handheld to collar, 6 bytes:

- byte 0: magic 0xC2
- byte 1: target collar id
- byte 2: command. 0x01 FIND\_START, 0x02 FIND\_STOP.
- byte 3: parameter. Duration in minutes for FIND\_START (0 = use
  FIND\_DURATION\_MIN).
- byte 4: command sequence number, incremented per new request (for logs)
- byte 5: reserved, 0

Rules:

- The collar ignores anything with the wrong magic, wrong id, unknown command or
  a CRC error.
- Same radio settings and sync word as the pings (SF9, BW125, CR 4/7, 0x12).

### Inverted IQ (conditional, not phase 2 by default)

The reason to invert IQ is that a collar's receive window can decode *another
collar's* ping and waste up to 300 ms in RX. That is real, but the magic byte
and the target id already make the collar discard it, and a collision is ~1.75%
of ping pairs - so this is worth ~0.5% of the collar's energy. **Only do it if
Phase 2 measures the waste and it matters.**

If it is done, the register values in the first draft were wrong in a way that
would have broken the uplink. The collar sits at `REG_INVERT_IQ = 0x27`, whose
**bit 0 is the *transmit* inversion and is what makes the handheld decode the
pings** (`setRegBits(REG_INVERT_IQ, 0x01, 0, 0)`, and the comment in
`radioInit()` about RadioLib issue #778). Writing `0x66` clears bit 0 and the
collars go inaudible. Do it as a read-modify-write of **bit 6 only**
(`RegInvertIQ` 0x27 -> 0x67), keep bit 0 set, and set `RegInvertIQ2 (0x3B)` to
**0x19** while inverted and back to **0x1D** when normal. On the handheld use
`radio.invertIQ(true)` before transmitting the command and
`radio.invertIQ(false)` before returning to receive - and note that RadioLib
does not reset that state inside `receive()`.

## 5. Collar changes

Build flags (defaults):

- SLOW\_INTERVAL\_S 60 (see §9 before choosing 180)
- FAST\_INTERVAL\_S 2 (4 s saves ~1-2 days of total runtime and is barely better
  than today's 8 s ping; see §9)
- FIND\_DURATION\_MIN 15
- LISTEN\_MS 120 (must cover RxDone at ~148 ms together with the ValidHeader
  extension; see §3 - 100 ms cannot work)
- MAX\_CMD\_RX\_MS 300 (extension ceiling, only while ValidHeader is set)
- FIND\_LISTEN\_EVERY 5
- FIND\_MIN\_BATT\_PCT 10

Behavior:

- Normal mode: ping every SLOW\_INTERVAL\_S and listen after every ping. At a
  60 s interval the window costs ~18 uA average, so there is no reason to skip
  it in slow mode.
- State: mode and findPingsLeft. Set findPingsLeft = FIND\_DURATION\_MIN \* 60 /
  FAST\_INTERVAL\_S and count pings rather than wall-clock time. This is
  drift-free, survives the inaccurate watchdog oscillator on the ATtiny, and
  needs no RTC.
- On FIND\_START: enter find mode, send the next ping after about 2 s as a quick
  confirmation, then ping every FAST\_INTERVAL\_S.
- **Listen after every ping, in both modes** (`FIND_LISTEN_EVERY` = 1). The
  original design listened only every fifth find-mode ping to save current, but
  the arithmetic says the saving is ~0.26 mAh/day while the cost is a real
  failure: the handheld retries on the *next three pings*, and if only every
  fifth has a window all three retries can land outside it. Measured on the
  bench before the change: `stop` failed 3/3, leaving a collar shouting for 15
  minutes. Listening every 2 s cycle costs 1.3 mAs against ~18 mAs of transmit.
- On FIND\_STOP or when findPingsLeft reaches 0: return to normal mode and write
  byte 5 back to 0.
- **The battery gate must check `batteryValid`, not just the percentage.** A
  collar with no divider fitted, or with no cell (which reads ~0.1 V at the pad),
  reports 0% and `batteryValid == false`. Gating on `batteryPct < 10` alone
  makes those boards silently refuse find mode - which is exactly the bench
  board you will test with. Ignore FIND\_START only when
  `batteryValid && batteryPct < FIND_MIN_BATT_PCT`.
- Keep the per-collar period offset in both modes so collars do not lock
  together.
- Always start in normal mode after a reboot. That means a collar that browns
  out mid-hunt silently drops back to slow pings, which from the phone looks
  like the find mode died; the handheld and app must re-request it (§6).
- Set flag bit1 and the minutes-left byte in every ping while in find mode.
- Add a serial command on the collar too (`f` to log one find-mode entry/exit),
  because Phase 1 debugging wants both ends on the wire. Every print stays
  behind `logOk()`, per the AGENTS rule.
- `sendPacket()` leaves the radio in standby with IRQ flags cleared, which is
  the right starting point for the RX window - do not call `radioSleep()` between
  TX and RX.

Optional later, and harder than it sounds: port the same logic to
`at-tiny85-collar/`. It has no `millis()` at all (Timer0/Timer1 are powered down
and the recommended FQBN is `millis=disabled`), so the listen window needs its
own timebase on top of a 1 MHz/8 KB part. The interval there is a count of 8 s
watchdog ticks, so SLOW is a multiple of 8 s and FAST is 4 s (WDTO\_4S) or 8 s.
Treat it as a separate project, not a port.

## 6. Handheld changes

- Per-collar state: findRequested, attempts, lastCmdSeq, findActive, minutesLeft.
- **In the receive handler, before BLE, display or audio work:** if
  findRequested[X] is set and FIND\_ACTIVE is not, transmit FIND\_START
  addressed to X, then return to receive. Use +17 dBm and the **8-symbol**
  preamble from §3 - not 32, which does not decode.
- Clear findRequested[X] when a ping from X arrives with FIND\_ACTIVE set. That
  ping is the acknowledgement. Retry on the next 3 pings, then report failed.
  FIND\_STOP works the same way, and re-sending FIND\_START to a collar already
  in find mode is harmless, so a lost ACK costs nothing.
- **Never touch the radio from the BLE write callback.** NimBLE delivers the
  write on its own task; the callback must only set a flag under `stateMux` and
  let `loop()` do the transmitting.
- **Replace the blocking `radio.receive()` with `startReceive()` + a DIO0 poll
  and a deadline.** With a 60 s (or 180 s) window, a blocking call means the
  handheld cannot service a serial command, blink, or publish anything for a
  whole minute - which is tolerable in the field and miserable at the bench, and
  worse once SLOW grows. DIO0 is wired for exactly this. Keep the deadline
  semantics (`RX_TIMEOUT_MS`) so a ping landing on the edge is not missed.
- Replace the fixed 8 s assumptions. Keep an expected interval per collar:
  SLOW\_INTERVAL\_S normally, FAST\_INTERVAL\_S when FIND\_ACTIVE. Use it for
  stale and signal-lost detection and for the age display. `STALE_MS` must
  become ~2.5x the *slow* interval (150 s at SLOW=60), or a single missed ping
  marks a healthy cat lost.
- Re-request find mode if a collar's find flags disappear while the requested
  minutes have not elapsed. That is the only thing that recovers from a collar
  reboot without the owner noticing.
- The homing tone follows incoming pings, so it speeds up automatically in find
  mode.
- Serial commands for bench testing: `find <id> [minutes]` and `stop <id>`.
- BLE: add a **write** characteristic for commands (id, command, minutes).
  **Done** - `FIND_UUID` = `8e7a1b22-…`, three bytes `[id, cmd, minutes]`, with
  the write callback only setting the request under the mutex (it runs on the
  NimBLE task and must never transmit). Verified without the phone:
  `tools/ble_find.py --id 2 --minutes 2` makes a collar leave slow mode and start
  reporting FIND_ACTIVE in the roster.
- For find *status*, do not add a second notify characteristic. The roster's
  flags byte uses only bits 0-3, so **bit 4 = FIND\_REQUESTED and bit 5 =
  FIND\_ACTIVE** ride along in the existing 20-byte notification, with no version
  bump and no MTU work; an app that does not know the bits ignores them. The
  minutes-left countdown is computed **in the app** from the duration it asked
  for, so it needs no wire byte. The collar's byte-5 minutes stays a log/report
  value.
- While transmitting the command (~239 ms at a 32-symbol preamble) the handheld
  cannot hear pings. That is acceptable; do not "fix" it.

### 6a. The base stations are part of this change

The first draft listed "no base-station changes" as a non-goal. That is wrong,
and it is the failure mode that will be noticed at home rather than at the
bench. Both bases have freshness windows sized for an 8 s ping:

- `homebase/homebase.ino`: `STALE_MS = 15000`, and `COLLAR_PING_MS` /
  `RX_TIMEOUT_MS` (11.2 s) built from the same assumption.
- `homebase/esphome/cat-base2.yaml`: the "in range" binary sensors, whose comment
  says "One stale window is 15 s - one collar ping interval plus slack".

At a 60 s slow interval every base marks every cat out of range for 45 of every
60 seconds, forever, and those connectivity entities flap 3x/minute into the
Home Assistant recorder and dashboard. So:

- set the base stale window to ~2.5x the slow interval, in both the C++ base and
  the ESPHome YAML, and fix the comments that justify the number;
- check the `RX_TIMEOUT_MS` window is still long enough to hear an ATtiny collar
  at 8 s while a find-mode nRF collar pings at 2 s (it is - the window only has
  to cover the *slowest* collar);
- remember the YAML exists twice: the ESPHome dashboard on `m710q` builds from
  `/home/stefan/homeassistant/esphome-config/`, so copy `cat-base2.yaml` **and**
  `collar_frame.h` there;
- confirm the "missing 10 minutes" automations still mean what they meant (they
  do at 60 s; they would need thought at 180 s).

## 7. Phone app and tools

- App: on the followed cat's row add a Find toggle with states Idle, Waiting for
  next ping (up to one slow interval), Active (minutes left), Ending and Failed.
  Tapping again sends stop. **Done** - see `CatTrackApp/src/find.ts` for the
  state machine and the four rules it encodes: a queued stop outranks the
  collar's stale "active"; a countdown that reaches zero reads "ending" rather
  than "finding · 0 min"; a request with no ack after 200 s reads "failed"; and
  each row carries its own toggle, so one cat being hunted never touches
  another.
- The row now carries the find control and nothing else: the range-test
  recorder and the tone mute button are gone (both were ways to make the app
  quietly useless - a silent tracker and a stale CSV), and the battery figure is
  labelled with its 10-minute sample interval because a percentage that looks
  live but is ten minutes old is worse than no percentage.
- **Auto-request find when a walk starts on a followed cat.** At a 60 s slow
  interval the walking view is useless until find mode is entered, and an owner
  who has to discover that will conclude the tracker got worse.
- **`tools/monitor.py` has a prerequisite that is missing.** ADR 0001,
  `monitor.py` and `test_monitor.py` all describe an ASCII `RPT …` line over the
  handheld's USB CDC port, but nothing emits it: there is no `RPT` in
  `handheld_ble/`, and commit d60e030 that added the tools and the ADR never
  touched the firmware. So the handheld's Report emitter has to be written
  before "monitor.py shows the find flag" can mean anything.
- `tools/monitor.py`: show the find flags and minutes left. Note that the `flags`
  column it prints is the *roster* flags byte, so adding bits 4-5 changes every
  value in that column - update the expected strings in `tools/test_monitor.py`
  at the same time, or the parser's own test goes red for the right reason.
- Update GLOSSARY.md, and add docs/adr/0002 explaining the listen window design
  and the rejected alternatives (continuous receive, CAD wake-on-radio, and the
  base-station-ack variant in §10).

## 8. Phases and acceptance tests

**Phase 0a - DONE (2026-11-19).** See `docs/plans/find-mode-bench-findings.md`
for the numbers. Summary: the collar receives over polled SPI with no DIO0; the
handheld's turnaround is ~4 ms; the command preamble must be 8 symbols; the
`REG_MODEM_STAT` extension is unusable; two RA-02s at 20 cm overload each
other. The original brief, kept for context:

**Bench proof of the collar's receive path.** Nothing in the repo has ever received on the collar, and no
DIO0 is wired there, so this is the part that can invalidate the design. The two
throwaway sketches are written:

- `collar_rx_probe/` - pings, then arms RX for `RX_WINDOW_MS` and reports
  `wait=` (its own TxDone to RxDone), the payload in hex, RSSI/SNR, the CRC
  verdict, and the first non-zero `REG_MODEM_STAT` value seen.
- `handheld_ble/handheld_ble.ino` - two new serial commands: `t <id> [min]`
  sends one command frame blind, `a <id> [n]` answers the next n pings from
  `<id>` automatically and prints `setup=` (packet complete to transmit call)
  and `to_key=` (packet complete to carrier up). `a` alone disarms.

Runbook:

```bash
# collar (nRF52840) - note the id: two collars with the same id is silent
arduino-cli compile --fqbn "adafruit:nrf52:feather52840" \
  --build-property "compiler.cpp.extra_flags=-DCOLLAR_ID=2" \
  --output-dir /tmp/collar_rx_probe collar_rx_probe
arduino-cli upload --fqbn "adafruit:nrf52:feather52840" \
  --input-dir /tmp/collar_rx_probe -p /dev/cu.usbmodemXXXX

# handheld (ESP32-C3)
tools/build_handheld.sh

# then, in two terminals
python3 tools/serial_cmd.py v            # collar: start logging (DTR high)
python3 tools/serial_cmd.py v            # handheld: banner (DTR low)
python3 tools/serial_cmd.py "a 2"        # handheld: arm auto-reply for id 2
```

The two commands do different jobs. `a <id>` makes the handheld answer the next
n pings by itself, which is what makes `wait=` repeatable and is the real timing
measurement. `t <id>` sends one frame blind, which is for the plumbing checks
(wrong magic, wrong id, CRC) - and because it lands at an arbitrary moment, arm
the collar's 30 s window with `l` first:

```bash
python3 tools/serial_cmd.py l            # collar: next RX window is 30 s
python3 tools/serial_cmd.py "t 2"        # handheld: one frame, blind
```

Acceptance - all met on 2026-11-19 except the last two, which need a meter and
a decision:

- DONE: the collar decodes the handheld's command frame over polled SPI with a
  valid CRC (`data=C2 02 01 0F 01 00`, `irq=0x50`, rssi -25 dBm at 20 cm).
  A frame whose magic byte is not 0xC2 is ignored by the parser.
- DONE: the turnaround is printed from both ends. `setup=0.81-0.86 ms`,
  `to_txdone=144.0 ms` against a 143.0 ms command airtime, and the collar's
  `wait=148.4 ms` for RxDone.
- DONE: `REG_MODEM_STAT` settles to `0x04` always-on, so it is unusable; use
  the ValidHeader IRQ instead (§3).
- Still open: the RA-02's TX current at 17 dBm, and whether `REG_OCP`'s 60 mA
  limit is clamping it (needs a meter).
- Still open: the real receive-window current, i.e. whether 120 ms at 11 mA is
  what the board actually draws.

**Phase 0b - baseline energy.** Measure idle current, ping current and duration,
and 24 h drain on the current firmware. Idle current is the number that decides
whether any of §9 survives. Two specific questions:

- TX current at 17 dBm, and whether `REG_OCP` (currently a 60 mA trim) is
  clamping it. Try trim 11 (100 mA, RadioLib's default) and compare both the
  current and a fixed-distance RSSI at the handheld.
- Receive current with the window armed, against the 11 mA the model assumes.

**Phase 1 - IMPLEMENTED AND SMOKE-TESTED (2026-11-19).** Both firmwares build
(collar 73,160 B / 8.8 kB RAM, handheld 706 kB / 53% of the app partition) and
run on hardware. What the bench showed, with the collar at `SLOW_INTERVAL_S=10`
so a cycle was quick:

- `find 2 1` -> `[find] sent ... ok` -> collar `[find] START for 1 min = 30
  pings at 2 s` -> pings every ~2.5 s carrying `FIND 1min` -> handheld `[find]
  acked id=2 active, 1 min left`.
- The duration counts down in pings and reverts exactly: 30 find-mode pings,
  then `[find] duration elapsed, back to slow` and the next ping is `slow 0`.
- `stop 2` -> `[find] STOP` -> next ping slow, handheld `[find] acked id=2
  stopped`.
- 6 find/stop cycles: 11 commands transmitted, 0 TX timeouts, 0 failures, every
  START accepted. (Not the full 20 attempts - that needs the phone connected as
  the jitter source, so it is the next session's job, together with the
  receive-window current measurement.)

Two bugs the bench caught that a build cannot: the serial parser took a single
character as the verb, so `find 2 15` was parsed as verb `f` with the argument
`ind 2 15`, i.e. **every find request silently addressed collar 0**; and the
retry-every-3-pings logic above needs a listen window on every ping.

Acceptance - status after the smoke test:

- `find 0` makes the collar switch to fast pings within one slow interval.
- It reverts after the duration with no further command.
- A wrong id and a wrong magic byte are ignored.
- A collar with **no cell fitted** still enters find mode (the `batteryValid`
  gate).
- Normal pings are unaffected, and `seq`/`missed` accounting is unchanged.
- At least 95% of 20 command attempts succeed, both on the bench and at field
  range, **with the phone connected** (the phone is the jitter source that
  matters, so a test without it proves less).
- The measured receive-window current matches the model: 120 ms of RX per ping,
  plus the extension only when a command arrives.
- The listen window adds no more than 10% to the energy of a ping.

**Phase 2 - several collars, and only then inverted IQ.** Two collars pinging at
nearly the same time must not waste their windows on each other, and the
handheld must still hear all of them. Measure the waste first; if the measured
cost is under ~1% of windows, skip inverted IQ entirely, and if it is applied,
verify the uplink still decodes afterwards (that is the register-value trap in
§4).

**Phase 3 - IN PROGRESS.** The app half is done (per-collar Find toggle, roster
bits, write characteristic, `tools/ble_find.py`, 49 tests green). Still open:
the base stations' freshness windows, and the handheld's `RPT` emitter that
`tools/monitor.py` waits for.

The remaining work: Include the base-station windows from §6a and the Report emitter
from §7, and check ESP32-C3 flash headroom against the 99% history.

**Phase 4 - field test and battery.** Run 24 h in normal mode including at least
one find session. Compare measured drain with §9, then choose SLOW\_INTERVAL\_S
from the table rather than from the target, and tune LISTEN\_MS and the preamble
length.

## 9. Energy estimate (to be replaced by measurement)

Calibrated from today's measured "~3 days" on a 180 mAh usable cell: a 140 ms
transmit every 8 s plus a few hundred microamps of idle averages ~2.5 mA, so one
ping costs **~18 mAs** (140 ms at ~130 mA) - not the 90 mA the first draft
assumed. That figure is consistent with the SX1278 datasheet's ~120 mA at
+17 dBm. **Check the OCP setting when the meter is on it:** `radioInit()` writes
`REG_OCP = 0x20 | 3`, i.e. a 60 mA limit, which is *below* the 87-120 mA the
datasheet gives for +17 dBm. If the measured current sits near 60 mA, the limit
is clamping the PA and raising it (RadioLib's default trim of 11 gives 100 mA)
should buy range as well as honesty.

Per ping the receive window is 120 ms at 11 mA = **1.3 mAs** (~18 uA average at
60 s). An earlier draft's table said 1.6 mAh/day for this row, which was a
factor-of-three arithmetic error: 1440 pings x 1.1 mAs is 0.44 mAh/day, not
1.6. Corrected below.

Per day, on 180 mAh usable, with two 15-minute find sessions:

| item | SLOW 60 s | SLOW 120 s | SLOW 180 s |
|------|-----------|------------|------------|
| normal-mode TX (~18 mAs per ping) | 7.2 mAh | 3.6 mAh | 2.4 mAh |
| listen windows (120 ms, 11 mA) | 0.53 mAh | 0.26 mAh | 0.18 mAh |
| idle, **assumed** 150 uA | 3.6 mAh | 3.6 mAh | 3.6 mAh |
| 2 find sessions at 2 s | 4.5 mAh | 4.5 mAh | 4.5 mAh |
| **total / days** | **15.8 / ~11 d** | **11.9 / ~15 d** | **10.7 / ~17 d** |

At a 4 s find cadence the sessions cost 2.3 mAh/day instead of 4.5, and the same
rows become 13.6 / 9.7 / 8.5 mAh/day - about **13, 18 and 21 days**. So:

- **60 s with an aggressive 2 s hunt is ~11 days, not the ~2 weeks an earlier
  draft implied**; 60 s with a 4 s hunt is ~13 days.
- **Three weeks is reachable at 180 s with a 4 s hunt (~21 days)**, and does not
  need an ultra-low idle current - the claim that it did came from the bad
  receive row.
- The 200 ms fallback window in §3 costs ~0.9 mAh/day at 60 s and nothing
  measurable at 180 s.
- The idle current still matters as much as the radio. Every 50 uA costs
  1.2 mAh/day, which is worth 1-2 days of total runtime. Measure it in Phase 0b
  before tuning anything else.
- At 60 s the transmit energy alone (7.2 mAh) plus two 4 s find sessions
  (2.3 mAh) already exceeds a 21-day budget of 8.6 mAh/day, so no amount of
  idle-current work gets 60 s to three weeks.

## 10. Risks and open questions

- **The collar's receive path is unproven and has no DIO0.** Phase 0a exists
  because of this; the design is dead if polled RX with a bit-banged bus does not
  work reliably.
- **A 100 ms listen window cannot receive a command** (measured RxDone at
  148 ms after TxDone). LISTEN_MS 120 with a ValidHeader extension, or 200 ms
  fixed, or nothing works. §3.
- Handheld turnaround is **measured, not assumed** (2026-11-19, handheld id 2,
  Chacha's collar pinging): processing from packet-complete to the `transmit()`
  call is 0.81 ms, and packet-complete to TxDone is 242.7 ms against 238.6 ms of
  command airtime, i.e. the carrier is up ~4 ms after the ping ends. That is
  7x better than the 30 ms this plan budgeted, so the 100 ms window has room to
  spare even with the phone connected. Re-measure if the BLE notify path grows,
  and keep the 32-symbol preamble anyway - it costs the handheld only.
- A command only reaches the collar if the handheld hears the ping, so out of
  range means no delivery. The next ping tries again.
- Find mode starts up to one slow interval after the request, which is why the
  app must show "waiting" and why SLOW is a responsiveness decision, not only a
  battery one.
- A collar reboot drops find mode silently; the handheld re-requests it (§6).
- Slowing the collar down without changing the base stations makes every cat flap
  in and out of range at home (§6a).
- Do not write `0x66` to `REG_INVERT_IQ`; it clears the transmit-inversion bit
  that makes the uplink work (§4).
- **The PA may be current-limited below its rated 17 dBm.** `REG_OCP` is set to
  a 60 mA trim, against 87-120 mA for +17 dBm. If Phase 0b measures ~60 mA, the
  clamp is real and raising it may improve range as well as the energy model.
- **Bench tests must not put two RA-02s nose to nose.** At 20-30 cm the collar
  saw -14 dBm from the handheld and decoded nothing; at a metre-plus it saw
  -28 dBm and decoded everything. A distant transmitter that "cannot be heard"
  from 20 cm is usually being flooded, not broken.
- Later idea: the base station acknowledges pings, and the collar stays slow
  while acknowledged and goes fast when the acknowledgements stop. It can reuse
  the same listen window, but it needs inverted IQ on both ends (so collars do
  not hear each other's acks) and a base station that transmits - a separate
  project.
- Check every register detail above against the SX1278 datasheet and RadioLib
  before relying on it. The two known-wrong values in the first draft
  (`0x66`, mask `0x0B`) are recorded in §11 for a reason.

## 11. What changed in revision 2, and why

- **"No base-station changes" removed.** Both bases have 15 s freshness windows
  sized for an 8 s ping; at 60 s they flap every cat in and out of range. This
  is now §6a and part of the work.
- **Inverted IQ: `0x66` -> bit 6 only (0x27 -> 0x67), IQ2 0x19/0x1D.** The
  first draft's value cleared bit 0, which is the transmit inversion the uplink
  depends on, so it would have made every collar inaudible. Also demoted from
  phase 2 to "only if measured to matter".
- **`FIND_MIN_BATT_PCT` must gate on `batteryValid`.** A board with no cell or
  no divider reports 0% and would silently refuse find mode - including the
  bench board.
- **`REG_MODEM_STAT` mask `0x0B` -> `0x0E`** (bit 3 detected, 2 synchronised, 1
  RX on-going), with a fixed-window fallback.
- **Energy rebuilt from the measured 3-day baseline** instead of an assumed
  90 mA. The target is restated: 60 s gives ~12 days, three weeks needs SLOW
  >= 180 s and ~100 uA idle. FAST 2 s is recommended over 4 s.
- **`radio.receive()` -> non-blocking `startReceive()` + DIO0 poll.** A blocking
  minute-long window blocks serial, LED and publishing.
- **Find status goes in roster flags bits 4-5, not a new notification**, and the
  countdown is computed in the app; the plan needs a BLE *write* characteristic
  but no new notify one.
- **Phase 0 split into 0a (collar RX proof + turnaround + current) and 0b
  (baseline energy)**, because the RX path is the risk that can invalidate
  everything else.
- **`tools/monitor.py` work is blocked** on a `RPT` emitter the firmware does not
  have; recorded in §7.
- **32-symbol command preamble** added, and **auto-find on walk start** added to
  the app section.

## 12. What changed in revision 3 (after the bench)

- **A 100 ms window cannot receive a command.** Measured RxDone is 148 ms after
  the collar's TxDone (143 ms air + 4 ms key-up), so the window must stay open
  until RxDone, not until the preamble is detected. LISTEN_MS is now 120 ms with
  a ValidHeader (`0x10`) extension to 300 ms; a fixed 200 ms is the documented
  fallback.
- **The command preamble is 8 symbols, and the 32-symbol idea is dead.** A collar
  whose preamble register is 8 decodes 8 symbols (3 of 3) and nothing at 16, 24
  or 32 (0 of 12). The 4 ms turnaround means no timing margin is needed anyway.
- **`REG_MODEM_STAT` is out.** Bit 2 is asserted from the first poll of every
  window, so every mask containing it (0x04/0x07/0x0B/0x0E/0x0F) is useless.
  ValidHeader is the signal to extend on.
- **The receive row of the energy table was wrong by 3x** (1.6 -> 0.53 mAh/day at
  60 s). With the corrected row: 60 s is ~11 days at a 2 s hunt, 120 s ~15,
  180 s ~17, and ~21 days at 180 s **with a 4 s hunt** - which retires the
  earlier claim that three weeks needed ~100 uA idle current.
- **`REG_OCP` is now an open question in §9/Phase 0b.** It is set to a 60 mA trim
  against 87-120 mA for +17 dBm, so the PA may be current-limited below its
  rated output.
- **Bench discipline**: never test with two RA-02s closer than about a metre
  (-14 dBm at 20 cm overloads the receiver and looks exactly like a dead
  transmitter).
