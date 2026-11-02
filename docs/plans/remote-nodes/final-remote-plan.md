# Remote nodes — 433 listeners on a 915 backhaul

Final plan, 3 Nov 2026. Consolidated from `deepseek-remote-nodes-plan.md`,
`claude-remote-nodes-plan.md`, the repo docs (`collar/README.md`,
`homebase/README.md`, `multi-collar.md`, `AGENTS.md`, `GLOSSARY.md`), the code
(`collar/collar.ino`, `homebase/homebase.ino`, `homebase/esphome/`), and checks
run against the house (Home Assistant, the ESPHome dashboard dir, the broker).

The execution list is `TODO.md`. This file is the reasoning and the contracts.

---

## 1. Goal

Fixed **nodes** in the jungle that hear the 433 MHz collar beacons and report every
packet's RSSI and SNR to a **gateway** at the house over a separate 915 MHz link,
so the house can say *where* a cat most likely is, not only *whether* it has been
heard. Output is a most-likely region per cat, with an uncertainty radius and a
heat map, in Home Assistant.

The reason this is worth building: the handheld's 4-element Yagi is not
directional enough in the field — the operator cannot tell "closer" from
"uphill". Fixed receivers plus statistics replace walking the signal.

The handheld keeps its job. Nodes say which part of the property to walk to;
the handheld finds the cat in the last 20 metres.

## 2. The constraint that shapes everything

The collar is **433 MHz SX1278/RA-02**: 433.0 MHz, BW 125 kHz, SF9, CR 4/7,
sync `0x12`, preamble 8, explicit header, payload CRC on, 17 dBm. Six-byte frame
(`0xC1`, `seq`, `id`, `battery`, `flags`, reserved), ~140 ms airtime, ping period
`8000 ms + 137 ms × id`.

**RAK3172 is band-split and single-radio.** The datasheet splits it by variant
(verified 3 Nov from `docs.rakwireless.com/product-categories/wisduo/rak3172-module/datasheet/`):

| Variant | Bands it is built for | Channel limits |
|---------|---------------------|--------------|
| **RAK3172(L)** | EU433, CN470 | up to 25 kHz per channel, 10 mW |
| **RAK3172(H)** | EU868, US915, AU915, KR920, AS923-1/2/3/4, IN865, RU864 | up to 200 kHz per channel, 25 mW |

The **chip** supports EU433, so a 433 RAK3172 exists — it is the **(L)** variant,
not the one that talks 915. Same firmware, different RF matching: a **(H)** module
is not built for 433, and one radio is half-duplex. Label and confirm with `AT+VER`.

> **Every node needs two radios: an RA-02 at 433 to hear the collar, and a
> RAK3172(H) at 915 to report back.**

This is the design, not a complication. The node listens on 433 *while* it
transmits on 915, so it never goes deaf. **v1 changes no collar firmware**: the
existing five-way frame contract is untouched. What is added is one new contract
(the backhaul frame) and one new consumer (the locator).

## 3. Topology

```
collar 0/1/2 ──433──> handheld (ESP32-C3 + RA-02) ──BLE──> phone      [hunting]
            ──433──> base1 (ESP8266 + RA-02)  ──WiFi/MQTT──> m710q    [house, live]
            ──433──> cat-base2 (ESPHome)      ──native API──> HA       [house, offline today]
            ──433──> node1..nodeN (ESP32-C3 + RA-02 + RAK3172)
                          │ 433 RX: id, seq, raw rssi, snr, battery
                          └──915 LoRa P2P──> gateway/`base3` (ESP32-C3 + RAK3172 RX)
                                                  └──WiFi/MQTT──> m710q
                                                    └──> locator (Python) ──> HA: zone, fix, heat map
```

Naming, and why these names:

| Word | Meaning |
|------|---------|
| **Node** | A fixed, solar-powered 433 listener in the jungle. `-DNODE_ID=1..15`. |
| **Gateway** (alias **collector**) | The 915 receive end at the house. It is a base station, base id `base3`. |
| **Node 0** | The house's own 433 receiver, published in the same observation schema, so the locator can use it. |
| **Locator** | The Python service on `m710q` that turns per-node RSSI into a zone and a fix. |
| **Zone** | The node that hears a cat best. Coarse, nearly unbreakable. |
| **Fix** | A position estimate with a radius. Weaker, and labelled as such. |

**Why `base3` and not `base2`.** `cattracker/base2/#` is free on the broker —
`cat-base2` is ESPHome and publishes over the native API, not MQTT. But HA
already has devices and entities named from it: `sensor.cat_base_2_*` exist and
are all `unavailable` (the board is offline). A base id of `base2` on MQTT would
put a second device with the same identity next to the ESPHome one. `base3`
avoids that, and per `AGENTS.md` a base name is never reused.

## 4. Hardware

Per node:

