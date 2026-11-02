# Remote node + gateway — wiring guide

Walk-through for the first build: **node 1** (ESP32-C3 + RA-02 433 RX + RAK3172(H)
915 TX) and **the gateway** (ESP32-C3 + RAK3172(H) RX + WiFi/MQTT, base id `base3`).
The design and the numbers are in `final-remote-plan.md`; this file is the bench
procedure. The order below is deliberate: **prove the 915 hop with no 433 radio
involved**, then add the RA-02.

Read `AGENTS.md` first. Three of its rules apply to every step here: never
transmit without an antenna; never add an ungated print to a board that runs from
a battery; `arduino-cli upload` does not compile.

## 0. Parts

| Qty | Part | Where it goes |
|-----|------|---------------|
| 2 | **RAK3172-9-SM-NI** (9XX = US915/AU915/KR920/AS923, SKU 306041, **without TCXO**), on a breakout with an RF-pad antenna path | node (TX) + gateway (RX) |
| 2 | ESP32-C3 board with **native USB** — GPIO20/21 must NOT be wired to a USB-UART bridge | node + gateway |
| 1 | RA-02 (SX1278, 433 MHz) | node |
| 3 | Antennas: node 433 (17.3 cm), node 915 (8.2 cm), gateway 915 (8.2 cm) | label each at the pigtail |
| 1 | **Protected** 18650 (with PCB) + holder | node |
| 1 | **3.3 V buck-boost**, 3.0–4.2 V in, ≥ 500 mA | node |
| 1 | CN3065 (or TP4056) charge module — **it charges, it protects nothing** | node |
| 1 | Solar panel 5 V, **10 W** (5 W is marginal under canopy, §5) | node |
| 4 | 100 µF electrolytic, one at each radio | |
| 3 | 100 nF ceramic | ADC divider, one at each radio |
| 2 | 100 kΩ resistors | battery divider |
| 1 | 5 V USB supply | gateway (mains, USB brick) |
| — | NodeMCU ESP8266 (optional, for the bench base only) | bench |
| — | IP65 box, desiccant, pole | node |

**Antenna geometry** (λ in cm = 299.79 / f in MHz, quarter wave = λ/4):

| Band | λ | Quarter wave |
|------|---|--------------|
| 433 MHz | 69.2 cm | **17.3 cm** |
| 915 MHz | 32.8 cm | **8.2 cm** |

The two lengths are **not** close — 433 is ~2.1× 915 — so the failure is not
mixing them up by length. A 17 cm wire on 915 is ~0.52 λ and radiates badly; an
8 cm wire on 433 is a badly detuned monopole. Cut the 915 radiator slightly long
and trim; insulation shortens the electrical length, so start at **7.8 cm** and
trim while watching the bench hop's RSSI.

**The `-NI` suffix means there is no IPEX connector.** The module label carries a
**`(P)`** marking, which is RAK's family tag for the no-IPEX variant (their `-T` table
marks its no-IPEX parts `(TP)`/`(TEP)`); the part number and the marking agree, so
there is no connector to click onto. (Verify by eye on the bench — an IPEX footprint
is an obvious plated circle — but do not buy a u.FL pigtail and hope.) The antenna goes on the
**RF solder pad**: solder a short coax pigtail, shield tied to the neighbouring
GND pad, and keep the unshielded stub as short as possible. This is a soldering
step, not a connector you click on, and it is the single most likely reason a
first node works on the bench and not in the box.

## 1. The ESP32-C3 budget, and why the pin map is tight

Usable GPIOs: **0–10, 20, 21**. **GPIO2, GPIO8, GPIO9 are boot straps** — boot
mode is read from them at reset, so a divider, a radio line, or a pull on those
pins can make the board not boot when the cell is low, which reads as a dead
board. **GPIO18/19 are the USB D-/D+ pair** — keep them for the USB CDC console,
because that is the only console a battery node has. **GPIO11–17 are the on-board
flash pads.**

**GPIO20/21 are the U0RXD/U0TXD IO-mux functions, and with the console on USB CDC
they are free GPIOs** (`pins_arduino.h` defines `TX = 21`, `RX = 20`). That is the
pin space that makes the node fit: the RAK3172 UART goes on 20/21, and RA-02 RST
keeps a real pin, so a stuck radio in the jungle can be reset in software. Check
your board's schematic first: on some devkits GPIO20/21 are also wired to an
onboard USB-UART bridge, and if yours is, the console needs them and they are not
free.

