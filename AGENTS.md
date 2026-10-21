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
- Match the two firmwares on every radio change. See
  "Air settings (must match the handheld)" in `collar/README.md`.
- **The packet frame has to match too, in three places:** the collar's
  6-byte frame, the handheld's parser and the app's decoder. See "Packet
  format (must match the handheld)" in `collar/README.md` and
  `multi-collar.md`. The frame carries the collar id, so a mismatch does not
  fail loudly - it merges two cats into one row, or hides one entirely.
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