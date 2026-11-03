# TODO

Execution list for `docs/plans/remote-nodes/final-remote-plan.md`. Read that file first; it has the
reasoning, the verified numbers and the sources. No code, files, commands, or
soldering until this list is approved (plan-first rule in `AGENTS.md`).

## Goal

Solar **nodes** in the jungle hear 433 MHz collar pings (RA-02) and report every
packet's RSSI/SNR to a **gateway** at the house over 915 MHz (RAK3172), which
publishes observations to Mosquitto for a locator that gives a most-likely region
per cat in Home Assistant. v1: gateway + node 1 in a tree just outside the house,
displayed as per-node dBm/SNR.

## Decisions (locked, verified)

- **Two radios per node.** RAK3172(L) is built for EU433/CN470, RAK3172(H) for
  EU868/US915/AU915/KR920/AS923/IN865/RU864, and one radio is half-duplex. A (H)
  module cannot hear 433. Node = RA-02 (433 RX) + RAK3172 (915 TX, TX-only).
  Gateway = ESP32-C3 + RAK3172 RX + WiFi. **Native-USB C3 boards only:** GPIO20/21 must
  not go to a board's USB-UART bridge. The part is **RAK3172-9-SM-NI** (9XX =
  US915/AU915/KR920/AS923, SKU 306041, **without TCXO**); `-NI` and the label's **`(P)`**
  marking both mean **no IPEX** — solder a coax pigtail to the RF pad. It is not built
  for 433, so the 433-MHz RAK collar idea is out for this part.
- **Per-ping 9-byte report, magic `0xD1`**: node id, collar id, collar `seq`, RSSI×10
  int16 BE, SNR×2 int8, collar battery, flags. Stagger `node_id × 250 ms` — sized for
  the frame's **~41 ms airtime at SF7**; at SF9 (~144 ms, 169 ms at CR 4/7) the stagger
  only just fits one frame and separates nothing between collars. Each node queues 4
  pending reports, drops the oldest on overflow, and counts drops in the heartbeat.
  Occupancy, 4 nodes × 3 collars per 8 s: SF7 ~6%, SF9 ~22%.
  Heartbeat every ~64 s when nothing heard. Batched reports rejected: the ATtiny
  collar's WDT runs 7–10 s for a nominal 8 s, so a fixed cycle misses it.
- Gateway base id **`base3`**, house 433 receiver published as **node 0**. `base2`
  is free on the broker, but ESPHome `cat-base2` owns `sensor.cat_base_2_*` in HA.
- Continuous 433 RX, RX window ≥ 11200 ms (existing `RX_TIMEOUT_MS` convention).
- Node budget: RA-02 RX ~12 mA + C3 CPU-on ~20 mA + **RAK3172 awake/idle, which the
  datasheet does not state (measure)** → planning **35–40 mA → 0.85–0.95 Ah/day →
  3.2–3.5 Wh/day at 3.7 V**. With ~90% buck-boost and ~85% charger efficiency the panel
  must deliver **~4.4 Wh/day**; 5 W needs ~0.9 equivalent full-sun hours, which under
  dense canopy is **marginal** → **10 W, mounted above the canopy edge**. ~2 days on a
  2000 mAh cell with no sun. **Continuous RX is affordable; duty cycling is a
  battery-life option, not survival.**
- **The cell does not power the board directly:** 18650 swings 4.2 → 3.0 V, the C3
  board's onboard LDO has no headroom at 3.0 V in, so the node needs a **3.3 V
  buck-boost** and a **protected cell / low-voltage cutoff** (CN3065 charges, protects
  nothing).
- Node ids baked with `-DNODE_ID=`, recorded in the node id → `AT+SN` → position
  table, same rule as collar id → USB serial → cat.
- 915 params: **`AT+P2P=915000000:7:125:0:8:14`** — 915.000 MHz, **SF7**, BW 125 kHz,
  **CR 0 = 4/5** (the field is 0–3), preamble 8, CRC on, 14 dBm (25 mW). Read
  `AT+P2P=?` back on both modules and compare strings; raise SF one step only for
  measured range (SF7 costs ~5–6 dB sensitivity vs SF9).
- Do not change collar firmware, the 433 air settings, or the 6-byte frame. Do not
  edit `homebase.ino` (IRAM 94%) or ESPHome configs without asking.

## Tasks

### 1. Pre-flight, before soldering
- [ ] Count the RAK3172 modules: bench needs 2, each node 1, gateway 1.
- [ ] Read each module's **ordering code on the label** against RAK's band table
      (`…-9-SM-I` = 9XX for US915/AU915/KR920/AS923; `…-8-…` = 8XX). `AT+VER` reports
      the **firmware build, not the band**. Record the label string and `AT+SN` of each
      module in the node table.
