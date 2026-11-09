# Field maintenance — getting code onto a node, and telling that it is connected

The node lives 3–5 m up a tree, on solar + 18650, and the only cable in it is the one
you put there at build time. Two questions:

1. How do the firmware updates get up there without pulling the board down?
2. How do you know, standing at the base of the tree, that the node is talking to base3?

Answer to 2 first, because it is nearly free, and because it shapes the answer to 1.

## 1. Verification — the house already knows

Every node reports on a per-ping basis, and base3 records what it hears. Home Assistant
therefore has a **per-node** connection record, published by discovery:

| unique_id | entity | what it says |
|-----------|--------|--------------|
| `base3_n1_in_range` | `binary_sensor` | the house heard node 1 within the window |
| `base3_n1_last_seen` | `sensor` (timestamp) | the last report's arrival time |
| `base3_n1_packets` | `sensor` | reports received, counting up every 8 s |
| `base3_n1_missed` | `sensor` | gaps in the node's own seq |

So the check is: **run `python3 tools/node_check.py` from a laptop on the house WiFi,
standing at the tree.** Within one report cycle it prints `reporting - base heard it N s
ago`, or `not transmitting`. It exits nonzero when no node is reporting, so it can be
the pass/fail gate of a checklist. It reads entity ids out of the registry by unique_id
(never typed — see `AGENTS.md`), and it reads states over REST, because this install
answers **no** state WebSocket command (`get_state`, `state/get`, `state/get_all` all
return `unknown_command`).

```
node 1: in_range=on last_seen=2026-11-06T23:23:10+00:00 packets=78 missed=0
  => node 1: **reporting** - base heard it 12 s ago
node 2: in_range=off last_seen=unknown packets=0 missed=0
  => node 2: **not transmitting** - packets 0 unchanged since last poll
```

Three limits, all worth knowing before relying on it:

- **It needs WiFi coverage at the tree.** The node's 915 link reaches the house; a
  laptop's 2.4 GHz link to the house is a different path, and foliage eats 2.4 GHz.
  Measure it: walk to the tree with the laptop, note the WiFi RSSI, run the tool. If it
  does not reach, the phone-on-HA option is out and BLE is the instrument.
- **The node cannot currently tell you whether the house heard it.** The 915 link is
  one-way: the node sends, the base receives. The node knows its own TX succeeded
  (`+EVT:TXP2P DONE`), not that the base got the frame. So a node-side status read tells
  you "I am sending", not "I am connected". **That distinction is the whole point of the
  check**, so the fix is the missing half of the link:

  **A base→node ack.** The base sends a 4-byte `ACK <node> <seq>` on the same 915 link
  with `AT+PSEND`, inside the node's 250 ms slot. The node arms RX after each TX, stores
  `last_ack_age`, and reports it. That gives the node a true `connected` bit — the only
  bit that answers "is the whole path alive", and the one bit you can read at the tree
  with no house involvement. `final-remote-plan.md` §17 put downlink out of scope for
  v1 ("nothing needs to command a node yet"); field verification is the thing that needs
  it, and the ack is the smallest possible downlink.

- **Node battery is not published.** `BAT_ADC`/`CHRG_PIN` are compiled as `-1`
  (`TODO-bench.md` item 2), so a node that goes silent gives no reason. Wire the TP5000
  before trusting a node in a tree for a week.

**BLE status service — the tree-side instrument.** The node advertises `CATNODE<n>` and
publishes one readable 20-byte characteristic: node id, battery mV, charge state, 433
packets, 915 sends ok/fail, queue drops, **last ack age**. The phone app already has the
plumbing (scan unfiltered, connect, `requestMTU`, read once, subscribe —
`CatTrackApp/src/ble.ts`), and the roster decoder is the pattern to copy. With the ack in
place this is a complete field instrument: collar → node → base, one screen, at the tree.

**Soft-AP is the alternative** (node hosts `CATNODE1`, phone connects, node serves an HTML
status page). It works, and costs more: an AP beaconing every 100 ms is a permanent power
cost on a 2.6 Wh/day budget, and the phone has to leave the house network. BLE is cheaper
and the app is already a BLE client.

## 2. Firmware — what each transport can actually do

Facts from this repo, not from the datasheets:

- The sketch is 340,010 bytes (the `poc915` node build).
- The **default** ESP32 partition scheme already has `app0` **and** `app1`, 1.3 MB each,
  plus `otadata` — so two-image OTA is available **without touching the partition
  scheme**, and a 340 KB image fits with room.
- The installed core (`esp32 3.3.12`) ships `ArduinoOTA` (with `BasicOTA`), `HTTPUpdate`,
  `HTTPUpdateServer`, and `tools/espota.py` (the host-side updater). No third-party
  library needed.
- `esp_ota_ops` gives the safety net: write to the **other** slot, verify, switch, and
  `esp_ota_mark_app_valid_cancel_rollback()` at boot. A bad image reboots back to the
  good one. This is the requirement that makes up-the-tree updates acceptable: **never
  overwrite the image you are running.**
- Today's node role has **no WiFi and no BLE** (`poc915` node role): a transport has to
  be added, and every addition has a power cost on the solar budget.

| Transport | Reaches a tree? | Transfer of 340 KB | Node power | Needs | Verdict |
|-----------|----------------|--------------------|-----------|-------|---------|
| **BLE** | Yes, ~5–15 m through foliage — you stand at the trunk | 5–20 KB/s → **20–70 s** + overhead (arithmetic, not measured) | Idle peripheral is cheap; the transfer is a few seconds of activity | Node GATT OTA service + app chunking client | **Primary.** Phone is already in your pocket, and the app already speaks BLE |
| **WiFi (ArduinoOTA)** | Only if the tree is in 2.4 GHz range of the house. Node 1 "just outside the house" is; deep jungle is not | ~1 min, dominated by flash writes (arithmetic) | **Expensive if permanent**: a station tracking beacons is tens of mA. A 1 h/day maintenance window is ~40 mAh/day against a ~790 mAh/day budget; 24 h WiFi is not affordable | Node runs WiFi + `ArduinoOTA`, and a way to **trigger** maintenance mode with no cable | **Secondary**, for house-range nodes. Updates from a laptop on the LAN, no climb |
| **915 MHz** | Yes — it is the node's own link | 255-byte payload ≈ 210 ms airtime at SF7/BW125 → ~1.2 KB/s raw → **10–20 min** with per-chunk ack and retransmit (arithmetic) | The node must be in continuous 915 RX for the window; its **433 RX keeps working**, so collars keep being heard, and the 4-deep report queue will overflow and count drops | A bootloader app that is always resident, a chunk protocol with ack/CRC, and the base flipped to TX | **Not for code.** Right answer for **config**: node id, report period, SF, sleep window — one 9-byte command frame |
| **USB** | Only by pulling the board down | n/a | n/a | cable | Recovery path, not maintenance |

**Recommendation: BLE for firmware, WiFi for house-range nodes, 915 for configuration,
USB for the board that got bricked.** Do not build the 915 code path: it is a week of
protocol work for a 15-minute transfer that blinds the node's reporting and needs an
always-resident bootloader app — and the BLE path needs the same protocol work with a
phone, 20× faster, and no change to the base.

## 3. Order of work

1. **Base→node ack + node `last_ack_age`** (smallest downlink). Acceptance: base console
   prints the ack, node console prints the ack age falling to 0–8 s, and
   `[RX] node 1 seq N` at the base lines up with it.
