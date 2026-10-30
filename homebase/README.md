# Homebase — the base station

A **base station**: a fixed, powered LoRa receiver that reports to the house. It
is not the walk device. It has no BLE, so the phone app does not talk to it — the
phone talks to the handheld. What a base station buys is _placement_: it sits high
and powered near where the cats go, and it never moves, which is what makes
"Chichi has not been heard in four hours" possible at all.

```
collar 0/1/2 --LoRa 433 MHz--> homebase (ESP8266 + RA-02) --WiFi--> Mosquitto (m710q) --> Home Assistant
                                                     (one MQTT state per snapshot)
```

## Why a separate board, and not the handheld plus WiFi

Two measured reasons, not taste.

**Flash.** Adding WiFi + MQTT to `handheld_ble` took the ESP32-C3 build to
**99% of the app partition** — 1,308,309 of 1,310,720 bytes. A base station has no
BLE, so it has room: this sketch is 26% of flash on ESP8266.

**Placement.** The walk device is carried into the jungle to find a cat; the base
station is bolted to a shelf. One board cannot be in both places in the same hour.

## Why ESP8266

The ESP8266 has no BLE and no USB CDC, which is exactly the set of things a base
station does not need. It is cheaper, the house already has one, and its WiFi is
proven in this house.

This board needs an SX1278 (RA-02) on the same 433 MHz band the
collars use.

## Wiring

ESP8266 hardware SPI is **HSPI**: SCK = GPIO14, MISO = GPIO12, MOSI = GPIO13, and
the hardware chip select is GPIO15. Six signals plus power and ground, listed by
RA-02 silkscreen label — the module's own pads are the authority, not a pin
number, because numberings differ between RA-02 revisions.

| RA-02 | ESP8266     | Arduino pin | Notes             |
| ----- | ----------- | ----------- | ----------------- |
| SCK   | GPIO14 (D5) | 14          | HSPI clock        |
| MISO  | GPIO12 (D6) | 12          | HSPI in           |
| MOSI  | GPIO13 (D7) | 13          | HSPI out          |
| NSS   | GPIO15 (D8) | 15          | HSPI chip select  |
| DIO0  | GPIO4 (D2)  | 4           | RX-done interrupt |
| RST   | GPIO2 (D4)  | 2           | Module reset      |
| 3.3V  | 3V3         | —           | See power below   |
| GND   | GND         | —           | Common ground     |

**The D labels are not in GPIO order.** NodeMCU silkscreen is `D5=GPIO14`,
`D6=GPIO12`, `D7=GPIO13`, `D8=GPIO15`. D6 and D7 are **not** in ascending GPIO
order, so wiring by the label alone crosses MOSI and MISO. A crossed pair is
silent in the specific way this board is: the ESP's MISO line lands on the
module's MOSI (an input, so it floats and reads high), the ESP's MOSI line lands
on the module's MISO (an output, and GPIO15's internal pull-down keeps the module
selected, so a selected SX1278 drives it low). The module is powered and alive,
the loop runs, and every SPI read returns `0xFF` - which looks exactly like a
dead module. Read the GPIO number, not the D label.

DIO1, DIO2, DIO3, DIO4 and DIO5 on the RA-02 are **not connected**. The SX1278
RX-done signal is on DIO0, which is the only interrupt this sketch needs. `ANT`
is the antenna — it is the one pad that must not be left as a stub.

### Two pin traps

**DIO0 must not be GPIO16 (D0).** Most wiring guides suggest D0 for DIO0. GPIO16 is
the only ESP8266 pin with **no interrupt support**, so a board wired that way
receives nothing at all: the module transmits and resets fine, the loop runs, and
no packet is ever heard. It looks exactly like a dead module.

**GPIO15, GPIO2 and GPIO0 are the boot strap pins.** At reset the ESP8266 samples
them: GPIO0 low = flash mode, GPIO2 must be high, GPIO15 must be low. GPIO15 has an
internal **pull-down** and GPIO2 an internal **pull-up**, and the RA-02's NSS and RST
lines are _inputs to the module_ — the module does not drive them — so using GPIO15
as NSS and GPIO2 as RST does not disturb the boot straps. That is why this pin map
boots. If you move RST to GPIO0, the board will never boot from flash.

### Power

The SX1278 PA at +17 dBm draws ~90 mA peaks on transmit. A CP2202/CP2102 module's
own 3.3 V regulator is often marginal for that, and a brown-out during transmit
looks like "the board resets randomly" or "some packets are heard and others are
not" — not like a wiring fault.