| Part | Notes |
|------|-------|
| ESP32-C3 | USB CDC (`CDCOnBoot=cdc`). No BLE, so the handheld's 99%-of-partition wall does not apply. |
| RA-02 (SX1278) | 433 RX, same module and air settings as the collar. **433 antenna.** |
| **RAK3172-9-SM-NI** | 915 TX, RUI3 AT firmware on a hardware UART. **915 antenna, 8.2 cm.** `-NI` = **no IPEX connector**: solder a short coax pigtail to the RF pad, shield to the neighbouring GND pad. TX-only: a TX-only module stays awake (a few mA, **measure**) unless low-power mode is on; 1.69 µA is deep sleep, not idle. Wake the UART with a throwaway byte before each AT command. Not built for 433, so the 433-MHz RAK collar idea is out for this part. |
| ESP32-C3 board with **native USB** | GPIO20/21 must not go to a USB-UART bridge, because the node uses them for the RAK UART. |
| 18650 **with protection board** + CN3065/TP4056 | The charge module **charges and protects nothing**; the cell needs a low-voltage cutoff (~2.5–3.0 V), as `collar/README.md` already insists for the collar's LiPo. 100 µF across 3V3-GND at each radio, 100 nF at the ADC divider. |
| **3.3 V buck-boost from the cell** | A cell swings 4.2 → 3.0 V and a C3 board's onboard 5 V→3.3 V LDO has no headroom at 3.0 V in (MCP1700-class gives ~2.65 V out; the C3's digital supply spec is 3.0–3.6 V). Without a buck-boost the node browns out on the first cloudy night, and a brown-out reads as a radio fault. |
| Solar panel, sized for shade | Node needs ~2.6 Wh/day. Under canopy a panel delivers **10–20% of its rating**, so 5 W → 5–10 Wh/day over a 10 h day, which covers it with margin; 10 W doubles the margin. **Estimate; measure the panel's real current into the cell on a shaded day.** |
| IP65 box, 3–5 m pole | Drip loop on the panel cable, desiccant, boards conformal coated. Wet season is the real test. |

Per gateway: ESP32-C3 + RAK3172 RX on a hardware UART, WiFi for MQTT, 5 V mains
supply. RX current of the RAK3172 is low (5.22 mA typ), so it can listen
continuously. **Antennas needed: 3** — node 433, node 915, gateway 915.

Three inventory checks before anything is soldered:

- **Antennas are not interchangeable.** At the connector 433 and 915 antennas look
  alike, and the correct lengths are far apart (17.3 cm vs **8.2 cm**), so the
  length will not tell you which is which. Label every one at the pigtail. A 433
  antenna on the 915 port is ~0.52 λ and radiates badly; a 915 antenna on the 433
  port is a detuned monopole. Both read as a dead radio.
- **RAK3172 band: read the label, not `AT+VER`.** `AT+VER` reports the **firmware
  build**, not the band. The **ordering code** is the band: `…-9-SM-I` / `…-9-SM-NI`
  is the 9XX group (US915/AU915/KR920/AS923), `…-8-…` is 8XX (EU868/IN865/RU864),
  and the EU433/CN470 parts are the low-band variants. Record the label string and
  `AT+SN` in the node id table. The matching circuit is built for one band; a
  module in the wrong band is not a tuning problem.
- **Count the modules.** Bench needs 2, each node 1, the gateway 1.

### ESP32-C3 pin map (starting point, verify against the board's silkscreen)

ESP32-C3 traps: **GPIO2, GPIO8, GPIO9 are boot straps** (verified from the Espressif
schematic checklist: boot mode is controlled by the GPIO2/GPIO8/GPIO9 combination,
with SPI boot = GPIO2 1 / GPIO9 1, and joint-download boot = 1/1/0). **GPIO18/19
are USB D-/D+** — leave them alone if USB CDC must keep working, and note GPIO2/GPIO3
default to 10 mA drive strength, the other pins 20 mA.

Usable GPIOs: **0–10, 20, 21**. **GPIO2, GPIO8, GPIO9 are boot straps.**
GPIO11–17 are the on-board flash/SPI pads, GPIO18/19 are USB D-/D+. **GPIO20/21
are the U0RXD/U0TXD IO-mux functions, and they are free GPIOs when the console is
USB CDC** (the Arduino core defines `TX = 21`, `RX = 20` — check your board's
schematic, because some devkits wire an onboard USB-UART bridge to them).

So the node has **10 free non-strap pins** for 9 signals (SPI×4, DIO0, RST, UART×2,
ADC), and **the RA-02 RST keeps a real pin** — GPIO10 — which is what matters on an
unattended node: a stuck radio you can reset in software is not a walk into the
jungle. The RAK3172 UART goes on GPIO21 (TX) and GPIO20 (RX).

(From the Arduino core's `variants/esp32c3/pins_arduino.h`: `SCK`=4, `MISO`=5,
`MOSI`=6, `SS`=7, `TX`=21, `RX`=20, `A0`…`A5` = GPIO0…5.)

| Signal | Pin | Arduino alias | Why |
|--------|-----|---------------|-----|
| SX1278 SCK / MISO / MOSI | GPIO4 / 5 / 6 | `SCK`/`MISO`/`MOSI` | board's SPI defaults; matrix-routed anyway |
| SX1278 NSS | GPIO7 | `SS` | board's SPI default |
| SX1278 DIO0 | GPIO0 | `A0` | RX-done interrupt, interrupt-capable, not a strap |
| SX1278 NRESET (pin 7) | **GPIO1** | `A1` | resettable in software; **not** tied to 3V3 |
| Spare | **GPIO10** | — | suggested use: RAK3172 NRST (**verify** the pad name on your breakout) |
| RAK3172 UART1 TX → module RX | **GPIO21** | `TX` | matrix-routed UART1, 115200 8N1, free because the console is USB CDC |
| RAK3172 UART1 RX ← module TX | **GPIO20** | `RX` | matrix-routed UART1, free because the console is USB CDC |
| Battery divider | GPIO3 | `A3` | ADC1_CH3, 100k/100k + 100 nF, not a strap |
| Console | USB-C | `Serial` (HWCDC) | GPIO18/19 stay free for CDC |

