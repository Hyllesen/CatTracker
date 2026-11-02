# Remote nodes — a 433 listener mesh over a 915 backhaul

Status: plan, not yet built. Written 2026-11-03.

The goal: fixed **nodes** out in the jungle that hear the collars at 433 MHz and
report what they heard to a **base station** on the roof at 915 MHz, so the house
can say *where* a cat probably is, not just *whether* it has been heard.

---

## 1. The constraint that shapes everything

The collar transmits **433 MHz** (SX1278/RA-02, SF9, BW125, CR 4/7, sync `0x12`,
preamble 8, payload CRC on, explicit header). The RAK3172 is a **single-band**
module: RAK3172(H) covers 525–960 MHz, RAK3172(L) covers 150–525 MHz. Same
firmware, different RF matching. A 915 MHz module cannot be tuned down to 433.

> **Every node needs two radios: an RA-02 at 433 to hear the collar, and a
> RAK3172 at 915 to talk back.**

This is not a complication, it is the design. The node listens on 433 *while* it
transmits on 915, so it never goes deaf, and **no collar changes are needed at
all**. v1 touches zero collar firmware: the existing five-way frame contract is
untouched. What gets added is a new contract (the backhaul frame) and a new
consumer (the locator).

The handheld keeps its job. It is still the only thing that finds a cat in the
last 20 metres; the nodes say which part of the property to walk to.

---

## 2. Topology

```
 collar 0/1/2 ──433 MHz LoRa──┬──> handheld (ESP32-C3 + RA-02) ──BLE──> phone   [hunting]
                              ├──> base1 (ESP8266 + RA-02) ──WiFi/MQTT──> m710q  [house]
                              │
                              └──> node1..node4 (ESP32-C3 + RA-02 + RAK3172)
                                        │ 433 RX: id, seq, rssi, snr
                                        └──915 MHz LoRa P2P──> base2 (ESP32-C3 + RAK3172)
                                                                     └──WiFi/MQTT──> m710q
                                                                                       │
                                                                       locator (Python) ┘
                                                                           └──> HA: zone, fix, heat map
```

Names, and why they are these names:

| Word | Meaning |
|------|---------|
| **Node** | A fixed, solar-powered 433 listener in the jungle. `node1`, `node2`, … Baked in at flash time with `-DNODE_ID=`. |
| **Base station** | A fixed, powered receiver that reports to the house. `base1` is the existing ESP8266 on 433. `base2` is the roof collector on 915. |
| **Collector** | The 915 half of `base2`. It is a base station; "collector" only names its radio role. |
| **Locator** | The Python service on `m710q` that turns per-node RSSI into a zone and a fix. |
| **Zone** | The node that hears a cat best. A coarse answer that is nearly unbreakable. |
| **Fix** | A position estimate with a radius. Weaker than a zone, and labelled as such. |

`base2` is free: there are no retained `cattracker/base2/#` topics on the broker
and the module has never been online. **But `homebase/esphome/cat-base2.yaml` is
a live build target in the ESPHome dashboard that would collide with it.** See
§12, pre-flight.

---

## 3. Hardware

Per node:

| Part | Notes |
|------|-------|
| ESP32-C3 | USB CDC (`CDCOnBoot=cdc`). No WiFi needed, no BLE needed, so flash is not the problem it was on the handheld (99% with BLE + WiFi + MQTT). |
| RA-02 (SX1278) | 433 RX. Same module, same air settings as the collar. **Antenna is a 433 antenna.** |
| RAK3172(H) | 915 TX, driven by RUI3 AT commands over UART1. **Antenna is a 915 antenna.** |
| 18650 + CN3065/TP4056 | Solar charge controller, 100 µF at the radio, 100 nF at the ADC divider. |
| 3–5 W panel, 6 V | Under canopy, assume 2–3 equivalent sun hours, not 5. |
| IP65 box, 2–3 m pole | Drip loop on the panel cable, desiccant, conformal coat the boards. Wet season is the real test. |

Per base station (`base2`):

| Part | Notes |
|------|-------|
| ESP32-C3 | UART1 for the RAK3172, WiFi for MQTT. No BLE, so the handheld's flash wall does not apply. |
| RAK3172(H) | 915 RX. RX current 5.2 mA — it can listen continuously. |
| 5 V supply | Roof box, mains or PoE-split. No solar maths needed. |

Two inventory checks before buying anything:

- **Antennas are not interchangeable.** 433 and 915 antennas look alike. Label
  every one of them at the pigtail. A 915 antenna on the 433 receiver is a node
  that hears nothing and looks exactly like a dead radio.