- [ ] Buy **3 antennas** (node 433, node 915, gateway 915). Label every antenna at
      the pigtail, 433 vs 915. They look alike at the connector; a swapped
      antenna is a node that hears nothing and looks like a dead radio.
- [ ] Resolve `base2` naming: retire `cat-base2.yaml` from the ESPHome dashboard dir,
      or confirm `base3`. Its `sensor.cat_base_2_*` entities exist and are
      `unavailable` (board offline).
- [ ] Fix `~/.ha_token` (401). HA **does** have MQTT (`sensor.chichi_chichi` live 2
      Nov), so update `homebase/README.md`, which says it does not.
- [ ] Credentials: `homebase.ino` hardcodes WiFi, `cat-base2.yaml` has both
      passphrases. New sketches take them from build flags/secrets; never paste
      credentials into prompts.

### 2. Backhaul frame, one canonical codec, host-tested
- [ ] `libraries/CatTrackerBackhaul/backhaul_frame.h`, header-only, encode + decode of
      the 9-byte frame plus the heartbeat variant.
- [ ] `libraries/CatTrackerBackhaul/test_backhaul_frame.cpp`: round-trip, truncation,
      wrong magic, node/collar id out of range, negative RSSI, battery 0xFF, seq wrap
      255→0, fixed hex vector. Mirrors `homebase/esphome/test_collar_frame.cpp`.
- [ ] `c++ -std=c++17 -o /tmp/t … && /tmp/t` passes before any firmware.
- [ ] Collar decode stays in `homebase/esphome/collar_frame.h`, compiled by the node;
      its host test keeps passing.

### 3. RAK3172 syntax proven on the module, not from docs — mostly done
- [x] Real `AT+VER`, `AT+NWM=?`, `AT+P2P=?`, `AT+PRECV=?` pasted into
      `poc915/README.md` (module `RAK3172-9-SM-NI`, firmware `RUI_4.0.6_RAK3172-E`).
- [x] **Bandwidth is a code, not kHz**: `AT+P2P=915000000:7:0:0:8:14` accepted,
      `…:7:125:…` → `AT_PARAM_ERROR`. The readback is `915000000:7:0:0:8:14`.
- [x] **`AT+PRECV` is persistent across reboot** — the module boots in continuous RX,
      so bring-up must be `PRECV=0` → set P2P → read back → arm. Measured reply:
      `P2P_RX_ON already, please disable P2P RX before setting. AT_BUSY_ERROR`.
- [x] `AT+PSEND` and `+EVT:RXP2P` recorded from a real hop: `[TX] done` / `[RX] node 1
      seq 4 rssi -13 snr 12 ok=5 miss=0`, board 1 ↔ board 2 on the bench
      (`poc915/README.md`). **RSSI −13 dBm is saturated near-field, not a range number.**
- [x] `AT+SN=?` returns **empty** on this module — `AT+VER` + the label string is the
      module record. The **board** MAC is the usable identifier (`esptool read_mac`).
- [x] **`AT+PRECV` is persistent across reboot** — recorded. `PRECV=0` is the arm
      command on this firmware (`P2P_RX_ON` is the *state query*, not the arm). In
      continuous RX no re-arm is needed after a receive; the 60 s watchdog re-arm is
      still to be recorded.
- [ ] Record that `AT+NWM=0` reboots and writes flash (query, set once), `AT+PSEND`
      needs `PRECV=0` first, and a new `AT+P2P=` during RX silently kills reception.
- [ ] Fix the 915 table (915.000, **SF7**, BW125, **CR 0 = 4/5**, preamble 8, CRC on) as
      one table used by node, gateway **and bench base**; probe the P2P sync word
      (`AT+SYNCWORD` exists).

### 4. Phase 1: prove the 915 hop — **DONE on the bench**

Follow `docs/plans/remote-nodes/remote-node-wiring.md` steps A and B for the bench, step by step, with the
check after each step.
- [x] `poc915/` exists, compiles for **three** roles (`-DROLE=0` listener,
      `-DROLE=1 -DNODE_ID=n` talker, `-DROLE=2 -DNODE_ID=n` node), **`PIN_RAK_RX =
      GPIO20`, `PIN_RAK_TX = GPIO21`**, SF7/CR 0. RX parse in `poc915/rak_event.h`,
      host-tested. **Run on hardware.** First hop: `[TX] done seq 4 ok=5` /
      `[RX] node 1 seq 4 rssi -13 snr 12 ok=5 miss=0`. Guide:
      `docs/plans/remote-nodes/poc-915-hop.md`; findings:
      `docs/plans/remote-nodes/FINDINGS.md`.