**The battery divider must not land on a strap.** GPIO2, GPIO8, GPIO9 are
sampled at reset; a divider pulling one the wrong way is a node that will not
boot when the cell is low, which looks like a dead board, not a wiring fault.

**NRESET is not a hard tie, and RadioLib's "no pin" constant is `RADIOLIB_NC`**
(`Module(cs, irq, rst, gpio = RADIOLIB_NC)`), not `RST_NOT_SET`. The SX1278 datasheet
§7.2.1 says **"Pin 7 (NRESET) should be left floating during the POR sequence"**
and the host should wait 10 ms after POR before touching the SPI bus — so NRESET
gets a pull-up and a GPIO, never a hard connection to 3V3.

The gateway has more room, since it drives no SPI radio: UART1 on GPIO0 (TX) /
GPIO1 (RX), console on USB CDC. The step-by-step bench procedure, the NodeMCU
bench-base wiring, the antenna geometry, the decoupling rules, and the symptom
table for this build are in **`remote-node-wiring.md`**.

The ESPHome dashboard dir on `m710q` already has `esp32-c3.yaml` (LD2410 radar +
CC1101 sala controller) using GPIO3/GPIO4 on *that* board. It is unrelated to a
node, but it is the reason to write the node's pin map down rather than borrow
that board's.

### NodeMCU bench base (phase 1 only, throwaway)

`SoftwareSerial` at 115200 is only reliable **with WiFi off** — hence a
separate sketch for the bench, and a hardware UART for the real gateway.

| Link | Node (ESP32-C3) | Bench base (NodeMCU) |
|------|-----------------|---------------------|
| RAK TX2 → MCU RX | GPIO20 | D1 (GPIO5) |
| RAK RX2 ← MCU TX | GPIO21 | D2 (GPIO4) |

## 5. Backhaul frame (node → gateway)

**Per-ping, 9 bytes, magic `0xD1`.** One copy of the codec, in
`libraries/CatTrackerBackhaul/`, host-tested.

| Byte | Meaning |
|------|---------|
| 0 | `0xD1` magic/version. Never `0xC1` — that is the collar's. |
| 1 | node id, baked at flash with `-DNODE_ID=` |
| 2 | collar id |
| 3 | collar `seq`, copied unchanged |
| 4–5 | RSSI × 10, int16 big-endian |
| 6 | SNR × 2, int8 |
| 7 | collar battery (0xFF = unknown) |
| 8 | flags: collar flags in bits 0–1, node-side flags above |

Sent as ASCII hex: `AT+PSEND=<hex>` (18 hex characters).

**Why per-ping, not a batched report.** `seq` is the point of the frame. RSSI is
only comparable across nodes if it describes the *same transmission*, and the
locator's unit is the RSSI vector of one ping, grouped by `(collar id, seq)` in a
~2 s window. Seq wraps every 256 pings (~34 min), safe for that window.

A batched report on a fixed 8 s cycle was considered and **rejected**:
`homebase/homebase.ino` sizes its receive window from the ATtiny collar
(`COLLAR_PING_MS = 8000 + (MAX_COLLARS-1)*1000 + 1000`, `RX_TIMEOUT_MS = 11200`)
because the ATtiny's WDT runs **7–10 s for a nominal 8 s** (worst ~13 s). A node
keyed to an 8.000 s cycle sends reports that miss the ATtiny ping heard late in
the window. Per-ping with a node-id stagger survives that.

No CRC of our own: LoRa's payload CRC covers it, as on the collar. The magic byte
rejects stray frames.

**Collision avoidance.** Every node hears the same ping at the same instant, so
each node transmits `node_id × 250 ms` after reception. Frame airtime at SF7/125/CR
4/5 is **~41 ms** for the 9-byte frame, so a 250 ms slot fits comfortably — at
**SF9 it is ~144 ms (169 ms at CR 4/7), which only just fits one slot**, so the
stagger separates nothing between collars. Occupancy for 4 nodes × 3 collars per 8 s:
**SF7 ~6%, SF8 ~11%, SF9 ~22%, SF10 ~37%**. **Start at SF7**, raise one step only for
measured range. Each node keeps a queue of 4 pending reports, drops the oldest if it
fills, and counts the drops in the heartbeat. A node that hears
nothing sends a heartbeat (`flags` byte type) every ~64 s with node battery and
the noise floor — that is what makes "this node is dead" distinguishable from "no
cat nearby", i.e. **Blind vs Missing**.

The node's RX window stays ≥ the existing convention (11200 ms), continuous.

## 6. RAK3172 / RUI3 AT facts

Syntax below is from RAK docs and forums, **not verified on the owner's modules
yet**. Task 3 in `TODO.md` replaces every line with a real readback from
`AT+VER`, `AT+NWM=?`, `AT+P2P=?`, `AT+PSEND`, `AT+PRECV`, `+EVT:RXP2P`.