- **RAK3172 variant.** The (H) part covers 915; the (L) part is deaf there.
  Confirm with `AT+VER` and the label. Pin 27 is internally pulled **up** on the
  high-band variant and **down** on the low-band one.

### Pin map (starting point — verify against your board's silkscreen)

ESP32-C3 traps: **GPIO2, GPIO8, GPIO9 are boot straps**, and **GPIO18/19 are
USB D-/D+** (leave them alone if you want USB CDC to keep working).

| Signal | Pin | Why |
|--------|-----|-----|
| SX1278 SCK / MISO / MOSI | GPIO4 / 5 / 6 | FSPI defaults, matrix-routed anyway |
| SX1278 NSS | GPIO7 | |
| SX1278 DIO0 | GPIO3 | RX-done interrupt, must be interrupt-capable |
| SX1278 RST | GPIO20 | free once USB CDC is on 18/19; not a strap |
| RAK3172 UART1 TX / RX | GPIO21 / GPIO0 | matrix-routed UART1, 115200 8N1 |
| Battery divider | GPIO1 | ADC1_CH1, 100k/100k + 100 nF |

RadioLib takes explicit pins, so the SPI map is a `begin()` argument, not a
hardware constraint. Cross the numbers against the silkscreen before soldering,
the way `homebase/README.md` does for the NodeMCU.

**The battery divider must not land on a boot strap.** GPIO2, GPIO8 and GPIO9 are
sampled at reset; a divider pulling one of them the wrong way is a node that will
not boot once the cell is low — and it looks like a dead board, not a wiring
fault. GPIO1 is ADC1_CH1 and carries nothing else.

---

## 4. The backhaul frame (v1)

28 bytes worst case, header-only codec, one canonical copy (see §11).

```c
0  0xB1   magic / version. Never 0xC1 — that is the collar's.
1  id     node id 1..15, baked at flash with -DNODE_ID=
2  type   0x01 measurement report | 0x02 heartbeat
3  batt   battery percent, 0xFF = unknown
4  flags  bit0 433 FIFO overflow since last report, bit1 915 TX was retried
5  n      entry count, 0..3
   then n entries, 7 bytes each, ascending collar id:
     0  collar id
     1  seq
     2  rssi      int8, dBm
     3  snr       int8, dB x 2
     4  age_ms/4  uint8, 0..1020 ms since the packet was heard
     5  heard     uint8, low byte of packets heard from this collar since boot
     6  missed    uint8, low byte of seq gaps since boot
last  noise     int8, dBm. SX127x idle RSSI. A placement diagnostic.
```

**`seq` is the whole point of this frame.** RSSI is only comparable across nodes
if it describes the *same transmission*. Without `seq` the locator compares
packets sent at different times, from different antenna orientations, and
produces a confident-looking position that is pure noise. The collar already
carries `seq`; the node must forward it unchanged.

`age_ms/4` exists so the base station can timestamp correctly even when a node
queues a report during its transmit slot.

No CRC of our own: LoRa's payload CRC covers it, exactly as on the collar. The
magic byte rejects stray frames.

---

## 5. Node behaviour

### Listen

RadioLib, configured to match the collar exactly: 433.0 MHz, SF9, BW125,
CR 4/7, sync `0x12`, preamble 8, CRC on, explicit header, continuous RX.

On each packet: reject anything that is not a 6-byte `0xC1` frame, then record
per collar id `{seq, rssi, snr, heard_at}`, bump `heard`, and add `seq` gaps to
`missed`. Read RSSI first, then SNR, immediately — the same ordering rule the
handheld uses (`handheld_ble/handheld_ble.ino`), because the registers are
volatile across reads.

### Report, with no clock sync at all

Every node hears the same ping, so the ping is the clock:

```
t0      = arrival of the first packet after a >1.5 s silence
tx_at   = t0 + 400 ms + (node_id - 1) * 400 ms
```

All three collars land within ~300 ms of each other (their phases are 137 ms
apart), so a single 400 ms wait collects the burst; each node then transmits in
its own 400 ms slot. Four nodes fit in 1.6 s of an 8 s cycle, and 16 would fit
in 6.4 s. No time distribution, no beacons, no drift problem.

A node that heard nothing this cycle stays silent, and every 8th cycle
(~64 s) sends a `type 0x02` heartbeat with battery and noise floor instead. That
is what makes "this node is dead" distinguishable from "no cat nearby".

### Power

