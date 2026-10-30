# Cat Tracker — agent notes

## Hardware reference: read this before advising on the collar board

The collar is a **TENSTAR nRF52840 Development Board** (Pro Micro form factor;
branded SAMIROB / FEIYANG on the silkscreen and packaging).

**Primary source — the TENSTAR nRF52840 Development Board User Manual:**
<https://manuals.plus/ae/1005007738886550>

Note: that URL currently returns 403 to scripted fetches. Use a web search for
the title if you need to re-read it, and prefer this file's summary for
routine questions so we are not guessing.

Secondary, for the Nice!Nano/Pro Micro clones these boards are based on:
<https://www.beachyuk.com/blog/connecting-and-testing-promicro-nrf52840-clones>

### Confirmed specs

| Question | Answer |
|----------|--------|
| Cell connection | `+` → `B+`, `−` → `B-` |
| Charge rate | **100 mA** onboard (good for a 150 mAh cell — ~0.67C) |
| Rail | 3.3 V LDO; `VCC` pad reads ~3.3 V |
| Bootloader | **Short `RST` to `GND` twice within 0.5 s** |
| Bootloader appears as | USB mass storage (`NICENANO` on clones; `CURRENT.uf2`, `INDEX.HTM`, `INFO_UF2.TXT`) |
| Flashing | Drag a `.uf2` onto the drive, or `arduino-cli` over USB CDC |
| Power pins | `B-`, `B+`, `RAW`, `GND`, `RST`, `VCC` |
| Bootloader LED tell | **Fades** in bootloader; the application leaves it **dark** |

There is no BOOT button to press on this board — the `RST`-to-`GND` double
short is the way in, and it is why bootloader entry looked unavailable.

### Pin silkscreen

Pads are labelled with **raw nRF port numbers** (`002`, `011`, `029`, `104`…),
not Arduino aliases. `029` means P0.29, `104` means P1.04. The firmware's pin
constants and the pad labels are therefore the same number — what you solder to
is what you write in code.

The manual labels `009`/`010`/`011` as SDA/SCL/INT, while Nice!Nano-based
documentation labels `009`/`010` as D9/D10 and `011`/`104` as D7/D8. Both agree
on the underlying pin number; only the *function name* differs, so trust the
number and ignore the function label.

### Arduino build

- Board: `adafruit:nrf52:feather52840` (the Adafruit core enumerates this clone)
- The collar is flashed over USB CDC with the `nrfutil` upload protocol
- See `collar/README.md` for the complete wiring table and build notes

## Project rules

- **Never enable unconditional `Serial.print` on either board.** Serial is
  native USB CDC; powering from a charger or power bank means nothing drains
  it, so the write blocks and silently stops the board. Logging defaults off and
  is enabled by sending `v`. See `collar/README.md`.
- **In the handheld, every print is gated on `hostAttached()`, which is
  `Serial.isPlugged()`.** That turns the rule above from a habit into a
  structure: a board with no cable has no Host, prints nothing, and cannot wedge.
  `isPlugged()` is the USB SOF watchdog, **not DTR** — the handheld must run with
  DTR low because asserting it resets the board, so a DTR-based "host is here"
  signal is not available. `setup()` sets `Serial.setTxTimeoutMs(0)` so a Host
  that opens the port and stops reading cannot stall `loop()` either. Do not add
  an ungated print, and do not gate on `Serial.isConnected()` (that is false
  until the host has *read* bytes).
- **The Report line is a fourth copy of the collar state** (collar frame, roster,
  app decoder, Report) and it must stay in step with `tools/monitor.py`. It is
  `RPT <ver> t= pk= unk= ok= fail= pc= ps= cN=r10/s2/batt/packets/missed/age/flags`,
  integers in the roster's own scaling (dBm × 10, dB × 2). A mismatch is silent:
  the terminal prints nothing, which looks exactly like a handheld that has not
  heard a collar. `python3 tools/test_monitor.py` replays the firmware's exact
  bytes through the parser, so change the format string in the firmware *and* in
  the test. See `docs/adr/0001`.
- **Nothing can find the handheld by probing for its banner any more.** The
  banner is printed only when a Host is on the cable, and the OS enumerates USB
  at plug-in time, so it is written to a port with nobody reading it and dropped.
  `tools/monitor.py`, `tools/log_dbm.py` and `tools/serial_cmd.py` find the board
  by USB vendor id `0x303a` (the collar is `0x239a`). Do not reintroduce a banner
  probe: it now returns nothing.
- **`tools/monitor.py` opens the port with DTR low**, and finds the port by USB
  vendor id `0x303a` rather than by the `usbmodem` suffix, which is shuffled
  between reboots. Asserting DTR restarts the board and drops the phone's BLE
  connection, so a run of the monitor is not a harmless act of curiosity — it
  resets the handheld. The firmware does not need DTR, so keep it low.
- Match **all four** firmwares on every radio change: `collar/`, `handheld_ble/`,
  `homebase/homebase.ino` and `homebase/esphome/cat-base2.yaml`. See "Air settings
  (must match the handheld)" in `collar/README.md`. A homebase built with different
  air settings receives nothing, and that failure is indistinguishable from a dead
  module. In the ESPHome build the settings are the `sx127x:` block, and `crc_enable:
  true` is part of them: the collar switches the payload CRC on and sends no
  checksum of its own, so without it a corrupted frame is delivered as a cat.