- [x] The listener is a second C3 (`-DROLE=0`); the NodeMCU bench base was never built.
- [x] `esp32:esp32:esp32c3:CDCOnBoot=cdc`, each role pinned to its own `--output-dir`.
- [x] 100 µF (+ 100 nF closest to the module) across 3V3-GND at each radio, antenna
      fitted before power.
- [x] **Node firmware exists as `ROLE=2` in `poc915`** — RA-02 on 433 (bit-bang SPI,
      SCK 4 / MISO 5 / MOSI 6 / NSS 7 / RST 1 / DIO0 0) decoding real collars through
      `homebase/esphome/collar_frame.h`, reporting over 915. Measured: 3 collars, RSSI
      −85…−111 dBm, `misaligned=0`, `crc_err=0`, `[TX] done ok=6`. This is the POC's
      shape, not the final `outpost/` firmware.
- [ ] **End-to-end acceptance not yet run:** node report received by the house board
      (`[RX] node 1 seq N cat <id> r<rssi>`), same `seq` and `cat` at both ends, RSSI
      within ±3 dB. Blocked only on powering the second board.
- [ ] Acceptance: 30 minutes, one report per 8 s, **0 missed seq** at a few metres,
      then at the longest distance expected in the field.
- [ ] Deliberate SF mismatch recorded as a known symptom.
- [ ] Replace the POC's ASCII payload (`CAT<n> <seq> c<id> r<rssi>`) with the task-2
      binary codec. The ASCII form is provisional: it is readable on two consoles, which
      is what made two silent firmware bugs visible, and it carries **one** collar per
      report.