| Item | Awake v1 | With C3 light sleep on DIO0 |
|------|----------|------------------------------|
| SX1278 RX continuous | ~12 mA | ~12 mA |
| ESP32-C3 | ~22 mA | ~0.5 mA |
| RAK3172 TX, 14 dBm, 0.2 s / 8 s | ~1 mA | ~1 mA |
| **Average** | **~35 mA → ~840 mAh/day** | **~14 mA → ~336 mAh/day** |

A 3 W panel under canopy yields roughly 900–1400 mAh/day, so v1 works with thin
margin in the rainy season; a 3500 mAh cell gives ~4 days autonomy at v1 and
~10 days at v2.

**Build v1 awake, measure the real draw with a USB meter, and only add light
sleep if the numbers demand it** (`esp_sleep_enable_gpio_wakeup()` with DIO0 as
a high-level wake source, then `readData()`).

**Do not plan on windowed listening.** The ATtiny collar pings on a watchdog
sleep whose period is 7–10 s for a nominal 8 s, so a node cannot phase-lock to
it; windows would miss most of its packets. Continuous RX is the only scheme
that works for every collar in the fleet.

### Serial

Nodes are solar-powered, so the AGENTS.md rule applies without exception: **no
ungated `Serial.print`.** Gate every print on `Serial.isPlugged()` (HWCDC on the
C3) and set `Serial.setTxTimeoutMs(0)` in `setup()`.

---

## 6. Base station behaviour

```
AT+NWM=0
AT+P2P=915000000:9:125:2:8:14     # freq:sf:bw:cr (2 = 4/7):preamble:power
AT+PRECV=65535                    # continuous RX
+EVT:RXP2P:<rssi>:<snr>:<hex>     # RUI3 hands us the 915 RSSI and SNR for free
```

Three traps, all of which are silent:

1. **RUI3 leaves RX mode after every received packet.** `AT+PRECV=65535` must be
   re-armed inside the receive handler (and on the receive-timeout event). Forget
   it and the base station hears exactly one frame and then looks like a dead
   network.
2. **`AT+P2P` parameters must match on both ends.** Frequency, SF, BW, CR,
   preamble. A mismatch is a base station that receives nothing, indistinguishable
   from a dead RAK3172.
3. **The 915 link's SF/BW/CR are independent of the collar's.** They are chosen
   for the backhaul, not to match the collar. SF9 is the default here; at 50–70 m
   in a hectare, SF7–SF8 will also work and cut airtime 2–4×. Pick from the
   phase-1 range measurement, not from taste.

Then: keep a state table (4 nodes × 3 collars), publish a retained summary every
~5 s, publish each frame on a non-retained topic for the locator, and publish HA
discovery records on boot and on retry. Availability via LWT (`offline`) so a
dead base station is visible as *Blind* and not as "no cats".

### MQTT

```
cattracker/base2/availability   retained  "online" / LWT "offline"
cattracker/base2/state          retained  summary, throttled to ~5 s
cattracker/base2/pkt            not retained, one JSON per received frame
homeassistant/sensor/base2_n1_battery/config ...
```

`state` keeps the shape base1 already uses, so the same HA habits apply:

```json
{"t":372597143,"ts":1793647723,"pk":97822,"unk":0,
 "n1":{"batt":87,"age":3,"noise":-104,"pk":812,"miss":41,
       "c0":{"rssi":-886,"snr":8,"age":3,"pk":210,"miss":12}},
 "n2":{...}}
```

`pkt` is the locator's input, one row per entry, no smoothing applied anywhere:

```json
{"ts":1793647723,"node":1,"type":1,"batt":87,"noise":-104,
 "e":[{"id":0,"seq":201,"rssi":-88,"snr":8,"age_ms":312,"heard":210,"miss":12}]}
```

Scaling follows the house convention, and the two topics deliberately differ:

- the **frame** carries RSSI as `int8` dBm and SNR as `int8` dB × 2 — smallest
  possible airtime;
- **`state`** publishes RSSI × 10 and SNR × 2, the same units base1 already uses,
  so one HA `value_template` shape works for both base stations;
- **`pkt`** publishes the frame's own values (dBm, dB) untouched, because the
  locator does the maths and must not inherit a scaling it did not ask for.

The one rule: a unit is converted exactly once, at the boundary, and
`tools/monitor.py`'s parser is the reference for what the numbers mean.

---

## 7. The locator

A Python service on `m710q`. Not HA templates: weighted centroid in Jinja is
painful and the likelihood grid is out of reach.

Inputs: `cattracker/base2/pkt` (per-packet, per-node), `tools/outposts.json`
(where the nodes are and how each one lies), and `cats.json` for names.