```
AT+NWM=0                        # P2P. Reboots the module and writes flash: query
                                # AT+NWM=? first and set only if needed.
AT+P2P=freq:SF:BW:CR:preamble:power
AT+PRECV=65534                  # continuous RX. 65535 = RX until one packet, 0 = stop.
+EVT:RXP2P:<rssi>:<snr>:<hex>   # the gateway gets the 915 RSSI and SNR for free
AT+PSEND=<hex>                  # ASCII hex. Module must not be in RX.
+EVT:TXP2P DONE                 # exists on firmware >= 4.0.5
AT+SYNCWORD                     # private sync word available if wanted
```

Traps, all silent:

1. **Re-arm `AT+PRECV` after every receive event**, on the RX-timeout event, and
   on a 60 s watchdog. Reports disagree on whether continuous RX survives a
   received packet; assume it does not. Forget it and the gateway hears exactly
   one frame, then looks like a dead network.
2. **`AT+PSEND` requires `AT+PRECV=0` first.**
3. **Issuing a new `AT+P2P=` while in continuous RX can silently stop
   reception.** Reconfigure only after `PRECV=0`.
4. **`AT+P2P` parameters must match on both ends** — frequency, SF, BW, CR,
   preamble. A mismatch is a gateway that receives nothing, indistinguishable
   from a dead RAK3172.
5. **Parameter encodings differ between RAK docs**: BW as 125/250/500 vs 0/1/2,
   CR 0 = 4/5. Read the config back after setting it; never trust the string you
   typed.
6. A node is TX-only on the RAK, the gateway is RX-only, so neither side needs
   the other's half.

Proposed 915 parameters: **915.000 MHz, SF7, BW 125 kHz, CR 0 (4/5), preamble 8, CRC
on, 14 dBm (25 mW)** — enough for the tree-to-house hop; there is no reason to run
the PA harder. **`AT+P2P=915000000:7:125:0:8:14`**, identical on the node, the
gateway, and the bench base. SF7 costs ~5–6 dB of sensitivity versus SF9
(demodulator limits: SF7 −7.5, SF8 −10, SF9 −12.5, SF10 −15); raise SF one step at a
time only if the measured hop needs it, aiming for hop SNR ~6 dB above the limit at
the worst node, measured in rain.

**The CR field is 0–3** (0 = 4/5, 2 = 4/7); a literal "4" is not a valid field value,
and the earlier readback example showing `…:4:…` was out of range. **Read
`AT+P2P=?` back on both modules and compare the strings** — never compare a typed
string to a typed string.

## 7. Node behaviour

**Listen:** RadioLib, 433 settings identical to the collar, continuous RX, DIO0
interrupt. Reject anything that is not a 6-byte `0xC1` frame. **Read RSSI before
SNR, immediately** — the same ordering rule `handheld_ble/handheld_ble.ino` uses,
because the registers are volatile across reads.

**The RSSI convention must match.** RadioLib's packet RSSI is SNR-corrected when
SNR < 0. The node, the house receivers, and the handheld must all report the same
convention, otherwise Phase 2's "±3 dB agreement" fails for a systematic reason.
Check which convention each build applies; do not assume.

**Report:** per ping, staggered `node_id × 250 ms`, `AT+PSEND` after `PRECV=0`.
Heartbeat every ~64 s when nothing was heard.

**Serial:** nodes are solar-powered, so the `AGENTS.md` rule applies without
exception. **No ungated `Serial.print`.** Gate every print on
`Serial.isPlugged()` (HWCDC on the C3), `Serial.setTxTimeoutMs(0)` in `setup()`,
`v` toggles the verbose report. A blocked write stops the node, and the symptom is
indistinguishable from a flat cell.

`v` report prints: node id, `AT+VER`, P2P readback, 433 registers (`FRF 0x6C4000`,
`MC1 0x76`, `MC2 0x94`, `Sync 0x12`, `InvIQ 0x27`, `PA 0xFF`, `RegVersion 0x12`),
per-collar rssi/snr/seq/heard/missed, node battery, noise floor.

## 8. Gateway behaviour

State table `roster[node][collar]`: `heard`, `missed` from `seq` gaps, smoothed
RSSI, arrival epoch from SNTP. Publish each observation non-retained, a throttled
`state` summary retained, availability retained with an `offline` last-will, and
discovery records on boot and on retry.

**A silent node is Blind, not Missing.** Publish each node's own in-range state so
Home Assistant can tell the plumbing from the cat — that is what stops the first
notification being a false alarm about a power cut.

## 9. MQTT schema

```
cattracker/base3/availability   retained  "online" / LWT "offline"
cattracker/base3/state          retained  throttled ~5 s summary
cattracker/base3/obs            not retained, one JSON per observation
homeassistant/sensor/base3_n1_battery/config ...
```

`state` keeps base1's shape and units so one HA `value_template` works for both
base stations: RSSI × 10, SNR × 2, integers.

`obs` carries the frame's own units untouched (dBm, dB), because the locator does
the maths and must not inherit a scaling it did not ask for.

**The rule: a unit is converted exactly once, at the boundary.**
`tools/monitor.py`'s parser is the reference for what the numbers mean.

## 10. House receivers as measurement points

The locator wants **raw per-packet** RSSI/SNR. Today:

- `homebase.ino` publishes **smoothed** RSSI (`rssiSmoothed × 10`) and last SNR.
- `cat-base2` ESPHome's `on_packet` **has** raw bytes, RSSI and SNR.
- `sensor.cat_base_2_*` exist in HA and are all `unavailable` — the board is
  offline, so making the house a measurement point starts by bringing it online.
