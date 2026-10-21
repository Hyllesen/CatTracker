This project is about making a lora based tracking system for felines (cats).

The project is located in a jungle area in Philippines, so we cannot use internet/mobile data based solutions.

Hence why we will be using a LORA based radio location system.

The system will be composed of 3 parts.

1. Cat collar tracking device. This device will be attached to the cats collar, and tracks it where abouts as it is roaming around. This collar needs to be lightweight and
   have good battery life. This will be utilizing a TENSTAR ROBOT NRF52840 Pro Micro and a RA-02 module. And a 150mA battery.
2. Handheld homing device. This device will be operated by a human to locate the cats by strength of radio signal. Will be using a esp32-c3 and a ra-02 module. Can use a 18650 battery or similar for power. Will also have a speaker and screen for visual/audio feedback.
3. Base stations. These will serve as a network of stations to receive pings from cat collar, to continually track the cats as they are roaming around. Perhaps using
   triagulation to pinpoint the cats exact location on a map.

For now, this project is all about getting part 1 and 2 up and running. Part 3 of base stations will come later as I will receive more electronic components for the project.

## Multi-collar

Up to three collars can run at the same time. Each one is flashed with its own
id and carries it in every packet, so the handheld keeps them apart and the
app shows one row per cat.

```bash
# Compile with the id, then upload that exact build.
# `upload` does not compile - it sends the last build of that sketch, so
# --input-dir pins which one. Without it, a stray compile can put the wrong
# id on the board with no error at all.
arduino-cli compile --fqbn "adafruit:nrf52:feather52840" \
  --build-property "compiler.cpp.extra_flags=-DCOLLAR_ID=1" \
  --output-dir /tmp/collar collar/collar.ino
arduino-cli upload -p /dev/cu.usbmodemXXXXXX \
  --fqbn "adafruit:nrf52:feather52840" --input-dir /tmp/collar
```

No collars to hand? The spare ESP32 can fake all three:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32c3" --board-options "CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DFAKE_COLLARS=3" transmitter
```

## The phone app

`CatTrackApp/` is a React Native app. It connects to the handheld over BLE and
shows one row per cat — name, signal bar, battery, and how long ago that cat
was heard.

- **Tap a row** to follow that cat: the big reading becomes its signal, and
  the tone follows it too, so the beep and the display always agree on which
  cat you are walking towards. Tap it again to go back to following the
  closest one.
- **The pencil** at the end of a row renames that cat.

```bash
tools/build_release.sh --install <adb-serial>   # build the release APK and install it
```

The release build embeds the JS bundle, so it runs with no Mac, no Metro and
no USB cable — which is what a range test in the jungle needs. The debug
build (`npm run android`) needs Metro running.

The app talks to the handheld, not to the collars: it never sees the radio.
So the handheld has to be plugged in (or at least switched on and in range)
for the app to show anything.

To check the handheld's half of the link without a phone in the loop:

```bash
python3 tools/ble_roster.py 30
```

It connects to the handheld over BLE and decodes the roster the way the
phone does. An app that shows nothing looks identical to an app that is not
connected, so this is the way to tell those two apart.

**MIUI (Xiaomi) blocks `adb shell input tap`** — injecting input events needs
the INJECT_EVENTS permission, which MIUI withholds from adb. So the app's
buttons cannot be pressed from the command line on a Redmi; use the phone
yourself, or `adb shell am start` to launch the app and read the screen with
`adb exec-out screencap`.

## Watching from a laptop

Plug the handheld into USB and run:

```bash
python3 tools/monitor.py
```

A live dashboard: one row per cat, smoothed RSSI, a signal bar, SNR, how long ago
that cat answered, battery, packets heard, and packets the collar sent that
never arrived. Ctrl-C prints a summary. `--csv` gives rows you can pipe into
`analyze_walk.py`, `--json` one object per report, and `--duration 120` stops by
itself.

Cat names come from `cats.json` in the repo root, read automatically:

```json
{ "0": "Chichi", "1": "Onanda", "2": "Chacha" }
```

Keys are collar ids (0, 1, 2), which the app shows as Cat 1, Cat 2, Cat 3. The app
keeps its own copy on the phone, written by the pencil button on each row, and that
one cannot be read from a Mac — so the names are typed once in each place.
`--names <file>` points the terminal at a different file.

The cable does not take the phone's place: the USB port and the Bluetooth
controller are separate hardware, so the phone keeps working over BLE while the
terminal is running. Unplugging puts the handheld back on the battery with
nothing else to change — the firmware prints only when a computer is on the
cable.

The terminal sees what the phone cannot: packets per collar, `missed` (gaps in
the collar's sequence counter — packets sent that never arrived), total packets,
packets naming an id this build does not know, and the BLE subscription counters.
The 20-byte roster has no room for them; a cable has no MTU. The numbers the two
*do* share are read from one snapshot, so the terminal and the phone cannot
disagree about a cat's age.

```bash
python3 tools/test_monitor.py   # checks the Report format against the parser
```

The reasoning is in [docs/adr/0001](./docs/adr/0001-laptop-watches-over-cable.md),
and the words the project uses are in [GLOSSARY.md](./GLOSSARY.md).

## Homebase — the base station

The house-facing half: a fixed, powered ESP8266 + RA-02 that receives collar pings
and publishes them to Mosquitto on m710q, which Home Assistant consumes.

```bash
arduino-cli compile --fqbn "esp8266:esp8266:generic" \
  --build-property "compiler.cpp.extra_flags=-DBASE_ID=base1 -DCAT_NAMES=Chichi|Onanda|Chacha" \
  homebase
arduino-cli upload --fqbn "esp8266:esp8266:generic" --port /dev/cu.usbserial-0001 homebase
```

Why a separate board: adding WiFi + MQTT to the handheld took it to **99% of the
ESP32-C3 app partition** (1,308,309 of 1,310,720 bytes), and the walk device and the
base station are never in the same place at the same hour. A base station has no BLE,
so it has room — this sketch is 26% of flash.

**The phone app does not talk to a base station.** It has no BLE. If you want the app,
carry the handheld.

Wiring, the two pin traps, power, MQTT topics, Home Assistant discovery and the
bring-up checklist are in [homebase/README.md](./homebase/README.md). Home Assistant
has **no MQTT integration configured today**, so add it pointing at `m710q:1883`
before expecting entities to appear.

## Working on a new collar

[Collar/README.md](collar/README.md) has the full story: the wiring table,
the packet format, how to flash a blank board through the bootloader, and a
bring-up checklist to work down for every board you solder. Two things worth
knowing before you start:

- **The chip's device id is the board's USB serial number.** A collar can be
  identified — and labelled — before anything is flashed onto it. Keep a table
  of collar id → USB serial number → cat; it is the only record of which id
  went onto which board.
- **The nRF52840's serial port needs DTR asserted**, or it passes no bytes in
  either direction and the board looks dead while it is transmitting. The
  ESP32 handheld needs the opposite.

The reasoning behind the frame, the id scheme and the collision handling is
in [multi-collar.md](multi-collar.md); the agent-facing notes are in
[AGENTS.md](AGENTS.md).