```json
{
  "path_loss": {"p0_dbm": -42, "n": 3.4, "d0_m": 1.0, "sigma_db": 8},
  "nodes": [
    {"id": 1, "name": "node1", "lat": 0.0, "lon": 0.0,
     "height_m": 2.5, "rssi_offset_db": -1.2},
    {"id": 2, "name": "node2", "lat": 0.0, "lon": 0.0,
     "height_m": 2.5, "rssi_offset_db": 0.8}
  ],
  "area": [[lat, lon], [lat, lon], [lat, lon], [lat, lon]]
}
```

Three tiers, shipped in this order, each published separately so it is always
obvious how much to trust the answer:

1. **Zone.** Per (cat, node) keep the median RSSI of the last ~10 packets.
   The zone is the argmax, with the margin over second place published as
   confidence. Single-packet RSSI in jungle multipath is a coin flip; a median
   is not. This tier alone answers the real question.
2. **Coarse fix.** `d = d0 * 10^((P0 - rssi) / (10n))` per node, then a weighted
   centroid, with a radius from the residual spread.
3. **Likelihood grid.** 5 m cells over the `area` polygon (<1 ha ≈ 400 cells, a
   numpy one-liner per cat), score each cell against every node's measured RSSI,
   take the peak and the contour that holds 68% of the likelihood. Optionally
   publish the grid as a PNG on a retained MQTT image topic.

Output, retained per cat:

```json
{"ts":1793647723,"zone":"node2","conf":0.72,
 "lat":0.0,"lon":0.0,"r":25,"nodes":3,"tier":2}
```

Plus HA discovery for zone, lat, lon, radius and confidence, so a map card and a
"Chichi is in the north clearing" notification are both possible.

### Calibration — this matters more than the algorithm

At 50 m spacing the RSSI differences between neighbouring nodes are 3–10 dB,
which is the same order as the per-module offset. Uncalibrated, every fix is
pulled toward whichever SX1278 reads hot.

1. **Co-location.** All four nodes on one table for 20 minutes. Same packets,
   four modules. The per-node offset is the mean difference from the cross-node
   median. Free, and it removes a systematic bias.
2. **Walk test.** The `transmitter/` sketch already fakes three collars. Carry it
   to 8–12 surveyed waypoints for 2 minutes each and fit `p0` and `n`
   (`walk.csv` layout, extend `tools/analyze_walk.py` or add
   `tools/fit_path_loss.py`).
3. **Validation.** Leave a collar at three known spots for 10 minutes and compare
   predicted against actual.

### Honest expectations

433 MHz through wet foliage gives hundreds of metres of link, but RSSI ranging
with one antenna on a moving cat gives **tens of metres** — and body absorption
(5–10 dB, directional, changes as the cat turns) is not removable by any
algorithm. On one hectare with four nodes, expect *"which third of the property"*
reliably and a 20–50 m radius the rest of the time. The heat map exists to show
that uncertainty rather than hide it.

`base1` can vote as a fifth, very coarse receiver (it is at the house, and it
publishes smoothed RSSI but no `seq`), so use it in tier 1 only, with a wide
sigma.

---

## 8. Failure modes, all of which look like something else

| Symptom | Actually |
|---------|----------|
| Node hears nothing | 915 antenna on the 433 module, or vice versa |
| Node hears nothing | Air settings mismatch: SF, BW, sync `0x12`, CRC, explicit header |
| Node hears nothing | DIO0 on a boot-strap pin (GPIO2/8/9 on the C3) |
| Base station hears one frame then nothing | `AT+PRECV` not re-armed after reception |
| Base station deaf at 915 | RAK3172**(L)** module, not (H) |
| Base station deaf at 915 | `AT+P2P` differs from the node's by one field |
| Frames arrive but the locator ignores them | `seq` not forwarded, so nothing fuses |
| Fixes lean toward one node | Per-module RSSI offsets not calibrated |
| Fix jumps every cycle | Single-packet RSSI instead of a median |
| Fix is confidently wrong | Node positions are guesses, not surveyed |
| Node alive but silent | Solar/shade, not radio: check battery and panel voltage first |
| HA state flaps or a dead base looks alive | `base2` name reused by `cat-base2.yaml` |
| Everything works on the bench, nothing at 200 m | 915 SF too low for the wet-canopy path |

---

## 9. Phases