Signals needed: SPI ×4, DIO0, RST, UART ×2, ADC ×1 = 9. Free non-strap pins: 0, 1,
3, 4, 5, 6, 7, 10, 20, 21 = **10**. It fits with one spare.

### Node pin map (ESP32-C3 ↔ RA-02 ↔ RAK3172)

| Signal | C3 pin | Arduino alias | Goes to | Note |
|--------|--------|---------------|---------|------|
| SCK | **GPIO4** | `SCK` | RA-02 SCK | board's SPI default |
| MISO | **GPIO5** | `MISO` | RA-02 MISO | board's SPI default |
| MOSI | **GPIO6** | `MOSI` | RA-02 MOSI | board's SPI default |
| NSS | **GPIO7** | `SS` | RA-02 NSS | board's SPI default |
| DIO0 | **GPIO0** | `A0` | RA-02 DIO0 | RX-done interrupt, not a strap |
| RA-02 RST | **GPIO1** | `A1` | RA-02 NRESET (pin 7) | firmware can reset the 433 radio |
| UART RX | **GPIO20** | `RX` | RAK3172 **TX2** | free because the console is USB CDC |
| UART TX | **GPIO21** | `TX` | RAK3172 **RX2** | free because the console is USB CDC |
| Battery divider | **GPIO3** | `A3` | 100k/100k midpoint + 100 nF | ADC1_CH3, not a strap |
| Spare | **GPIO10** | — | suggested: RAK3172 NRST (**verify** the pad name on your breakout) | the one pin left over |
| Console | USB-C | `Serial` (HWCDC) | your laptop | `CDCOnBoot=cdc` |
| 3V3 / GND | | | RA-02 VCC/GND, RAK VCC/GND | **never 5 V, never VIN** |

Cross-check every row against your board's **silkscreen** before soldering. DevKit
boards print GPIO numbers; some clones print both. Do not trust a pin function
label, trust the number.

**Do not use GPIO2/8/9 for anything.** Do not put the battery divider on them.
Do not use GPIO18/19 while the console is USB CDC.

### NRESET is not a hard tie

The SX1278 datasheet §7.2.1 is explicit: **"Pin 7 (NRESET) should be left
floating during the POR sequence"**, and the host should wait 10 ms after the POR
cycle before touching the SPI bus. The chip can drive NRESET during power-on
reset, so a hard tie to 3V3 is a contention risk. **NRESET goes to GPIO1**, idle
high, pulled low for a manual reset, nothing tied hard to 3V3. RadioLib's constant
for an unused pin is **`RADIOLIB_NC`**, not `RST_NOT_SET`; its signature is
`Module(cs, irq, rst, gpio = RADIOLIB_NC)`. Compile before trusting a name:

```cpp
SX1278 radio = new Module(/*cs*/ 7, /*irq DIO0*/ 0, /*rst*/ 1, /*gpio*/ RADIOLIB_NC);
// setup(): SPI.begin(/*sck*/ 4, /*miso*/ 5, /*mosi*/ 6, /*ss*/ 7);
```

**Watchdog that pin:** on an unattended node, read `RegVersion` about once a
minute. If it is not `0x12`, pulse RST, re-init the radio, and count the event in
the heartbeat. A radio that wedges in the jungle with no reset path is a walk into
the jungle.

### Gateway pin map (ESP32-C3 ↔ RAK3172)

| Signal | C3 pin | Goes to |
|--------|--------|---------|
| UART TX | **GPIO21** | RAK3172 **RX2** |
| UART RX | **GPIO20** | RAK3172 **TX2** |
| Console | USB-C | laptop, for the `v` report |
| WiFi | on-board, 2.4 GHz | Mosquitto on `m710q:1883` |
| 5 V / GND | USB brick | board VIN/5V |

The gateway is RX-only on 915, so it has no radio to drive, one antenna at **8.2
cm**, and no battery. **Same UART pins as the node** (GPIO20 RX, GPIO21 TX), so
`PIN_RAK_RX`/`PIN_RAK_TX` are one pair in the shared code and one cable pattern
serves both boards.

### Bench base (NodeMCU ESP8266, phase 1 only, throwaway)