2. **Node BLE status service** (`CATNODE<n>`, one readable 20-byte characteristic, the
   roster's MTU rule applies: keep it inside the default ATT payload). Acceptance:
   `python3 tools/ble_roster.py`-style host reader, extended to print the node status,
   and the app screen. Acceptance must prove the ack bit, not just the counters.
3. **BLE OTA**: chunked write characteristic, `esp_ota_begin`/`esp_ota_write`/
   `esp_ota_end`/`esp_ota_set_boot_partition`, rollback armed, whole-image CRC32
   verified before the switch. Acceptance: push a build that changes the banner,
   confirm the banner changes, confirm the old image is still in the other slot, and
   confirm a deliberately corrupt image rolls back. **The rollback test is the
   acceptance test** — it is the only thing standing between a tree and a brick.
4. **WiFi maintenance mode** for house-range nodes: ArduinoOTA gated on a trigger that
   needs no cable — the 915 ack/command channel, a reed switch on the enclosure, or
   "maintenance for 60 s after power-up". Acceptance: `espota.py` from a laptop, node
   comes back with the new banner, and the node's average current measured with
   maintenance mode off is unchanged.

Each step needs the node in range, so 1 and 2 can be done on the bench with node 1, and
step 3 is what the tree test is for.

## 4. Rules this creates

- **Never flash the slot you are running from.** OTA writes the other slot, verifies the
  whole image, switches, and rolls back automatically if the new image does not boot.
- **A node that cannot be commanded cannot be maintained.** Anything up a tree needs the
  ack/command half of the 915 link, so the downlink is a prerequisite, not a feature.
- **Every throughput number above is arithmetic, not a measurement.** Replace the BLE
  transfer time and the WiFi maintenance-mode current with measured values in this file
  before quoting them.
- **Console silence is not connection failure.** base3 prints `[RX] node …` only in
  verbose mode (key `v`), and the node prints nothing with no cable. Read
  `node_check.py` / the HA row, not the console, to decide whether a link is alive.

## 5. The BLE path, designed (nodes out of WiFi range)

Because the deep-jungle nodes are out of 2.4 GHz range, BLE is the transport. ESPHome's
native OTA is WiFi-only, so the ESPHome route buys OTA for house-range nodes and nothing
for the ones that need it.

**Node side — one service, four characteristics, BLE off until woken.**

| Characteristic | Direction | Contents |
|----------------|-----------|----------|
| `STATUS` | read | 20 bytes: node id, fw version, battery mV, charge, 433 packets, sends ok/fail, queue drops, **last ack age** |
| `OTA_CTRL` | write-with-response | `B <len> <crc32>` begin · `E` end+verify · `G` go · `Q` abort |
| `OTA_DATA` | write-without-response | 244-byte chunks at MTU 247 |
| `OTA_EVT` | notify | `ACK <offset>` · `ERR <code>` · `DONE` |

Advertising name `CATNODE-<node_id>`, so a scan finds the node the way `ble_roster.py`
finds `CatTracker`, and the app's unfiltered scan shows what is nearby when nothing
matches.

Node implementation is `esp_ota_ops`, in this order and no other:

1. `esp_ota_begin(other slot, OTA_SIZE_UNKNOWN)` — **never the running slot**.
2. Write chunks in sequence, checking the seq the client puts on each one. A gap is an
   `ERR` and an abort, not a silent skip: a skipped chunk lands as a plausible image
   that dies at boot.
3. CRC32 the whole image as it arrives. `esp_ota_end()` + **whole-image CRC32 match**
   before `esp_ota_set_boot_partition()` — the switch is the last act, so a failed
   transfer leaves the node running the image it had.
4. Reboot. The new image boots with rollback armed. It marks itself valid
   (`esp_ota_mark_app_valid_cancel_rollback()`) **after it has heard a base ack**, so
   "healthy" means the whole path works, not "the board powered up".
5. A bad image reboots back to the old one on its own. **Deliberately flashing a
   corrupt image and watching it roll back is the acceptance test** — it is the only
   thing between a tree and a brick.

**BLE is off by default and opened by a magnet on the enclosure** (a reed switch on a
GPIO with internal pull-up, waking the node; BLE runs for 10 min, auto-closes with no
connection). That is the version with no permanent power cost. The alternatives are a
button, which needs a hole in the enclosure, and BLE always on, which is a permanent
advertising cost on a 2.6 Wh/day budget — unmeasured, and the sort of number that has to
be measured before it is accepted.

**Client side — a laptop, not the app.** `tools/ble_ota.py <build.bin>` in the same shape
as `tools/ble_roster.py` (bleak, scan, connect, request MTU, read, stream): scan for
`CATNODE-<id>`, print `STATUS`, send `B`, stream chunks with seq, watch `OTA_EVT` acks,
verify, send `G`, reconnect after reboot and read `STATUS` to confirm the new version. A
phone can display status with the existing `ble.ts` plumbing; picking a 340 KB `.bin` on
Android is the part that costs more than the protocol, so the **update runs from the
laptop that built it**, and the app carries `STATUS`.

Acceptance for the path, on the bench with node 1 before anything climbs a tree:

```
client: 340 KB pushed, measured KB/s written into this file
node:   console banner changes, STATUS reports the new version, base3 keeps hearing reports
rollback: a deliberately corrupt image rolls back to the old one and the node keeps
          reporting
power:   average node current measured with BLE off, unchanged from before
```