- Feed the RA-02's `3.3V` from a proper 3.3 V supply, not the CP2102 bridge's
  onboard regulator.
- Put **100 µF across 3V3 and GND**, at the module, not at the board.
- Common ground between the two boards is not optional.

### NodeMCU V3 + ESP-12E — the board actually in the house

The pin map above is unchanged on a NodeMCU V3; what changes is that the pads
have silkscreen names, the board has its own regulator, and two of the six
signal pins carry onboard hardware. Read the **D name**, cross it to GPIO, and
solder to the GPIO the table names.

```
   NodeMCU V3 (ESP-12E)                                 RA-02 / SX1278
   -------------------                                   -------------
   D5   GPIO14  ────────── SCK  ─────────────────────  SCK
   D6   GPIO12  ────────── MISO ─────────────────────  MISO
   D7   GPIO13  ────────── MOSI ─────────────────────  MOSI
   D8   GPIO15  ────────── NSS  ─────────────────────  NSS
   D2   GPIO4   ────────── DIO0 ─────────────────────  DIO0
   D4   GPIO2   ────────── RST  ─────────────────────  RESET
   3V3          ─────────────────────────────────────  3.3V
   GND          ─────────────────────────────────────  GND
```

Eight wires. The RA-02 has a GND pad at each end of each row — use either, both
are the same net. `ANT` takes the antenna and nothing else. `DIO1`–`DIO5` are
**left unconnected**, exactly as in the section above.

| RA-02 pad | NodeMCU pad | GPIO | Why this one                                     |
| --------- | ----------- | ---- | ------------------------------------------------ |
| SCK       | D5          | 14   | HSPI clock — fixed by the hardware peripheral     |
| MISO      | D6          | 12   | HSPI in — fixed                                   |
| MOSI      | D7          | 13   | HSPI out — fixed                                  |
| NSS       | D8          | 15   | HSPI chip select                                  |
| DIO0      | D2          | 4    | interrupt-capable, and not a boot strap           |
| RESET     | D4          | 2    | see the LED note below                            |
| 3.3V      | 3V3         | —    | **never VIN / 5 V**                               |
| GND       | GND         | —    | common ground, not optional                       |

**D4 carries the onboard blue LED.** The `nodemcuv2` variant in the installed
core sets `LED_BUILTIN 2` (`LED_BUILTIN_AUX 16`), so on this board GPIO2 has an
LED and its series resistor tied to 3.3 V. The SX1278 `RESET` pad is a
high-impedance input with its own internal pull-up, so the LED does not hold the
module out of reset and the wiring works — but the LED lights whenever the
sketch pulses reset, which looks alarming and is not a fault. If you would
rather keep the LED out of it, move that one wire to **D1 / GPIO5** and change
`PIN_RST` to `5` in `homebase.ino`. D1 is the only pad left that is neither a
boot strap, nor the UART, nor tied to the flash.

**Do not use these pads for anything:**

| Pad         | GPIO    | Why not                                                     |
| ----------- | ------- | ----------------------------------------------------------- |
| D0          | 16      | **no interrupt support** — the classic dead-receiver trap    |
| D3          | 0       | boot strap (must be HIGH at boot) and the FLASH button       |
| D9 / D10    | 3 / 1   | UART RX/TX — the serial console and the flash upload          |
| SD2 / SD3 / CMD / CLK | 9 / 10 / 11 / 6 | wired to the SPI flash; touching these bricks the boot |
| A0          | —       | the ADC, behind a divider; there is nothing to read here     |

**D8 already has a 10 k pull-down on the board.** That is the ESP8266 boot
requirement (GPIO15 low at reset), and it is why the boot straps in "Two pin
traps" are satisfied without extra resistors. Once `SPI.begin()` runs, HSPI
drives NSS as an output and the pull-down is overridden. It only bites if you
ever stop driving NSS, at which point the module stays selected.

**Power is easier here than on a bare module.** On a NodeMCU V3 the `3V3` pad is
an AMS1117 fed from USB 5 V, which sources far more than the ~90 mA the PA draws
— so the warning above about a CP2102 dongle's regulator does not apply to this
board. The **100 µF across 3V3 and GND at the RA-02 still does**: it is the
peak-current reservoir, not a regulator substitute. VIN is 5 V and must never
reach the module.

**The USB bridge is a CH340G, not a CP2102.** The port therefore enumerates as
`/dev/cu.wchusbserial*`, not `usbserial-0001`, so the upload command below needs
that path. The auto-flash circuit is present, so `esptool` resets the board into
flash mode without you touching D3.