### 5. Node firmware (`outpost/outpost.ino`, ESP32-C3)
- [ ] Pin map, as `docs/plans/remote-nodes/remote-node-wiring.md`: SX1278 SCK/MISO/MOSI/NSS on GPIO4/5/6/7
      (the core's SPI defaults), **DIO0 on GPIO0**, **RAK3172 UART1 TX on GPIO21 and RX
      on GPIO20** (free because the console is USB CDC — check the board schematic for an
      onboard USB-UART bridge on those pads), battery divider on **GPIO3** (ADC1_CH3),
      straps GPIO2/8/9 and USB GPIO18/19 untouched. **RA-02 NRESET on GPIO1**, never
      tied hard to 3V3: SX1278 datasheet §7.2.1 says NRESET is **floating during the
      POR sequence**, and the chip can drive it during power-on. **GPIO10 is the spare**
      (suggested use: RAK3172 NRST, **verify** the pad). RadioLib's unused-pin constant is
      **`RADIOLIB_NC`**, not `RST_NOT_SET`. Cross every row against the board's
      silkscreen before soldering.
- [ ] 433 RX identical to the collar, continuous, DIO0 interrupt, window ≥ 11200 ms.
- [ ] **Read RSSI before SNR**, immediately; record which SNR-correction convention
      RadioLib applies and confirm the house receivers use the same one.
- [ ] Decode the 6-byte collar frame via `collar_frame.h`.
- [ ] Report each ping, staggered `node_id × 250 ms`, `AT+PSEND` after `PRECV=0`;
      heartbeat every ~64 s when nothing heard.
- [ ] Node battery: 100k/100k + 100 nF at the ADC pin, 0..100, 0xFF unwired.
- [ ] Logging gated on `Serial.isPlugged()` (HWCDC), `setTxTimeoutMs(0)` in `setup()`,
      `v` toggles. No ungated print.
- [ ] `v` report: node id, `AT+VER`, P2P readback, 433 registers (FRF 0x6C4000, MC1
      0x76, MC2 0x94, Sync 0x12, InvIQ 0x27, PA 0xFF, RegVersion 0x12), per-collar
      rssi/snr/seq/heard/missed, node battery, noise floor.
- [ ] RX duty-cycle power gate as a compile-time option, off by default.
- [ ] Build/flash commands in `outpost/README.md` with `-DNODE_ID=1`.

### 6. Phase 2 acceptance: the node agrees with the house
- [x] **Two nodes agree on collar identity**: 29 `(id, seq)` pairs heard by both, seqs
      identical, so the collar ids in the house are unique. Bench pass recorded in
      `FINDINGS.md` §6.
- [x] **Node + BASE (`ROLE=3`) match the same collar packet**: the report carries the
      collar's seq, so the base's own RSSI and the node's RSSI are compared on one ping,
      not on a time window. `matched 13, unmatched 0`.
- [ ] Node and a house receiver side by side: same `id` and `seq`, RSSI within ±3 dB.
      **Measured and NOT met.** Packet-matched: cat 0 **+5**, cat 1 **+4…+6**, cat 2
      **−12…−13 dB**. The sign flips between collars, so the term is geometry (which
      collar is nearer to which board), not board bias. The band is only meaningful for
      boards at the same point, and even co-located the bench spread was −2…+5 dB.
- [ ] `transmitter/` (`-DFAKE_COLLARS=3`) exercises node and gateway indoors, no cats.
- [ ] Systematic RSSI disagreement → find the RSSI/SNR-convention cause before
      calibrating offsets.

### 7. Gateway firmware (`gateway/gateway.ino`, ESP32-C3 + RAK3172 + WiFi/MQTT)
- [ ] RX side with `PRECV` re-arm, decode via the shared codec, `roster[node][collar]`
      with `heard`/`missed` from `seq` gaps, smoothed RSSI, arrival epoch from SNTP.
- [ ] Non-retained `cattracker/base3/obs` per observation (node, collar, seq, rssi10,
      snr2, batt, hop_rssi, hop_snr, gateway timestamp); throttled retained `state`;
      retained `availability` with `offline` last-will.
- [ ] HA discovery: per node per cat signal/SNR/last seen; per node battery, packets,
      in-range, last seen; base packets and unknown drops. Unique ids include node id
      and `base3`.
- [ ] A silent node is Blind, not Missing: publish node in-range so HA separates them.
- [ ] Units: `state` dBm×10 and dB×2 like base1; `obs` in the frame's own units.
      Convert once, at the boundary; `tools/monitor.py`'s parser is the reference.
- [ ] Serial `v` report of every node row in the same integer scaling as the JSON.
- [ ] Build/flash commands with `-DBASE_ID=base3`.

### 8. House receiver as node 0
- [ ] Bring `cat-base2` online first (its entities are `unavailable`).
- [ ] Add raw per-packet observations to the ESPHome build, same `obs` schema, node 0.
      **Not** `homebase.ino` (IRAM 94%, ~3.8 KB free).
- [ ] Confirm base1's entities and `chichi_missing_10_minutes` /
      `chichi_back_in_range` keep working.

### 9. Power, measured
- [ ] Measure node average current with 433 RX running, uplink idle, against the
      budget (12 mA RA-02 RX + 20 mA C3 modem-sleep + **RAK3172 idle, which the datasheet
      does not give** = 35–40 mA planning figure). The datasheet is not the measurement,
      and 1.69 µA is deep sleep, not idle.
- [ ] Measure current during RAK transmit (87 mA typ); **the buck-boost's efficiency**;
      solar charge current into the 18650 in sun and **under canopy shade** (a panel
      delivers 10–20% of its rating there); brown-out margin during transmit.
- [ ] Confirm the **low-voltage cutoff** (~3.2 V) trips before the cell is damaged, that
      the buck-boost holds 3.3 V at 3.0 V cell, and that the board's own USB regulator
      does not fight it when the battery is connected — flash with the battery
      disconnected, or disable EN on USB.
- [ ] Decide panel size / cell count / duty cycle from the measured average.
- [ ] Write the measured numbers into `outpost/README.md`, replacing every estimate.

### 10. Field install
- [ ] IP65 box, 3–5 m pole, drip loop on the panel cable, desiccant, boards conformal
      coated.
- [ ] 915 antenna: quarter wave **8.2 cm** (λ = 299.79/f, 32.8 cm at 915 MHz; start a
      bare wire at 7.8 cm and trim while watching the bench hop's RSSI) + counterpoise,
      geometry fixed rigidly, same orientation on every node, never transmit without an
      antenna. 433 antenna keeps the collar's **17.3 cm** geometry rules — the two
      lengths differ by ~2.1×, the connectors do not.
- [ ] **Separate the node's two antennas by tens of cm** — 25 mW transmitting at 8.2 cm
      from an RA-02 trying to hear −129 dBm at 433 is not a small desense. Verify by
      listening to a collar while transmitting.
- [ ] Survey positions with phone GPS into `tools/outposts.json` (lat, lon, height_m,
      rssi_offset_db, name).
- [ ] **Per-collar offset, not only per-node.** Measured on the bench: with two boards in
      the same room, per-collar cross-node differences were −0.2 / +0.8 / +4.0 dB. A
      single `rssi_offset_db` per node cannot absorb that. Either calibrate a collar term
      too, or constrain the locator so a per-node offset cannot move the zone by itself.
- [ ] Co-location calibration on one table 20 min: `rssi_offset_db` = mean difference
      from the cross-node median.
- [ ] Walk test with `transmitter/` at 8–12 waypoints, 2 min each, CSV logged.
- [ ] Log a day from the installed tree node; write results into `outpost/README.md`.

### 11. Locator on m710q
- [ ] `tools/locator.py`: group by `(collar, seq)` in a 2 s window; median RSSI of the
      last ~10 per (cat, node); zone = argmax, confidence = margin over second place.
      **Cross-node absolute RSSI is the wrong quantity at the measured spread** (13 dB
      of geometry on one collar): compare each node's RSSI for a collar against that
      node's own baseline for that collar, not against another node's number.
- [ ] Treat a missing observation as evidence: likelihood the signal fell below
      sensitivity — SX1278 SF9/BW125 **−129 dBm typ** (datasheet), SF7 ~5–6 dB worse,
      measured on the bench.
- [ ] `tools/fit_path_loss.py`: fit per-node `A_n, n_n, σ_n` from the walk.
- [ ] `tools/test_locator.py`: synthetic RSSI from known positions recovers the zone;
      an injected 10 dB fade does not move the zone; a silent node does not move it.
- [ ] Publish best cell, 50% and 90% credible areas, radius, tier, heat map per cat.
- [ ] Deferred and recorded: likelihood grid over 5 m cells, random-walk motion prior,
      kNN fingerprint fallback, `base1` voting as a coarse receiver.

### 12. Tool in step with the Report
- [ ] Extend `tools/monitor.py` to parse the gateway's node line (`ND <ver> n= pk= unk=`).
- [ ] Add the exact bytes the firmware emits to `tools/test_monitor.py`; it passes.
- [ ] `--csv` rows for node observations.

### 13. Docs that keep the contracts true
- [ ] `outpost/README.md` (wiring, AT setup, power, antenna, bring-up, troubleshooting
      table) and `gateway/README.md` (RAK traps, MQTT schema, discovery).
- [ ] Root `README.md`: "Remote nodes" section; the phone app still does not see nodes.
- [ ] `GLOSSARY.md`: add **Node**, **Uplink**, **Gateway/Collector**, **Zone**, **Fix**;
      drop *node* from Base station's *Avoid*; add *Avoid*: repeater, relay, satellite.
- [ ] `AGENTS.md`: 915 settings must match in `outpost/` and `gateway/`; the backhaul
      frame must match in `outpost/`, `gateway/` and `tools/locator.py`; `PRECV`
      re-armed, `PSEND` needs `PRECV=0`; 433 and 915 antennas not interchangeable;
      per-node offsets calibrated before a fix is believed; `base3` never reused;
      `-DNODE_ID=` recorded in the id → `AT+SN` → position table; SoftwareSerial only
      with WiFi off; never transmit from an RA-02/RAK3172 without an antenna.
- [ ] `docs/adr/0002-two-radios-per-node.md` — the band split is the reason.
- [ ] `docs/plans/` note: WiFi-direct escape hatch, and node 1 in WiFi range as the
      fastest way to prove the 433 half with no 915 in the loop.

### 14. Phase 7: more nodes
- [ ] 3 nodes minimum, 4+ preferred, spacing ≈ measured range ÷ 2. Heat map and
      credible areas published per cat.

## Notes
- 915 suffers more canopy attenuation than 433: the uplink is the shorter link, so
  the gateway goes high and in sight of the node. **SF7 start** (occupancy and airtime),
SF8/SF9/SF10 only if the measured hop needs the sensitivity.
- Honest expectation: "which third of the property" reliably, 20–50 m radius
  sometimes. Body absorption (5–10 dB, directional) is not removable by any algorithm.
- Failure modes that look like something else are in `docs/plans/remote-nodes/final-remote-plan.md` §14.
- Out of scope v1: downlink, ranging (needs a collar change), windowed RX (ATtiny
  jitter), WiFi-direct, the phone app, more than one node.

## Open questions (blocking, answer before task 4)
1. How many RAK3172 modules are on hand? Bench needs 2, gateway 1, each node 1.
2. Area size and realistic number of node sites.
3. Gateway firmware: Arduino + PubSubClient (matches `base1`) or ESPHome (matches
   `cat-base2`, gives `on_packet`, OTA, native API)?
4. House measurement point: ESP8266 `base1` (smoothed only, IRAM 94%) or `cat-base2`
   brought online with raw per-packet publishing?
5. Is node 1 in WiFi range? **Decided: run it WiFi-direct first** — prove the 433 half
   and the ±3 dB RSSI agreement with no second RAK3172 in the loop, then add the 915
   hop as a separate step.
