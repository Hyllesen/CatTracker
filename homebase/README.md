# Homebase — the base station

A **base station**: a fixed, powered LoRa receiver that reports to the house. It
is not the walk device. It has no BLE, so the phone app does not talk to it — the
phone talks to the handheld. What a base station buys is *placement*: it sits high
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

**The one thing it cannot do is a CC1101.** The ESP8266 already in this house runs
a CC1101 at 433.92 MHz. CC1101 is FSK/GFSK and has **no LoRa mode**, so it cannot
receive a collar. This board needs an SX1278 (RA-02) on the same 433 MHz band the
collars use. A CC1101 homebase is a different radio project, not this one.

## Wiring

ESP8266 hardware SPI is **HSPI**: SCK = GPIO14, MISO = GPIO12, MOSI = GPIO13, and
the hardware chip select is GPIO15.

```
              RA-02 (SX1278)                    ESP8266
              ┌──────────┐                      
       NSS ───┤          │                      
       MOSI ──┤          │        D6 / GPIO13 ──┤ MOSI
       MISO ──┤          │        D7 / GPIO12 ──┤ MISO
       SCK ───┤          │        D5 / GPIO14 ──┤ SCK
       DIO0 ──┤          │        D2 / GPIO4  ──┤ DIO0
       RST ───┤          │        D4 / GPIO2  ──┤ RST
       3.3V ──┤          │        3V3           ─┘ 3.3V
       GND ───┤          │        GND           ─┘ GND
              └──────────┘
```

| RA-02 | ESP8266 | Arduino pin | Notes |
|---|---|---|---|
| SCK | GPIO14 (D5) | 14 | HSPI clock |
| MISO | GPIO12 (D7) | 12 | HSPI in |
| MOSI | GPIO13 (D6) | 13 | HSPI out |
| NSS | GPIO15 (D8) | 15 | HSPI chip select |
| DIO0 | GPIO4 (D2) | 4 | RX-done interrupt |
| RST | GPIO2 (D4) | 2 | Module reset |
| 3.3V | 3V3 | — | See power below |
| GND | GND | — | Common ground |

DIO1, DIO2, DIO3 and ANT on the RA-02 are **not connected**. The SX1278 RX-done
signal is on DIO0, which is the only interrupt this sketch needs.

### Two pin traps

**DIO0 must not be GPIO16 (D0).** Most wiring guides suggest D0 for DIO0. GPIO16 is
the only ESP8266 pin with **no interrupt support**, so a board wired that way
receives nothing at all: the module transmits and resets fine, the loop runs, and
no packet is ever heard. It looks exactly like a dead module.

**GPIO15, GPIO2 and GPIO0 are the boot strap pins.** At reset the ESP8266 samples
them: GPIO0 low = flash mode, GPIO2 must be high, GPIO15 must be low. GPIO15 has an
internal **pull-down** and GPIO2 an internal **pull-up**, and the RA-02's NSS and RST
lines are *inputs to the module* — the module does not drive them — so using GPIO15
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

## Build and flash

```bash
arduino-cli compile --fqbn "esp8266:esp8266:generic" \
  --build-property "compiler.cpp.extra_flags=-DBASE_ID=base1 -DCAT_NAMES=Chichi|Onanda|Chacha" \
  homebase
arduino-cli upload --fqbn "esp8266:esp8266:generic" --port /dev/cu.usbserial-0001 homebase
```

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

```
Variables and constants in RAM:  30956 / 80192   (38%)
Instruction RAM (IRAM):          61699 / 65536   (94%)
Code in flash (IROM):           273952 / 1048576 (26%)
```

Flash is comfortable. **IRAM at 94% is the constraint.** On ESP8266, anything
called from an ISR must live in IRAM, and that segment has ~3.8 KB left. A future
change can fail there with a *link* error, not a compile error. If you add a radio
feature and the build fails at link time, that is why.

## Air settings — must match the collar

`FREQ_MHZ 433.0`, `BANDWIDTH 125.0`, `SPREADING 9`, `CODING_R 7`, `SYNC_WORD 0x12`,
`PREAMBLE 8`. `TX_POWER` is transmit-only and irrelevant to a receiver.

A homebase built with different air settings receives nothing, and the failure is
indistinguishable from a dead module. Check the register values against the golden
table in `collar/README.md` before blaming the antenna.

## What it publishes

MQTT broker: `192.168.100.66:1883` (m710q, anonymous).

| Topic | Content |
|---|---|
| `cattracker/<base>/state` | retained JSON, one snapshot per publish |
| `cattracker/<base>/availability` | retained `online` / `offline` |
| `homeassistant/sensor/<base>_cat<N>_<field>/config` | retained discovery |

State fields per collar: `rssi` (dBm × 10), `snr` (dB × 2), `batt`, `age` (seconds
since last packet), `pk` (packets heard), `miss` (seq gaps — packets the collar sent
that never arrived), `in` (in range), `last` (epoch of last packet). Integers, in the
roster's own scaling, because ESP8266's `printf` has no `%f` — Home Assistant's
`value_template` divides by 10 and 2.

Entities per cat: signal, SNR, battery, seconds-since-last-heard, last seen
(timestamp), in-range (binary). Plus, on the base station device: total packets and
unknown-id drops.

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