## Build and flash

```bash
arduino-cli compile --fqbn "esp8266:esp8266:generic" \
  --build-property "compiler.cpp.extra_flags=-DBASE_ID=base1 -DCAT_NAMES=Chichi|Onanda|Chacha" \
  homebase
arduino-cli upload --fqbn "esp8266:esp8266:generic" --port /dev/cu.usbserial-0001 homebase
```

On a **NodeMCU V3**, use the board that matches the hardware rather than
`generic` — `generic` defaults to 1 MB of flash with 64 KB of SPIFFS and ~470 KB
of OTA room, while the V3 is 4 MB on an ESP-12E:

```bash
arduino-cli compile --fqbn "esp8266:esp8266:nodemcuv2" \
  --build-property "compiler.cpp.extra_flags=-DBASE_ID=base1 -DCAT_NAMES=Chichi|Onanda|Chacha" \
  homebase
arduino-cli upload --fqbn "esp8266:esp8266:nodemcuv2" \
  --port /dev/cu.wchusbserial1420 homebase
```

There is no `nodemcuv3` board; the V3 is flashed as `nodemcuv2` (ESP-12E, 4 MB).

The base station id and the cat names are baked in at flash time, the same way the
collar id is, so Home Assistant's discovery creates devices called **Chichi, Onanda,
Chacha** rather than Cat 0, Cat 1, Cat 2.

`arduino-cli upload` does not compile: it sends the artifacts of the last build of
that sketch. Build first, then upload, and do not let a stray compile of a
different `-DBASE_ID` land on this board with no error at all.

**The board must be in flash mode** (GPIO0 low at reset). If your board has the
auto-flash circuit, `esptool` handles it; the wiring on the CP2102 board tested here
does, and `esptool` connected and ran its stub.

### Measured sizes, and the one to watch

Measured on `esp8266:esp8266:nodemcuv2` (ESP-12E, 4 MB), `base1` with three cat
names:

```
Variables and constants in RAM:  30972 / 80192   (38%)
Instruction RAM (IRAM):          61699 / 65536   (94%)
Code in flash (IROM):           274096 / 1048576 (26%)
```

Flash is comfortable. **IRAM at 94% is the constraint.** On ESP8266, anything
called from an ISR must live in IRAM, and that segment has ~3.8 KB left. A future
change can fail there with a _link_ error, not a compile error. If you add a radio
feature and the build fails at link time, that is why.

## Air settings — must match the collar

`FREQ_MHZ 433.0`, `BANDWIDTH 125.0`, `SPREADING 9`, `CODING_R 7`, `SYNC_WORD 0x12`,
`PREAMBLE 8`. `TX_POWER` is transmit-only and irrelevant to a receiver.

A homebase built with different air settings receives nothing, and the failure is
indistinguishable from a dead module. Check the register values against the golden
table in `collar/README.md` before blaming the antenna.

## ESPHome build — `esphome/cat-base2.yaml`

ESPHome 2026.x ships an `sx127x` component with an `on_packet` trigger that hands
over the raw bytes plus RSSI and SNR, so this board also runs as an ESPHome device
instead of a hand-written sketch. For a base station bolted to a wall that is the
build to prefer: OTA updates, and the entities arrive through the ESPHome native
API rather than through hand-published MQTT discovery.

| File                          | What it is                                                   |
| ----------------------------- | ------------------------------------------------------------ |
| `esphome/cat-base2.yaml`      | the device: WiFi, time, SPI, `sx127x`, one entity set per cat |
| `esphome/collar_frame.h`      | the six-byte frame decode, header-only, one place              |
| `esphome/test_collar_frame.cpp` | host test for that decode                                    |

```bash
esphome compile homebase/esphome/cat-base2.yaml
esphome upload --device /dev/cu.usbserial-21140 homebase/esphome/cat-base2.yaml
esphome logs   --device /dev/cu.usbserial-21140 homebase/esphome/cat-base2.yaml
# after the first USB flash, over the network:
esphome run homebase/esphome/cat-base2.yaml
```

### Where the file lives — and why the dashboard did not show it

The ESPHome dashboard on `m710q` compiles whatever sits in **its own** config
directory, `/home/stefan/homeassistant/esphome-config/` on the host (mounted as
`/config` in the `esphome` container). This repo holds the source of truth, but a
configuration that exists only on the laptop is a device the dashboard has never
heard of — which is exactly how `cat-base2` was missing from it. Copy the two
buildable files across:

```bash
scp homebase/esphome/cat-base2.yaml homebase/esphome/collar_frame.h stefan@m710q:/tmp/
ssh stefan@m710q 'sudo cp /tmp/cat-base2.yaml /tmp/collar_frame.h \
    /home/stefan/homeassistant/esphome-config/'
```

Two copies is the one real drag here: the dashboard will happily build a stale one,
so re-copy after every edit. `collar_frame.h` has to travel with the YAML — the
`includes:` line resolves it next to the configuration, and a missing header fails
the build, not the flash.

### Radio and network, as configured

- **WiFi:** `networks:` with **TP-Link_9E19** first and HUAWEI second. The TP-Link
  hands out `192.168.100.x` like the HUAWEI does, so this is the same L2 — the
  second entry is a fallback that keeps the board reachable for OTA if the TP-Link
  is off or the passphrase is wrong, rather than stranding it off the network.
- **Address:** static `192.168.100.231`, outside the routers' `.100`–`.199` pool and
  free on the LAN. Home Assistant's config entry stores that address, so changing
  it means editing the entry, not just the flash.
- Read the board's own view of all this from its `SSID`, `BSSID`, `IP address` and
  `WiFi signal` entities — "it is on the network" and "it is on the network I
  assumed" are otherwise indistinguishable, and a device in a bad spot shows
  up as missing packets rather than as a signal problem.

The board is on USB at `/dev/cu.usbserial-21140` (CH340). After the first flash
everything can go over the network. `esphome upload` does not compile - build
first, or use `esphome run`.

The frame decode is byte-level code worth testing, so it is not in the YAML:

```bash
c++ -std=c++17 -Wall -Wextra -o /tmp/test_collar_frame \
    homebase/esphome/test_collar_frame.cpp && /tmp/test_collar_frame
```

### Why this one is base 2, not base 1

`cattracker/base1/#` is already published by the Arduino firmware above, and the
three "… missing 10 minutes" automations in Home Assistant fire on the MQTT
entities that discovery created (`binary_sensor.chichi_chichi` and friends). Two
boards publishing as the same base id would overwrite each other's retained state
on the same topics, so this board is **Cat Base 2**: its own device in Home
Assistant, its own `cat_base_2_*` entities, and no change to how base 1 alerts.
Point the automations at `binary_sensor.cat_base_2_<cat>_in_range` if this is the
board that replaces it — and clear the retained `homeassistant/…/base1_*` and
`cattracker/base1/#` topics when the old board is retired, or its ghost entities
live on in the broker and keep the old automations "working" against a dead base.

There is deliberately no MQTT in this build: the native API is what the ESPHome
integration reads, so nothing is published twice and the broker keeps only base 1.

### What it exposes

Per cat: signal (dBm, smoothed over 12 packets), SNR (dB), battery (% — unknown
when the collar sends `0xFF`), seconds since heard (unclamped, so a cat missing for
an hour reads as an hour), last seen (a `timestamp`), in range (connectivity),
packets and missed (diagnostics). Per base: packets, unknown id drops.

Per base, the network: `SSID`, `BSSID`, `IP address`, `WiFi signal` (dBm).

Every entity polls every 5 s. Going out of range is the *absence* of an event, so
the three `in range` flags are pushed from an `interval:` instead — the one timer on
the board.

Built on `esp8266:esp8266:nodemcuv2`: **RAM 41%, flash 35%**. The IRAM ceiling
described above belongs to that sketch, not to the board.

### The Cats dashboard

Home Assistant's sidebar dashboard **Cats** is laid out per cat rather than per
device, because that is the question being asked: one section per collar, with a
**Base 2 · rooftop** row and a **Base 1 · inside house** row, then a Stations section
with packets, unknown-id drops and base 2's WiFi. The two rows matter: coverage is
per station, so a cat can be `in range` on one and not the other, and only *no
station* hearing it means missing. It is a storage-mode dashboard
(`lovelace.dashboard_cats`), so it is edited in the UI, not in this repo — but the
entity ids it depends on are the ones named above, and base 1's carry the numeric
suffixes (`_3` signal, `_4` battery, `_5` last seen) the MQTT discovery records
collided into.

### Alerts

Four automations carry the cat alerts. Three are `<cat> missing 10 minutes` — each
cat's `in range` off for ten minutes, which is the collar going silent. The fourth,
`Chichi back in range`, is the opposite edge: it fires when *either* station hears
Chichi again (`in range` on for 20 s continuously, so one stray packet is not a
"back"), and is rate-limited to one alert per half hour, because a collar at the
edge of range flaps and the point of the alert is "it is alive", not a feed. All
four notify the Xiaomi phone with `title`, `message` and a high-priority channel.