- `sensor.chichi_chichi` and `binary_sensor.chichi_chichi` are live (last reported
  2 Nov), so HA's MQTT integration exists and `homebase/README.md`'s "no MQTT
  integration configured today" is **stale**. Update the README.
- `homebase.ino` is at **IRAM 94% (61699/65536, ~3.8 KB free)**. Raw per-packet
  publishing must not go into that sketch; it goes into the ESPHome build, as
  **node 0**, in the same `obs` schema.
- The `chichi_missing_10_minutes` / `chichi_back_in_range` automations must keep
  working after any of this.

`base1` can also vote as a fifth, very coarse receiver (smoothed, no `seq`) in the
zone tier only, with a wide sigma.

## 11. The locator

A Python service on `m710q`, not HA templates. Inputs: `cattracker/base3/obs`,
`tools/outposts.json` (where the nodes are and how each one lies), and
`cats.json` for names.

Three tiers, shipped in this order, published separately so it is always obvious
how much to trust the answer:

1. **Zone.** Per (cat, node), median RSSI of the last ~10 packets. Zone = argmax,
   with the margin over second place published as confidence. Single-packet RSSI
   in jungle multipath is a coin flip; a median is not. This tier alone answers
   the real question.
2. **Coarse fix.** Per node `d = d0 · 10^((P0 − rssi)/(10n))`, then a weighted
   centroid, with a radius from the residual spread.
3. **Likelihood grid.** 5 m cells over the surveyed area. Score each cell against
   every node's measurement. Publish the best cell, the 50% and 90% credible
   areas, and a heat map per cat.

The scoring model per node: `rssi = A_n − 10·n_n·log10(d) + noise(σ_n)`, with
`A`, `n`, `σ` fitted from the calibration walk. A k-nearest-neighbour fingerprint
map is the fallback if the model fits poorly. Smooth over time with a loose
random-walk motion prior: a cat moves slowly relative to an 8 s ping.

**Treat a node *not* hearing a ping as evidence.** The likelihood is the
probability the signal fell below receive sensitivity — SX1278 at SF9/BW125 is
around **−129 dBm typ at SF9/125 kHz (verified in the SX1278 datasheet, RF
sensitivity, Long-Range Mode, highest LNA gain)**; SF7 is ~5–6 dB worse, which is the
price of the lower occupancy the stagger needs. Negative evidence is
what makes a 3–4 node grid
work.

SNR carries more information than packet RSSI near the noise floor.

### Calibration matters more than the algorithm

At 50 m spacing, RSSI differences between neighbouring nodes are 3–10 dB, which
is the same order as the per-module offset. Uncalibrated, every fix is pulled
toward whichever SX1278 reads hot.

1. **Co-location.** All nodes on one table for 20 minutes. Same packets, several
   modules. Per-node `rssi_offset_db` = mean difference from the cross-node
   median. Free, and it removes a systematic bias.
2. **Walk test.** `transmitter/` already fakes three collars (`-DFAKE_COLLARS=3`).
   Carry it to 8–12 surveyed waypoints, 2 minutes each, and fit `p0`, `n`,
   `σ_n` (`walk.csv` layout; extend `tools/analyze_walk.py` or add
   `tools/fit_path_loss.py`).
3. **Validation.** Leave a collar at three known spots for 10 minutes and compare
   predicted against actual.

Node coordinates are surveyed with phone GPS into `tools/outposts.json`: lat, lon,
`height_m`, `rssi_offset_db`, and the path-loss model.

### Honest expectations

433 MHz through wet foliage gives hundreds of metres of link, but RSSI ranging
with one antenna on a moving cat gives **tens of metres**, and body absorption
(5–10 dB, directional, changes as the cat turns) is not removable by any
algorithm. On one hectare with 4 nodes, expect *"which third of the property"*
reliably, and a 20–50 m radius the rest of the time. The heat map exists to show
that uncertainty, not hide it.

## 12. Power — measured, never quoted

**Numbers verified from the datasheets, 3 Nov** (they replace the ~10–12 mA
figures in both earlier plans, which were right, and my own "114 mA" correction,
which was wrong — the SX1278 datasheet says 11.5 mA, not 114, and there is no
150 mA RA-02 figure):

| Item | Verified current | Source |
|------|-----------------|--------|
| SX1278 `IDDR` (receive) | **11.5 mA** typ (LnaBoost on, band 1) / **12.0 mA** (bands 2&3 — 433 MHz is band 2) | SX1276/77/78/79 datasheet, Table 5-1 |
| SX1278 `IDDFS` (synthesizer, FSRx) | 5.8 mA typ | same |
| SX1278 `IDDT` (transmit, +17 dBm PA_BOOST) | 87 mA typ | same |
| SX1278 standby / sleep | 1.6 mA / 0.2 µA | same |
| RA-02 module, RX | **12.15 mA** typ, standby 1.6 mA, max working 105 mA | Ai-Thinker Ra-02 spec V1.1 |
| ESP32-C3, modem-sleep with CPU running | **20 mA** typ @160 MHz, 15 mA @80 MHz | ESP32-C3 datasheet / Wireless Adventure Table 12.4 |
| ESP32-C3, light-sleep | 130 µA | same |
| ESP32-C3, deep-sleep (RTC) | 5 µA | same |
| ESP32-C3, WiFi RX (active) | 84 mA | same |
| RAK3172 RX | **5.22 mA** typ | RAK3172 datasheet |
| RAK3172 deep sleep | **1.69 µA** typ — deep sleep, **not idle** | same |
| RAK3172 TX | 87 mA typ @20 dBm | same |
| RAK3172 awake, idle | **not in the datasheet. Measure.** A TX-only module stays awake unless low-power mode is enabled | bench |

