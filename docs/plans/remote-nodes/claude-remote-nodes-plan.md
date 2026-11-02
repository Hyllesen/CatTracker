# Cat tracker: remote RSSI nodes (handoff plan)

Prepared 3 Nov 2026 from the owner's repo docs (homebase and collar READMEs, homebase.ino, collar.ino) and a design chat. Status: planning. The Phase 1 bench sketches exist but have not been compiled or run.

## 1. Goal

Locate a cat in dense jungle (no WiFi or mobile coverage at the site) from the RSSI and SNR of its 433 MHz collar beacon, measured at several fixed remote nodes. The nodes forward their measurements over a separate 915 MHz LoRa link to a base station on the house network. Output: a most likely position region per cat (best estimate plus uncertainty and a heatmap), shown in Home Assistant.

Motivation: the homemade 4-element Yagi on the handheld is not directional enough in the field (the operator cannot tell closer from uphill or downhill), so fixed receivers plus statistics replace walking the signal.

## 2. Existing system (do not break it)

**Collars** (up to 3: ids 0..2 named Chichi, Onanda, Chacha). TENSTAR nRF52840 Pro Micro plus RA-02 (SX1278), 150 mAh LiPo, one-way beacons, no ACK. Another small collar (ATtiny85 plus Ra-02, same air settings and frame) is planned.

- Air settings: 433.0 MHz, BW 125 kHz, SF9, CR 4/7, sync word 0x12, preamble 8, explicit header, payload CRC on, 17 dBm.
- Frame, 6 bytes: 0xC1 magic, seq (wraps 0..255), id (0..2), battery (0..100, or 0xFF unknown), flags (bit0 = low battery), reserved.
- Airtime about 140 ms. Ping period is 8000 ms + 137 ms x id (collar 0: 8.000 s, 1: 8.137 s, 2: 8.274 s) so collars drift apart instead of colliding forever.

**Homebase** (fixed receiver, works today). NodeMCU V3 (ESP-12E) plus RA-02 on HSPI: SCK GPIO14, MISO GPIO12, MOSI GPIO13, NSS GPIO15, DIO0 GPIO4, RST GPIO2. Arduino sketch using RadioLib, blocking radio.receive() with an 11.2 s timeout. Publishes MQTT to Mosquitto on the m710q (192.168.100.66:1883) with Home Assistant discovery. RSSI is smoothed over 12 packets and published as dBm x 10; SNR is published as dB x 2. IRAM is at 94%, so adding features to that sketch can fail at link time.

A second homebase build exists as ESPHome (cat-base2, sx127x component, on\_packet gives raw bytes plus RSSI and SNR, native API, static IP, no MQTT). Home Assistant has per-cat missing-for-10-minutes alerts.

**Handheld.** ESP32-C3 plus BLE and a phone app showing raw RSSI per ping.

Important gap: raw per-packet RSSI and SNR exist inside the firmware, but homebase publishes only the smoothed values. The locator needs raw per-packet values.

## 3. Decisions

**Stated by the owner:**

- Add remote nodes in the jungle that read each collar's RSSI/dBm/SNR and report to the base station, to deduce where a cat most likely is.
- Use RAK3172 modules at 915 MHz for node-to-base communication.
- Hardware available for base and nodes: NodeMCU ESP8266 and ESP32-C3 boards.
- Start simple: one base station and one remote node communicating, before adding more nodes.

**Proposed in the design chat (confirm before relying on them):**

- Node = ESP32-C3 + RA-02 (433 MHz receive) + RAK3172 (915 MHz transmit). Two radios because the RAK3172 has a single half-duplex radio and its RF matching is tuned for 915.
- Use identical RA-02 modules on every node so RSSI is comparable between nodes; measure per-node offsets side by side.
- Gateway = a separate ESP32-C3 + RAK3172 (hardware UART, receive-only), leaving the working homebase alone.
- The locator is a small Python service on the m710q, not firmware.

## 4. Architecture

```
collar --433 MHz LoRa--> node A (ESP32-C3 + RA-02 + RAK3172) --915 MHz--> gateway (ESP32-C3 + RAK3172) --WiFi--> m710q (MQTT + locator) --> Home Assistant
collar --433 MHz LoRa--> node B (same)                       --915 MHz-->        |
collar --433 MHz LoRa--> homebase (existing NodeMCU) -------- WiFi/MQTT ------------------------------------> m710q
```

The collar link stays exactly as it is. Nodes are passive listeners on 433 MHz and add nothing to its airtime.

## 5. Hardware per role

| Role | Parts | Notes |
| --- | --- | --- |
| Remote node | ESP32-C3, RA-02 (433), RAK3172 (915), antennas for both, power | Continuous RX, solar plus 18650 likely |
| Gateway | ESP32-C3, RAK3172, WiFi | RAK3172 on hardware UART, receive-only |
| Homebase | NodeMCU plus RA-02 (existing) | Counts as a measurement point once it publishes raw per-packet data |
| Bench base (Phase 1 only) | NodeMCU plus RAK3172 on SoftwareSerial, WiFi off | Separate throwaway sketch |