The trap is the service name. The notify **entity** is `notify.xiaomi_orange`; the
notify **service** is `notify.mobile_app_xiaomi_mi_lite_5g`. Calling the entity id
as if it were a service fails with `Action notify.xiaomi_orange not found`, and
because that is the *first* action in the sequence it aborts the rest — so the iPad
notification in the same automation never went out either. All three silence alerts
were dead from the day they were written until 2026-10-28 for exactly this reason,
and the only trace was one line in `home-assistant.log`: nothing on the phone, no
red entity in the UI, and the automation's `last_triggered` looked perfectly
healthy. Check the log line for a new alert once, rather than trusting that a
`last_triggered` means anybody was told.

## What it publishes

MQTT broker: `192.168.100.66:1883` (m710q, anonymous). This is the Arduino build's
half of the story; the ESPHome build above uses the native API instead.

| Topic                                               | Content                                 |
| --------------------------------------------------- | --------------------------------------- |
| `cattracker/<base>/state`                           | retained JSON, one snapshot per publish |
| `cattracker/<base>/availability`                    | retained `online` / `offline`           |
| `homeassistant/sensor/<base>_cat<N>_<field>/config` | retained discovery                      |

State fields per collar: `rssi` (dBm × 10), `snr` (dB × 2), `batt`, `age` (seconds
since last packet), `pk` (packets heard), `miss` (seq gaps — packets the collar sent
that never arrived), `in` (in range), `last` (epoch of last packet). Integers, in the
roster's own scaling, because ESP8266's `printf` has no `%f` — Home Assistant's
`value_template` divides by 10 and 2.

Entities per cat: signal, SNR, battery, last seen (timestamp), in-range (binary).
Plus, on the base station device: total packets and unknown-id drops.

There is **no seconds-since-last-heard entity**. It was redundant: the duration is
`as_timestamp(now) - as_timestamp(state)` on the last-seen sensor, so a template
sensor gets it without a second entity. The state JSON still carries `age` per
collar, because that is the raw fact and a template can use it directly.

**`age` is not clamped.** It used to be clamped to 255 s, and `last` was computed
from the clamped value, so a collar missing for 20 minutes reported
`last = now - 255`: Home Assistant showed it as heard four minutes ago, and that
value moved forward with the clock for as long as it stayed missing. Past 255 s
both sensors made a missing cat look healthy, which is the exact failure the base
station exists to detect. `age` is a `uint32_t` printed with `%u`, so it carries
hours with no packing constraint.

**The last-will is the point.** The availability topic carries a last-will of
`offline`. If the base station loses power or WiFi, the broker fires it and Home
Assistant knows the house went **blind**. A Missing alert then only ever means the
cat, never the plumbing — which is what stops the first notification being a false
alarm about a power cut.

Publishing cadence: on every beacon, and once per quiet receive window, so ages keep
counting up. The state is retained, so Home Assistant sees the last known state on
restart rather than showing every cat as unknown until the next beacon.

**Home Assistant has no MQTT integration configured today** — there is no `mqtt:` in
`configuration.yaml` and no `mqtt` entry in `.storage`. Add it pointing at
`m710q:1883` before expecting entities to appear.

## Bring-up checklist

1. `python3 -m esptool --chip esp8266 --port <port> --baud 115200 chip-id` — confirms
   the chip and the auto-flash wiring. esptool 5.x has no `detect`/`chip_detect`
   command; `chip-id` is the one that works.
2. Flash, then read the serial banner: base id and state topic.
3. Watch the broker: `mosquitto_sub -h 192.168.100.66 -t 'cattracker/#' -v`.
4. Check WiFi associated. `WiFi.setSleep(false)` is set in `setup()`; generic boards
   do not associate reliably with modem sleep on.
5. Put a collar in range (or the fake collar: `-DFAKE_COLLARS=3` on the spare ESP32)
   and watch `pk` climb.
6. If nothing arrives, check DIO0 is on GPIO4 and not GPIO16, then the air settings.

## Known limits

- **No BLE.** The phone app cannot connect to a homebase. If you want the app, carry
  the handheld.
- **Single-core, 80 MHz, no FreeRTOS task creation** in the ESP8266 Arduino core, so
  the publish timer lives in `loop()`. `radio.receive()` blocks for the RX window
  (8.2 s), so a quiet window publishes once, not every 2 s.
- **One base station sees one range.** Coverage is what base stations #2 and #3 are
  for, and the topic scheme already carries the base id so a second base station
  cannot overwrite the first.