Node budget: RA-02 continuous RX **~12 mA** + C3 CPU running **~20 mA** + RAK3172
awake/idle (unspecified, **measure**) → planning figure **35–40 mA → 0.85–0.95
Ah/day → 3.2–3.5 Wh/day at 3.7 V**. With ~90% buck-boost and ~85% charger
efficiency the panel must deliver **~4.4 Wh/day**. A 5 W panel needs ~0.9 equivalent
full-sun hours, which under dense canopy is **marginal, not comfortable**: mount the
panel **above the canopy edge**, use **10 W**, and measure the real average current
and charge current before the first install. With no sun, expect **~2 days** from a
2000 mAh cell. **The continuous-RX design is affordable — duty cycling is a
battery-life option, not survival.** Measure the real average, the shaded panel
current, the regulator's efficiency, and the RAK's idle; the RA-02 PA peaks, the RAK
transmit burst (87 mA), and the brown-out margin are what will actually surprise us.

**The cell cannot feed the board directly.** 18650 swings 4.2 → 3.0 V, a C3 board's
onboard LDO has no headroom at 3.0 V in, and the C3's digital supply spec is
3.0–3.6 V, so the node needs a **3.3 V buck-boost** and a **protected cell /
low-voltage cutoff**; the CN3065 charges and protects nothing.

Options, in order:

1. 1×18650 (protected) + buck-boost + 5–10 W panel, node awake, continuous RX, RAK
   asleep between sends. The budget above says this works; the measurement says
   whether it works in shade.
2. RX duty cycle: power-gate the RA-02. 50% duty ≈ 16 mA average and ~50% packet
   loss — the power saving is now small, so this is a battery-life option, not a
   survival option. Loss must be quantified against the ATtiny's 7–10 s jitter, not
   the nRF collar's 8 s.
3. Windowed, phase-locked listening is **out**: the ATtiny pings on a watchdog
   sleep whose period is 7–10 s for a nominal 8 s, so a window would miss most of
   its packets. Continuous RX is the only scheme that works for every collar in
   the fleet.
4. The gateway is on mains, so its continuous RX (RAK3172 at 5.22 mA + C3) is a
   non-issue.

The RAK3172 transmit peak needs a solid 3.3 V and a local capacitor; the RA-02 PA
draws ~90 mA peaks at +17 dBm. Brown-out during transmit looks like "the board
resets randomly" or "some packets arrive and others do not" — not like a wiring
fault.

## 13. Phases and acceptance

| Phase | Deliverable | Acceptance |
|-------|-------------|------------|
| 1 | Prove the 915 hop: `node_link/` (C3, fake report every 8 s) + `base_link/` (NodeMCU, SoftwareSerial, **WiFi off**). **Drafted in the chat that wrote `claude-remote-nodes-plan.md`, not in this repo, never compiled — copy them in and set `PIN_RAK_RX = GPIO20`, `PIN_RAK_TX = GPIO21`, SF7.** | Base prints one decoded report per 8 s for 30 minutes with **0 missed seq** at a few metres, then at the longest distance expected in the field. Deliberate SF mismatch recorded as a known symptom. |
| 2 | Node with the 433 half: RA-02 via RadioLib, raw per-packet RSSI/SNR forwarded with `seq` and `id`, node-id stagger. | Node and a house receiver side by side: same `id` and `seq`, RSSI within ±3 dB. If they disagree systematically, find the RSSI/SNR-convention cause before calibrating offsets. |
| 3 | Gateway on C3 hardware UART + MQTT + HA discovery, `base3`. | `mosquitto_sub -t 'cattracker/#'` shows per-node observations; unplug a node and its age climbs without hiding the cats; last-will fires on a power cut. |
| 4 | House as node 0: bring `cat-base2` online, add raw `obs` in the ESPHome build. | Existing alerts keep working; raw per-packet data reaches the locator. |
| 5 | Power measured, enclosure, mounting 3–5 m, same antenna orientation, surveyed coordinates. | Measured average current and solar charge current written into `outpost/README.md`, replacing every estimate. |
| 6 | Locator zone tier + co-location calibration + walk fit. | `tools/test_locator.py` recovers synthetic known positions; an injected 10 dB fade does not move the zone; a silent node does not move the zone. Zone correct in most cycles of a real walk. |
| 7 | More nodes: 3 minimum, 4+ preferred. Spacing ≈ measured range ÷ 2. | Heat map and credible areas published per cat. |

Phase 2 is worth doing on its own: if a node's RSSI does not agree with a house
receiver at the same spot, nothing downstream can be trusted.

## 14. Failure modes, all of which look like something else