`SoftwareSerial` at 115200 is only reliable **with WiFi off**, so the bench base is
a separate sketch with no WiFi.

| Signal | NodeMCU | Goes to |
|--------|---------|---------|
| RX (SoftwareSerial) | **D1 = GPIO5** | RAK3172 **TX** pad |
| TX (SoftwareSerial) | **D2 = GPIO4** | RAK3172 **RX** pad |
| Console | UART0 (GPIO1/3, the USB port) | laptop, also the flash port |
| 3V3 / GND | | RAK VCC/GND — **not 5 V** |

ESP8266 traps from `homebase/README.md`: read the **GPIO number**, not the `D`
label (D6/D7 are not in GPIO order); crossed MISO/MOSI reads 0xFF and looks like
a dead module; DIO0 must not be on GPIO16 (no interrupts); GPIO0, GPIO2, GPIO15
are boot straps.

## 2. Power and decoupling, on both boards

- Every radio gets **100 µF across 3V3-GND**, soldered as close to the module's
  VDD pad as possible. The RA-02 PA peaks at ~105 mA and the RAK3172 transmit
  burst is 87 mA typ. A brown-out during transmit looks like "the board resets
  randomly" or "some packets arrive, others do not" — not like a wiring fault.
- **Never feed the RA-02 or the RAK3172 from 5 V or VIN.** 3.3 V only.
- **Supply chain: cell → 3.3 V buck-boost → the board's 3V3 pin and both radios.**
  A plain board regulator is not safe to rely on: an AMS1117-class LDO needs ~4.4 V
  in for 3.3 V out and dies as soon as the cell sags to 3.5 V. **Verify** the
  regulator marking on your C3 board.
- **Feeding 3V3 directly means the board's own USB regulator can fight the
  buck-boost.** Flash with the battery disconnected, or disable the buck-boost's EN
  while USB is plugged in. **Verify** on your board.
- **Protection:** protected 18650 or a protection board, buck-boost undervoltage
  cutoff around 3.2 V. The CN3065 only charges; it does not protect against
  over-discharge.
- The C3 gets its own 10 µF at the power entrance plus 100 nF near the module.
- Node battery: 100 kΩ / 100 kΩ divider to **GPIO3**, 100 nF at the pin. The
  divider reads half the cell voltage; firmware reports 0..100 % and `0xFF` when
  unwired — the same convention as the collar. A cell-less node reads ~1 V at the
  pin, which the firmware calls "no reading", not 0 %.
- CN3065 `BAT+` → cell +, `BAT-` → cell −, panel → `Vin`. Node's C3 and radios
  all hang off the **battery** side, not the panel side.

## 3. Antennas