- **The packet frame has to match too, in four places:** the collar's
  6-byte frame, the handheld's parser, the base station's decode
  (`homebase/esphome/collar_frame.h`) and the app's decoder. See "Packet
  format (must match the handheld)" in `collar/README.md` and
  `multi-collar.md`. The frame carries the collar id, so a mismatch does not
  fail loudly - it merges two cats into one row, or hides one entirely. The
  base station's copy is header-only and has a host test:
  `c++ -std=c++17 -o /tmp/t homebase/esphome/test_collar_frame.cpp && /tmp/t`.
  Change the decode and the test together, or the only symptom is an alert that
  fires weeks later or does not fire at all.
- **A base station's name is its identity on the broker, so never reuse one.**
  `cattracker/base1/#` and the `homeassistant/…/base1_*` discovery records are
  retained, and base 1's "missing 10 minutes" automations fire on the MQTT entities
  they created. A second board must be `base2` (as ESPHome: `esphome: name:
  cat-base2`) or the two overwrite each other's state on the same topics and a
  dead base still looks alive. In Home Assistant an ESPHome entity's unique id is
  rolled from the board's MAC and the object id, **not** from the device name, so
  renaming `esphome: name:` re-adopts the same entities and leaves their
  `entity_id`s frozen at the old name - retarget them in the entity registry, or a
  device called "Cat Base 2" owns `sensor.cat_base_1_…` forever.
- **`homebase/esphome/cat-base2.yaml` exists twice, and the second copy is the one
  that runs.** The ESPHome dashboard on `m710q` builds from its own config
  directory, `/home/stefan/homeassistant/esphome-config/` (root-owned, mounted as
  `/config` in the `esphome` container), and it will build a stale copy without
  complaining. Edit the repo file, then copy `cat-base2.yaml` **and
  `collar_frame.h`** into that directory - the header resolves next to the YAML, and
  a missing header is a build failure, not a flash failure. A device missing from
  the dashboard means its YAML is not in that directory, not that the board is
  offline.
- **Each collar is flashed with its own id** (`-DCOLLAR_ID=0|1|2`). Two
  collars with the same id are invisible as separate cats. `arduino-cli
  compile` has to run before each `upload`, because the id is baked into the
  binary.
- The roster the handheld publishes is capped at 20 bytes on purpose - that
  is the payload of the *default* ATT MTU. NimBLE truncates anything longer
  rather than failing, so an oversized roster reaches the phone mangled and
  the app shows nothing at all. See `multi-collar.md`.
- **The two boards need opposite DTR states, and getting it wrong is
  silent.** The ESP32-C3 handheld must have DTR low (asserting it resets the
  board); the nRF52840 collar must have DTR high (with it low, its USB CDC
  delivers nothing in either direction - no output and no keystrokes - so the
  board looks dead while it is happily transmitting). `tools/serial_cmd.py`
  picks the right one from the USB vendor id. See "Commands" in
  `collar/README.md`.
- **The chip's device id is the board's USB serial number.** The `v` output
  prints `DEVICEID[0]` then `DEVICEID[1]`; the USB serial number is the same
  two words in the opposite order. So a collar can be identified before it is
  flashed and the two can be checked against each other afterwards. Keep one
  table of collar id -> USB serial number -> cat; it is the only record of
  which id went onto which board. See `multi-collar.md`.
- **Healthy collar register values, measured on a verified board:** `FRF
  0x6C4000` (433.000 MHz), `MC1 0x76`, `MC2 0x94`, `Sync 0x12`, `InvIQ 0x27`,
  `PA 0xFF`, and `RegVersion 0x12`. An `MC1`/`MC2` off by a nibble is a board
  built with different air settings: it transmits fine and is invisible to
  the handheld, which looks exactly like a dead radio. The table with the bit
  decoding is in `collar/README.md` under "Troubleshooting".
- **A collar with no cell fitted reads a tenth of a volt** at the `029` pad,
  and the firmware reports that as "no reading" rather than 0%. The `[BATT]`
  line prints the reconstructed *cell* voltage (pin × 2), so a healthy 4.1 V
  cell reads `~4100mV` there and `~2.05 V` at the pad. Do not mistake the
  no-cell case for a flat cell.
- `arduino-cli upload` does NOT compile - it sends the artifacts of the last
  build of that sketch, from arduino-cli's per-sketch cache. `--build-property`
  is compile-only. Use `compile --output-dir` plus `upload --input-dir` to pin
  the build: otherwise a stray compile can put the wrong collar id on a board
  with no error at all.
- **The handheld's half of the link can be checked without a phone** with
  `python3 tools/ble_roster.py`, which connects over BLE and decodes the
  roster the way the app does. Use it rather than the phone to tell "the
  handheld is not publishing" from "the phone is not listening" - the two look
  identical from the app. A healthy roster is 20 bytes, version `0x10`,
  count `0x03`, three six-byte entries in id order.
- **The app's render tests are real now.** `react-native-safe-area-context`
  has to be mocked with its own jest mock, or `SafeAreaProvider` renders no
  children at all and every render test passes while drawing an empty
  screen. `__tests__/app_roster.test.tsx` drives the whole path - mocked
  BLE, real roster bytes, press Connect, press a row - so a change to what
  the app shows is caught by a test rather than by eye.
- **`arduino-cli upload` to a collar can fail with "Target is not in DFU
  mode" and succeed on the second run.** The reset into the bootloader is a
  1200-bps touch and it is asynchronous; the first attempt can get there
  before the board has reset. The touch has already happened, so retry. It
  is not a wiring or build problem.