| Symptom | Actually |
|---------|----------|
| Node hears nothing | 915 antenna on the 433 module, or vice versa |
| Node hears nothing | Air settings mismatch: SF, BW, sync `0x12`, CRC, explicit header |
| Node hears nothing | DIO0 on a boot-strap pin (GPIO2/8/9 on the C3) |
| Node hears nothing | Crossed MISO/MOSI — every SPI read returns 0xFF, the same reading as a dead module |
| Node does not boot when the cell is low | Divider on a boot-strap pin |
| Gateway hears one frame then nothing | `AT+PRECV` not re-armed after reception |
| Gateway deaf at 915 | module is not a **9xx** part — check the **part number**; `AT+VER` reports firmware only |
| Gateway deaf at 915 | `AT+P2P` differs from the node's by one field, **including CR 0 vs 2**, or a new `AT+P2P` was sent while in RX |
| 915 hop works on the bench, dies outdoors | 915 radiator cut to 19 cm instead of **8.2 cm** |
| Node hears collars well until it transmits | 433 and 915 antennas too close; compare the RA-02 noise floor with the RAK idle vs transmitting |
| RA-02 stops responding after weeks, rest of the node fine | no software reset path — RST must be on GPIO1 and the `RegVersion` watchdog running |
| Gateway drops many reports with 4+ nodes | SF too high for the node count; check frame airtime against occupancy (~22% at SF9) |
| Frames arrive, locator ignores them | `seq` not forwarded, so nothing fuses |
| Fixes lean toward one node | Per-module RSSI offsets not calibrated |
| Fix jumps every cycle | Single-packet RSSI instead of a median |
| Fix is confidently wrong | Node positions are guesses, not surveyed |
| Node alive but silent | Solar/shade, not radio: check battery and panel voltage first |
| HA state flaps, dead base looks alive | Base name reused, or two devices with the same identity |
| Works on the bench, nothing at 200 m | 915 SF too low for wet-canopy path |
| Board resets randomly, packets partly arrive | 3.3 V brown-out during transmit — 100 µF at the radio |
| Node stops beaconing reports minutes after walking away | Ungated `Serial.print` on a USB CDC with nothing reading it |

## 15. Contracts to update

- **`AGENTS.md`**: the 915 air settings must match in `outpost/` and
  `gateway/`; the backhaul frame must match in `outpost/`, `gateway/` and
  `tools/locator.py`; `AT+PRECV` must be re-armed and `AT+PSEND` needs
  `PRECV=0`; a 433 antenna and a 915 antenna are not interchangeable; per-node
  offsets must be calibrated before any fix is believed; `base3` is never reused;
  nodes are flashed with `-DNODE_ID=` and recorded in the same
  id → `AT+SN` → position table as the collars' id → USB serial → cat table;
  `SoftwareSerial` at 115200 only with WiFi off; **never transmit from an RA-02
  or RAK3172 without an antenna** — it can permanently damage the output stage.
- **`GLOSSARY.md`**: add **Node**, **Uplink**, **Gateway/Collector**, **Zone**,
  **Fix**. Drop *node* from Base station's *Avoid* list — it now names a
  different device. Add *Avoid*: repeater, relay, satellite.
- **`README.md`**: a "Remote nodes" section, and the note that the phone app still
  does not see nodes — it talks BLE to the handheld, and the handheld has no room
  for a third radio.
- **`docs/adr/0002-two-radios-per-node.md`**: the band split is the reason.
  Recorded so nobody re-litigates a single-radio node.
- **Files**: `libraries/CatTrackerBackhaul/backhaul_frame.h` +
  `test_backhaul_frame.cpp` (one canonical codec, included by both sketches with
  `arduino-cli --libraries ./libraries`), `outpost/outpost.ino` + README,
  `gateway/gateway.ino` + README, `node_link/`, `base_link/`,
  `tools/locator.py`, `tools/test_locator.py`, `tools/fit_path_loss.py`,
  `tools/outposts.json`.
- The collar decode stays in `homebase/esphome/collar_frame.h`, compiled by the
  node; `test_collar_frame.cpp` must keep passing.

## 16. Standing rules that apply to all of this

- `arduino-cli upload` does **not** compile. Use `compile --output-dir` plus
  `upload --input-dir`, and bake ids with build flags; otherwise a stray compile
  puts the wrong id on a board with no error.
- Credentials: `homebase.ino` hardcodes WiFi credentials, `cat-base2.yaml`
  contains two passphrases, and `~/.ha_token` returns 401. New sketches take
  credentials from secrets or build flags. **Never paste credentials into prompts
  or commit them.**
- Do not change collar firmware, the 433 air settings, or the 6-byte frame.
  **Do not edit `homebase.ino` or ESPHome configs without asking.** ESPHome
  builds from its own dir on `m710q`
  (`/home/stefan/homeassistant/esphome-config/`), so a repo edit alone builds a
  stale copy — copy `cat-base2.yaml` **and** `collar_frame.h`.
- Fix one phase at a time and stop at each acceptance check. Do not start the
  locator before Phase 2 produces real data. Mark every untested assumption as
  untested. Do not quote a number that has not been measured.
- Byte-level decoding stays in host-testable headers, following
  `collar_frame.h` + its host test.

## 17. Deliberately out of scope for v1

- **Downlink.** `AT+PSEND` works both ways and the flags byte has room, but
  nothing needs to command a node yet.
- **Ranging.** Real distance needs a collar change (two power levels, or
  time-of-flight). RSSI only.
- **Windowed listening.** Cannot be phase-locked to an ATtiny collar.
- **WiFi-direct transport.** A node inside WiFi range of the house could publish
  straight to MQTT and skip the gateway. Kept as an escape hatch, not a v1 code
  path: it doubles the ways a node can be deaf. **Node 1 in a tree just outside
  the house is probably in WiFi range**, so WiFi-direct is the fastest way to
  prove the 433 half with no 915 in the loop. Decide from the open questions.