Phase 1 wiring (3.3 V logic, common ground, 100 uF plus 100 nF across 3V3-GND at each RAK module, antenna fitted before power):

| Link | Node (ESP32-C3) | Bench base (NodeMCU) |
| --- | --- | --- |
| RAK TX2 to MCU RX | GPIO20 | D1 (GPIO5) |
| RAK RX2 from MCU TX | GPIO21 | D2 (GPIO4) |

The RAK3172 AT port is the TX2/RX2 pair, default 115200 baud.

## 6. Radio parameters

**433 MHz collar link (must match the collars exactly):** values in section 2.

**915 MHz hop (proposed):** `AT+P2P=915000000:9:125:0:8:14`, meaning 915.000 MHz, SF9, BW 125, CR 4/5, preamble 8, 14 dBm. SF8 or SF9 at 125 kHz gives margin through foliage. Verify the legal 915 MHz channel for the Philippines before deployment (believed to be 915-918 MHz for low-power LoRa, not confirmed).

## 7. Report frame (node to gateway, 9 bytes)

| Byte | Meaning |
| --- | --- |
| 0 | 0xD1 magic |
| 1 | node id |
| 2 | collar id |
| 3 | collar seq (copied from the collar frame) |
| 4-5 | RSSI x 10, int16 big-endian (dBm, SNR-corrected if needed) |
| 6 | SNR x 2, int8 |
| 7 | collar battery (0xFF unknown) |
| 8 | flags (collar flags, node-side flags) |

Sent as hex through `AT+PSEND=<hex>` (about 18 hex characters). The key `(collar id, seq)` identifies one collar ping, so the same ping heard by several nodes can be grouped without synchronised clocks. Seq wraps every 256 pings (about 34 minutes), which is safe for a 2 s grouping window.

**Collision avoidance (required):** every node hears the same ping at the same instant, so each node waits `node_id x ~250 ms` after reception before transmitting its report.

## 8. RAK3172 (RUI3 AT firmware) notes

Syntax was taken from RAK's docs and forum, not tested on the owner's firmware. Check `AT+VER=?` first and compare with the current RAK AT manual.

- `AT+NWM=0` selects P2P. It reboots the module and writes flash, so query `AT+NWM=?` and only set it if needed.
- `AT+P2P=freq:SF:BW:CR:preamble:power`. BW is accepted as 125/250/500, and some docs use 0/1/2. CR 0 = 4/5. `AT+SYNCWORD` exists if a private sync word is wanted.
- `AT+PSEND=<hex>` takes ASCII hex. The module must not be in RX: send `AT+PRECV=0` first. `+EVT:TXP2P DONE` is emitted on firmware 4.0.5 and later.
- `AT+PRECV=65534` is continuous RX, 65535 is RX until one packet, 0 stops RX. Reports differ on whether continuous RX survives a received packet, so re-arm (`PRECV=0` then `65534`) after every receive event and on a 60 s watchdog.
- Receive event: `+EVT:RXP2P:<rssi>:<snr>:<hex payload>`. Other events are `RXP2P RECEIVE TIMEOUT` and `RECEIVE ERROR`.
- Sending a new `AT+P2P=` while in continuous RX can silently stop reception. Reconfigure only after `PRECV=0`.
- A node only transmits, so it never needs RX on the RAK3172. The gateway only receives.

## 9. Phases and acceptance criteria