- **Node:** 433 antenna on the RA-02 (17.3 cm radiator + 17.3 cm counterpoise to
  **GND only** — the counterpoise touching anything else is the classic silent
  failure, see `collar/README.md` §"The counterpoise must touch GND and nothing
  else"). 915 antenna on the RAK3172, **8.2 cm** radiator + counterpoise.
- **Gateway:** 915 antenna only, 8.2 cm.
- **Never power a RAK3172 with no antenna.** It can permanently damage the output
  stage. Same rule for the RA-02 when it ever transmits.
- Keep the counterpoise geometry fixed and rigid. A dangling wire is a measured
  antenna that becomes an unmeasured one.
- **Separate the node's two antennas by tens of cm, not a few.** The node transmits
  at 25 mW on 915 while its RA-02 is trying to hear a −129 dBm signal on 433. A
  transmitter a few cm from a receiver is not "desensed a bit", it is the RA-02
  sitting in the shadow of its own transmit and missing collar pings. Put the 915
  antenna at the top of the pole box and the 433 antenna at the bottom, on opposite
  sides, with the boards between them, and verify by listening to a collar while
  transmitting.

## 4. Build order, with a check after every step

### Step A — the 915 hop, node side only
Wire node C3 → RAK3172 (UART1 TX GPIO21 → module RX, RX GPIO20 ← module TX, 3V3,
GND, 100 µF, 8.2 cm antenna).
Flash `node_link/` (`-DNODE_ID=1`), which sends a fake report every 8 s.

**Band check first.** `AT+VER` reports the **firmware build**, not the band. The
part number settles it: **`RAK3172-9-SM-NI` is the US915/AU915/KR920/AS923 part**
(SKU 306041), so it is the 915-capable module and not a low-band part. (`…-8-…` is the
8XX group, EU868/IN865/RU864.) The label also reads **`(P)`**, the no-IPEX marking.
This part is **without TCXO** — a plain crystal — so its frequency error moves with
temperature; at BW 125 kHz that is normally tolerable, but if a cold morning link
looks worse than a warm-afternoon one, suspect it before the antenna. Record the label
string and `AT+SN` in the node id table. The
module is **not** built for 433, so the 433-MHz RAK collar idea in the older notes is
out for this part. A module in the wrong band is not a tuning problem: its matching
circuit is built for another frequency, and it reads as a dead radio.

Console: `python3 tools/serial_cmd.py` picks the right DTR by USB vendor id —
the C3 must have **DTR low** (asserting it resets the board), the collar's nRF52
needs DTR high. Expect:

```
NODE 1 ver 1 DEVICEID ...            (only when the host is on the cable)
P2P 915000000:7:125:0:8:14          (readback, not what you typed)
TX #123 seq 12 ok
```

**915 settings, one table, three consumers** (node, gateway, bench base):
`AT+P2P=915000000:7:125:0:8:14` — 915.000 MHz, **SF7**, BW 125 kHz, **CR 0 = 4/5**,
preamble 8, 14 dBm. The CR field is **0–3** (0 = 4/5, 2 = 4/7); a readback showing
`…:4:…` is out of range. **Read `AT+P2P=?` on both modules and compare the strings**
— never trust what you typed, and never compare a typed string to a typed string.

### Step B — the 915 hop, receive side
Wire the NodeMCU bench base (D1/D2, 3V3, GND, 100 µF, 915 antenna), **WiFi off**,
flash `base_link/`. Acceptance from `final-remote-plan.md` phase 1: **one decoded
report per 8 s for 30 minutes, 0 missed seq** at a few metres, then at the longest
distance you expect in the field. Then change SF on one side and watch it go deaf,
so that symptom is on record before it happens in the jungle.

The receive event you should see on the module's own AT port is
`+EVT:RXP2P:<rssi>:<snr>:<hex>` — the gateway gets the hop's RSSI and SNR for
free, which is what the locator uses for the link-quality row.

### Step C — add the 433 half to the node
Wire RA-02 to the SPI pins above, DIO0 to GPIO0, **NRESET to GPIO1** (not tied to
3V3 — the datasheet wants NRESET floating during POR, and the chip can drive it
during power-on), 3V3/GND, 100 µF, 433 antenna with its counterpoise.

Check before writing firmware: the `v` report must read back
**FRF `0x6C4000` (433.000 MHz), MC1 `0x76`, MC2 `0x94`, Sync `0x12`, InvIQ
`0x27`, PA `0xFF`, RegVersion `0x12`** — the healthy register values measured on
a verified collar. If `MC1`/`MC2` is off by a nibble, that board was built with
different air settings: it transmits fine and is invisible, which is exactly what
a dead radio looks like. Every SPI read returning 0x00 means SCK is not being
clocked; every read returning 0xFF means MISO/MOSI are crossed.

Acceptance for phase 2: node and a house receiver side by side, **same `id` and
same `seq`**, RSSI within **±3 dB**. Use `transmitter/` (`-DFAKE_COLLARS=3`) to do
this indoors with no cats on. If RSSI disagrees systematically, find the
RSSI/SNR-convention cause (RadioLib corrects packet RSSI when SNR < 0, and RSSI
must be read **before** SNR) **before** calibrating offsets.

### Step D — the gateway
Wire the second C3 + RAK3172 (UART TX GPIO21, RX GPIO20, 3V3, GND, antenna), flash
`gateway/` with `-DBASE_ID=base3`, credentials from build flags — not hardcoded,
the ESP8266 sketch's are, and do not paste them into prompts.

Verify on `m710q` (the box has no `mosquitto_sub`, so use the docker container
that runs Mosquitto, or Home Assistant's developer tools):

```
cattracker/base3/availability  "online"      (retained, LWT "offline")
cattracker/base3/state         throttled summary, dBm×10 / dB×2
cattracker/base3/obs           one JSON per observation, frame's own units
```

`state` keeps base1's scaling so one HA `value_template` serves both bases; `obs`
stays raw. A unit is converted exactly once, at the boundary.

Unplug the node and confirm: its age climbs, the cats' rows stay, and the node's
own in-range binary goes false. That is the Blind-vs-Missing distinction, and it
is what stops the first notification being a false alarm about a power cut.

### Step E — power, solar, enclosure

**The 18650 does not power the board directly.** A cell swings 4.2 → 3.0 V (and
below, which damages it), and a typical C3 board's onboard 5 V→3.3 V LDO needs
headroom the cell does not give: at 3.0 V in, an MCP1700-class LDO puts out ~2.65
V, and the C3's digital supply spec is 3.0–3.6 V. **So the node needs a buck-boost
regulator, cell to a solid 3.3 V** (LTK5112, TPS63906, MP2315-based modules, any
that keeps 3.3 V out over 2.5–4.2 V in). This is the difference between a node that
runs for a month and a node that browns out on the first cloudy night, and a
brown-out looks like a radio fault.

**Over-discharge:** the CN3065 only charges. Use a **protected** 18650 (integrated
PCB) or a separate protection board with a low-voltage cutoff (~2.5–3.0 V), so the
node cannot run the cell below that. `collar/README.md` already makes this point
about the collar's LiPo.

Measure node average current with 433 RX running and uplink idle, and put the
**measured** number in `node/README.md`. The budget, with the RAK's idle draw
included, is an estimate — the RAK's non-sleep idle is the weakest number in it:

| Load | Current |
|------|---------|
| RA-02 continuous RX | ~12 mA (datasheet `IDDR` 11.5–12.0 mA, RA-02 spec 12.15 mA) |
| ESP32-C3, CPU running, WiFi off | ~20 mA |
| RAK3172 awake, idle | **a few mA — measure it.** The datasheet's 1.69 µA is **deep
  sleep**, not idle. A TX-only module stays awake unless low-power mode is enabled. |
| **Planning figure** | **35–40 mA → 0.85–0.95 Ah/day → 3.2–3.5 Wh/day at 3.7 V** |

With ~90% buck-boost and ~85% charger efficiency, the panel must deliver roughly
**4.4 Wh/day**. A 5 W panel needs ~0.9 equivalent full-sun hours; under dense
canopy that is **marginal, not comfortable**. **Mount the panel above the canopy
edge, use 10 W, and measure the real average current and charge current before the
first field install.** With no sun, expect **~2 days** from a 2000 mAh cell.

If the RAK3172's low-power mode is used, **wake its UART with a throwaway byte
before each AT command** — the first byte after sleep can be lost. **Verify** on
your firmware.

IP65 box, 3–5 m pole, **drip loop** on the panel cable, desiccant inside, boards
conformal coated. The wet season is the test, not the dry one.

### Step F — the sketches, and the channel-occupancy maths

`node_link.ino` and `base_link.ino` **do not exist in this repo** (`ls` and
`git status` show no such directories) — they were drafted in the chat that wrote
`claude-remote-nodes-plan.md` and have never been compiled. Write them here, with
`PIN_RAK_RX = GPIO20`, `PIN_RAK_TX = GPIO21`, the SF7 settings above, and the
`(collar id, seq)` grouping.

**Frame airtime** (9-byte backhaul frame, explicit header, CRC on, BW 125, CR 4/5):

| SF | Frame airtime | Occupancy, 4 nodes × 3 collars per 8 s |
|----|---------------|----------------------------------------|
| 7 | ~41 ms | **~6%** |
| 8 | ~72 ms | ~11% |
| 9 | ~144 ms (CR 4/7: ~169 ms) | ~22% |
| 10 | ~248 ms | ~37% |

**Start at SF7.** SF9 puts 4 nodes at ~22% occupancy, and a 250 ms stagger only
just fits one SF9 frame — it does nothing to separate the reports for different
collars. Raise SF one step at a time only if the measured hop needs it: SF7 is
roughly 5–6 dB less sensitive than SF9 (demodulator limits: SF7 −7.5 dB, SF8 −10,
SF9 −12.5, SF10 −15), so aim for hop SNR at least ~6 dB above the limit at the
worst node, **measured in rain**.

Stagger: each node transmits `node_id × 250 ms` after hearing a ping. Keep a
**queue of 4 pending reports**, drop the oldest if it fills, and count the drops in
the heartbeat — a full queue is the first sign the channel is oversubscribed.

## 5. Build and flash commands

```bash
# node, id baked in. compile --output-dir + upload --input-dir, because
# `arduino-cli upload` sends the artifacts of the LAST build of that sketch.
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DNODE_ID=1" \
  --output-dir /tmp/node1 node
arduino-cli upload --input-dir /tmp/node1 --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" /dev/cu.usbmodemXXXX

# gateway
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DBASE_ID=base3" \
  --output-dir /tmp/gw gateway
arduino-cli upload --input-dir /tmp/gw --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" /dev/cu.usbmodemXXXX

# bench base
arduino-cli compile --fqbn "esp8266:esp8266:nodemcuv2" --output-dir /tmp/bl base_link
arduino-cli upload --input-dir /tmp/bl --fqbn "esp8266:esp8266:nodemcuv2" /dev/cu.usbserialXXXX
```

Find the port by **USB vendor id**, not by the `usbmodem` suffix, which is
shuffled between reboots: the C3 is `0x303a`, the collar's nRF52 is `0x239a`.
`tools/monitor.py` and `tools/serial_cmd.py` already do this.

## 6. Symptom table for this build

| What you see | What it usually is |
|-------------|--------------------|
| Every SPI read returns **0xFF** | MISO/MOSI crossed |
| Every SPI read returns **0x00** | SCK never clocked (wrong pad, or the pad is a strap) |
| Node boots once, then never boots again when the cell sags | Divider or radio line on GPIO2/8/9 |
| No console output at all, board seems dead | USB CDC with nothing reading it, and an ungated print blocking `loop()` |
| Gateway hears **one** frame, then silence | `AT+PRECV` not re-armed after the receive event |
| Node silent, gateway silent, both boards fine | module is not a 9xx part — check the **part number**, `AT+VER` reports firmware only |
| 915 hop works on the bench, dies at a few metres outdoors | 915 radiator cut to 19 cm instead of **8.2 cm** |
| Node resets or goes dark as the cell sags | no 3.3 V buck-boost (AMS1117-class dropout), or no cell cutoff |
| Node hears collars well until it transmits | 433 and 915 antennas too close; compare the RA-02 noise floor with the RAK idle vs transmitting |
| RA-02 stops responding after weeks, rest of the node fine | no software reset path — RST must be on GPIO1 and the `RegVersion` watchdog running |
| Gateway drops many reports with 4+ nodes | SF too high for the number of nodes; check frame airtime against occupancy |
| The same 915 hop is worse on a cold morning | no TCXO on this part; a plain crystal drifts with temperature. Compare RSSI at both temperatures before blaming the antenna |
| Frames arrive, nothing decoded | `AT+P2P` differs on one field, or a new `AT+P2P` was sent while in RX (kills reception) |
| Node hears nothing on 433 | 915 antenna on the RA-02, air settings mismatch, DIO0 not on an interrupt pin |
| Node board resets mid-transmit, packets half-arrive | 3.3 V brown-out — 100 µF at the radio |
| Node alive, zero packets for hours | shade / battery: check panel voltage **first**, radio last |
| HA shows a second device with the same name | base id reused — `base3` is the node base, `cat-base2` is ESPHome |

## 7. Rules this build cannot break

- `AGENTS.md`: logging on the node is gated on `Serial.isPlugged()`
  (`isPlugged()` is the USB SOF watchdog, not DTR), `Serial.setTxTimeoutMs(0)` in
  `setup()`, `v` toggles verbosity. **No ungated print.** A blocked write stops the
  node, and the symptom is indistinguishable from a flat cell.
- DTR: **C3 low, nRF52 collar high.** Asserting DTR on the C3 resets it and drops
  the phone's BLE connection.
- The 433 air settings and the 6-byte collar frame are **not** touched by this
  build. Only the new 9-byte backhaul frame is added, in
  `libraries/CatTrackerBackhaul/backhaul_frame.h`, host-tested by
  `test_backhaul_frame.cpp`, and included by the node, the gateway, and
  `tools/locator.py` — three copies that must agree, same rule as
  `collar_frame.h` and its host test.
- Node ids are baked in with `-DNODE_ID=` and recorded in the node id → `AT+SN`
  → position table. That table is the only record of which module went where,
  exactly as the collar id → USB serial → cat table is for collars.