- **The phone app.** It talks BLE to the handheld. Locations live in HA.
- **More than one node.** One node answers "which node hears it best" only
  trivially; the grid needs 3–4.

## 18. Open questions

1. How many RAK3172 modules are available? Phase 1 needs 2; each node plus the
   gateway needs one.
2. Size of the area, and the realistic number of node sites.
3. Legal 915 MHz channel and power limit in the Philippines.
4. Node power: solar + 18650 everywhere, or mains at some sites?
5. Gateway firmware: Arduino + PubSubClient (matches `base1`), or ESPHome
   (matches `cat-base2`, gives `on_packet`, OTA, and the native API for free)?
6. Is the ESP8266 `base1` the house measurement point, or is `cat-base2` brought
   back online and given raw per-packet publishing? Only ESPHome has room.
7. Is node 1 in WiFi range? **Decided: run it WiFi-direct first.** It proves the 433
   half and the ±3 dB RSSI agreement against a house receiver with no second RAK3172
   in the loop; the 915 hop is added as a separate step, and WiFi stays as the escape
   hatch.

## 19. Sources verified 3 Nov 2026

Every number in this plan that is not "measure it" comes from these, fetched with
Ollama's web_search/web_fetch endpoints (see the tool-budget note below):

| Fact | Source |
|------|--------|
| RAK3172 band by ordering code: `-8-` = 8XX (EU868/IN865/RU864), `-9-` = 9XX (US915/AU915/KR920/AS923); low-band parts are EU433/CN470; RX 5.22 mA typ, TX 87 mA @20 dBm, sleep 1.69 µA; 25 mW/channel high band. `AT+VER` reports the firmware build, not the band | `docs.rakwireless.com/product-categories/wisduo/rak3172-module/datasheet/` |
| **The exact part on hand:** `RAK3172-9-SM-NI` = 9XX MHz, **No IPEX**, **without TCXO**, SKU **306041**. The `-T` variant table labels no-IPEX parts `(TP)`/`(TEP)`, which is where the module label's **`(P)`** marking comes from | same datasheet, "Ordering Information" and "RAK3172-T Variants" |
| SX1278 `IDDR` 11.5 mA (band 1) / 12.0 mA (bands 2&3), `IDDFS` 5.8 mA, `IDDT` 87 mA @+17 dBm, standby 1.6 mA, sleep 0.2 µA; SF9@125 kHz sensitivity −129 dBm typ; PA_LF to 525 MHz, PA_HF from 779 MHz; §7.2.1 **"Pin 7 (NRESET) should be left floating during the POR sequence"**, wait 10 ms after POR before SPI | `cdn.sparkfun.com/assets/a/a/0/b/5/DS_SX1276-7-8-9_W_APP_V5.pdf` |
| RA-02 module RX 12.15 mA, standby 1.6 mA, max working current 105 mA, band 410–525 MHz, +18 dBm max, −140 dBm | Ai-Thinker Ra-02 spec V1.1 (`docs.ai-thinker.com/Ra-02/`) |
| ESP32-C3 strapping pins GPIO2/8/9 and boot-mode table; GPIO2/3 10 mA default drive, GPIO18/19 40 mA | `docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c3/schematic-checklist.html` |
| ESP32-C3 currents: modem-sleep 20 mA @160 MHz / 15 mA @80 MHz, light-sleep 130 µA, deep-sleep 5 µA, WiFi RX 84 mA | `espressif.github.io/esp32-c3-book-en/chapter_12/12.2/12.2.4.html` (datasheet Table 5-7/5-9) |
| ESP32-C3 Arduino pin aliases: `TX`=21, `RX`=20, `SCK`=4, `MISO`=5, `MOSI`=6, `SS`=7, `A0`–`A5` = GPIO0–5, `PIN_RGB_LED`=8 — so GPIO20/21 are free GPIOs when the console is USB CDC | local core, `~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12/variants/esp32c3/pins_arduino.h` |
| RadioLib unused-pin constant is **`RADIOLIB_NC`**; `Module(cs, irq, rst, gpio = RADIOLIB_NC)` | `raw.githubusercontent.com/jgromes/RadioLib/master/src/Module.h` |


Two tool facts found on the way, worth repeating so nobody re-diagnoses them:

- `@ollama/pi-web-search` sizes its own output from the context window:
  `budget = 25% of window − tokens used − 5% safety`, and
  `fitResults(budget <= 0) → []`, which prints "No results found." So every search
  in a long session returns nothing even though the upstream search succeeded.
  The fix is a floor on the budget (`FLOOR_MIN_TOKENS = 1024`, applied to the
  installed copy; needs a restart). The workaround, which works immediately, is to
  hit the endpoint directly:
  `curl -X POST localhost:11434/api/experimental/web_search -d '{"query":"…","max_results":3}'`
  and `…/api/experimental/web_fetch -d '{"url":"…"}'`.
- `pi install npm:pi-exa` is installed. It registers `deep_search_exa` and the Exa
  MCP tools, and **needs an Exa key** (`EXA_API_KEY` env var, or `/exa-login`). Tools
  registered by a package load at startup, so a restart is needed before any of
  these tools appear in the session.