**Phase 1: prove the 915 MHz hop (files written, untested).** `node_link/node_link.ino` (ESP32-C3, sends a fake report every 8 s) and `base_link/base_link.ino` (NodeMCU, receives and decodes, prints collar RSSI/SNR from the payload plus the hop's own RSSI/SNR and a missed-seq counter). Build lines: `arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" --build-property "compiler.cpp.extra_flags=-DNODE_ID=1" node_link` and `arduino-cli compile --fqbn "esp8266:esp8266:nodemcuv2" base_link`. Acceptance: base prints one decoded report per 8 s for 30 minutes with 0 missed seq at a few metres, then at the longest distance expected in the field.

**Phase 2: real collar data on the node.** Add the RA-02 to the node using RadioLib with the 433 MHz air settings above. Reuse the six-byte frame decode (see esphome/collar\_frame.h and its host test in the repo). Forward raw per-packet RSSI and SNR with the collar's seq and id. Apply the node-id stagger. Acceptance: with a real collar, the base shows the same seq and id the node heard, and the node's RSSI agrees with the existing homebase RSSI within a few dB when both sit side by side.

**Phase 3: gateway to MQTT.** Move the 915 receive end to an ESP32-C3 + RAK3172 on a hardware UART. Publish each observation to MQTT. Proposed topic: `cattracker/nodes/<node>/<collar>/obs` with JSON fields seq, rssi10, snr2, batt, hop\_rssi, hop\_snr and a gateway timestamp, not retained. Gateway availability uses a retained last-will topic so a silent gateway is distinguishable from a missing cat. Also extend homebase (or the ESPHome build) to emit raw per-packet observations in the same schema, as node 0. Do this without breaking the existing alerts or exceeding the IRAM budget.

**Phase 4: field nodes.** More nodes (3 minimum, 4 or more preferred), enclosures, mounting high (about 3-5 m), same antenna orientation, power (see section 12), survey each node's coordinates.

**Phase 5: locator service on the m710q.** See section 10.

**Phase 6: calibration and dashboard.** Walk with a collar on a known GPS track, log per-node RSSI, fit the model, add a Home Assistant view (best position, uncertainty radius, heatmap image).

## 10. Localization approach

Expect a region, not a point. In dense jungle RSSI fluctuates by roughly 6-10 dB from foliage and multipath, plus the cat's body and collar antenna orientation. Direct RSSI-to-distance and circle intersection will perform badly.

1. Group observations by `(collar id, seq)` within a window of about 2 s. Each group is one RSSI vector across nodes.
2. Model per node: `rssi = A_n - 10 * n_n * log10(d) + noise(sigma_n)`, with A, n and sigma fitted from the calibration walk. A fingerprint (k-nearest-neighbour) map is an alternative if the model fits poorly.
3. Score a grid of candidate positions (for example 5 m cells in a local XY frame built from node GPS positions) by likelihood.
4. Treat a node not hearing a ping as evidence: the likelihood is the probability that the expected signal falls below the receive sensitivity (SX1278 at SF9/BW125 is around -129 dBm; measure it).
5. Smooth over time with a loose random-walk motion prior, since a cat moves slowly relative to an 8 s ping.
6. Near the noise floor, SNR carries more information than packet RSSI. Check whether the library's packet RSSI is already SNR-corrected for SNR below 0, and apply the correction if not.
7. Publish the best cell, 50% and 90% credible areas, and a heatmap per cat to MQTT.

## 11. Constraints and pitfalls from the repo

- Never transmit from an RA-02 or RAK3172 without an antenna attached; it can permanently damage the output stage.
- ESP8266 SPI: read the GPIO number, not the D label (D6 and D7 are not in GPIO order; crossed MISO and MOSI reads 0xFF and looks like a dead module). DIO0 must not be on GPIO16 (no interrupts). GPIO0, GPIO2 and GPIO15 are boot straps.
- Never feed the RA-02 or RAK3172 from 5 V or VIN. Put 100 uF across 3V3-GND at each radio; the RA-02 PA draws about 90 mA peaks.
- A native-USB CDC serial port blocks when nothing reads it. Do not add unconditional Serial printing to anything that must keep transmitting or receiving unattended.
- `arduino-cli upload` does not compile; it sends the last build. Use `--output-dir` and `--input-dir`, and bake node ids in with build flags.
- SoftwareSerial at 115200 is only reliable with WiFi off (hence the C3 gateway).
- Keep the collar air settings and six-byte frame unchanged; every receiver depends on them.
- homebase.ino currently has WiFi credentials hardcoded. Do not paste credentials into LLM prompts or commit them; supply them through secrets or build flags.

## 12. Power (rough, unmeasured)

A node in continuous receive likely draws about 30 mA (SX1278 RX roughly 10-12 mA plus an ESP32-C3 without sleep), which is only a few days from one 18650. Plan for a small solar panel plus an 18650, then reduce consumption later by sleeping between predicted pings. The RAK3172 transmit peak needs a solid 3.3 V supply and a local capacitor. Measure before quoting any figure.

## 13. Open questions

1. How many RAK3172 modules are available? Phase 1 needs two; each node plus the gateway need one each.
2. Size of the area and realistic number of node sites.
3. Legal 915 MHz channel and transmit power in the Philippines.
4. Node power: solar plus 18650, or mains where possible.
5. Gateway firmware: Arduino plus MQTT, or ESPHome (the existing alerts use both styles).
6. Whether to modify homebase to publish raw observations, or to run a new ESPHome board as an extra measurement point.

## 14. Working rules for the implementing LLM

- Work one phase at a time and stop for confirmation at each acceptance check. Do not start the locator before Phase 2 produces real data.
- Do not change collar firmware, the 433 MHz air settings or the six-byte frame. Do not edit homebase.ino or the ESPHome config without asking first.
- State every assumption and mark what is untested. Verify RUI3 AT syntax against the current RAK documentation and the module's firmware version.
- Keep byte-level decoding in separate, host-testable code, following the repo's esphome/collar\_frame.h plus host test pattern.
- Follow the repo's conventions: arduino-cli builds, build flags for ids, README per component with a bring-up checklist and a troubleshooting table.
- Never include credentials, and never hand-wave the failure modes in section 11; add a check for each one the new hardware can hit.