| Phase | Deliverable | Acceptance |
|-------|-------------|------------|
| 0 | Node with only the 433 half; prints `id seq rssi snr` over USB | Hears all 3 collars; RSSI within ±3 dB of the handheld at the same spot; `seq` matches the handheld's |
| 1 | RAK3172 pair: `AT+P2P`, `AT+PSEND`, `AT+PRECV` re-arm | Hex round-trip both ways; `+EVT:RXP2P` carries RSSI/SNR; measured range in the actual jungle |
| 2 | Node firmware v1: both radios, slotting, heartbeat, battery | ≥95% of frames received at 200 m; frame codec passes its host test |
| 3 | `base2` + MQTT + HA discovery | `mosquitto_sub -t 'cattracker/#'` shows per-node state; unplug a node and its age climbs without hiding the cats |
| 4 | Locator + co-location + walk fit | Synthetic test recovers known positions; zone correct in most cycles of a real walk |
| 5 | Placement, solar, survey | Spacing ≈ measured range ÷ 2, 2–3 m height, surveyed coordinates in `outposts.json` |

Phase 0 is worth doing on its own: if a node's RSSI does not agree with the
handheld's at the same spot, nothing downstream can be trusted.

---

## 10. Tests

- `outpost/test_backhaul_frame.cpp` — encode/decode round-trip, truncation,
  unknown magic, and a fixed hex vector, mirroring
  `homebase/esphome/test_collar_frame.cpp`.
- `tools/test_locator.py` — synthetic RSSI generated from known positions must be
  recovered within tolerance; injected 10 dB fades must not move the zone.
- `tools/test_monitor.py` — unchanged, but run it: it is the canary for anyone
  touching shared scaling.
- Bench: `transmitter/` fakes three collars so a node and the base station can be
  exercised indoors with no cats present.

---

## 11. Files

```
outpost/outpost.ino              node firmware
outpost/README.md                wiring, AT setup, power budget, bring-up checklist
collector/collector.ino          base2 firmware
collector/README.md              RAK3172 traps, MQTT schema, discovery
libraries/CatTrackerBackhaul/    one canonical backhaul_frame.h + host test
tools/locator.py                 the service
tools/test_locator.py            synthetic-position test
tools/fit_path_loss.py           walk-test fitter
tools/outposts.json              node positions, heights, offsets, path-loss model
docs/adr/0002-two-radios-per-node.md
```

The frame codec goes in `libraries/` on purpose. The collar frame already lives
in five copies and `AGENTS.md` has a bullet about the cost of that; the backhaul
frame gets one copy, included by both sketches (`arduino-cli --libraries
./libraries`) and by the host test.

### Contracts to update

- `AGENTS.md`: the backhaul frame must match in `outpost/`, `collector/` and
  `tools/locator.py`; `AT+PRECV` must be re-armed; a 433 antenna and a 915
  antenna are not interchangeable; per-node RSSI offsets must be calibrated
  before any fix is believed; nodes use `-DNODE_ID=` and are recorded in the same
  id → serial → location table as the collars.
- `GLOSSARY.md`: **Node**, **Base station** (already there), **Zone**, **Fix**.
  Remove "node" from the *Avoid* list under Base station, since it now names a
  different device.
- `README.md`: a short "Remote nodes" section pointing here, and the note that
  the phone app still does not see them — it talks BLE to the handheld, and the
  handheld has no room for a third radio.

---

## 12. Pre-flight

- [ ] `cat-base2.yaml` in the ESPHome dashboard must be retired or renamed so it
      can never publish as `base2`. The name collision is silent and makes a dead
      base station look alive.
- [ ] Regenerate `~/.ha_token` (currently returns 401) and confirm Home Assistant
      actually has the MQTT integration configured — the README says it does not,
      yet `base1` discovery records are being published, so one of the two is
      stale.
- [ ] Confirm the RAK3172 modules are (H) with `AT+VER`, and label the antennas.
- [ ] Record `AT+SN` for each RAK3172 into the node id table.
- [ ] Verify the 902–928 MHz allocation and power limits that apply in the
      Philippines before raising `AT+PTP` above 14 dBm. The link does not need it
      at one hectare.

---

## 13. Deliberately out of scope for v1

- **Windowed listening.** Cannot be phase-locked to an ATtiny collar (§5).
- **WiFi-direct transport.** A node inside WiFi range of the house could publish
  straight to MQTT and skip `base2`. Kept as an escape hatch, not a v1 code path:
  it doubles the ways a node can be deaf.
- **Downlink.** `AT+PSEND` works both ways, and the frame has a `type` byte for
  it, but nothing needs to command a node yet.
- **The phone app.** It talks BLE to the handheld. Locations live in HA.
- **Ranging.** Real distance measurement would need a collar change (two power
  levels, or time-of-flight). RSSI only, for now.
