# Bench TODO — node 1 and the solar base

State as of 2026-11-07 morning. Base3 (`gateway/`) is flashed, on WiFi, publishing to
Home Assistant, and **hears all three collars on 433 MHz**. Node 1 is alive too — item 1
resolved without a soldering iron, and the 915 hop node → base is accepted. Items 2 and 3
are still open.

## 1. Node board (`80:53:e0:a0:be:30`) — **RESOLVED 7 Nov, no soldering needed**

The RAK3172 was never dead. The board was running a **stale binary**, `gateway/wifi_check`,
which never touches UART2, so the "24 probes over 40 s, no reply" was nobody probing the
module at all. **Read the boot banner before reaching for the multimeter:** opening the
port with DTR **low** resets the C3 and prints it, and the banner names the firmware on
the board — it said `WIFI_CHECK`, not `POC915 ver 2 role NODE node 1`.

Reflashed `-DROLE=2 -DNODE_ID=1` to `/tmp/poc_node` and uploaded with `--input-dir`. The
module answered on the **first** probe:

```
POC915 ver 2  role NODE  node 1
[mod] AT+VER=RUI_4.0.6_RAK3172-E
[mod] readback AT+P2P=915000000:7:0:0:8:14
[433] ver 12 want 12  frf 6C4000  mc1 76  mc2 94  sync 12  inviq 27  pa FF  opmode 85
[TX] done  seq 22  ok=22      ← every send acknowledged
```

Acceptance against base3 (`python3 tools/poc_watch.py 120`): 15 reports, `missed 0`, 915
rssi −21..−16 dBm, all three collars at the house, and base3's own RSSI for the **same
packet** inside ±3 dB (mean +1.2, spread −3..+3). In Home Assistant
`sensor.cat_base_3_node_1_node_3_packets` climbs every 8 s and
`binary_sensor.cat_base_3_node_1_node_3_in_range_4` is `on`.

**Console gotchas that made a working board look dead** — all three are fixed in
`tools/poc_watch.py`, and worth knowing before reading any board:

- base3 prints `[RX] node …` only when **verbose is on**, and key `v` toggles it. Watch
  it with no key and it prints nothing for minutes while publishing normally: a 120 s
  silent capture ran while the packets counter went 12 → 31 and `last_seen` advanced.
- The `[RX]` line is `cat <id> seq <collarseq> r<rssi>`, and base3 omits the `dBm`/`dB`
  suffixes. A regex pinned to `cat 2 rssi -90` matches nothing.
- Keys typed during `setup()` are **queued**: the RAK probe blocks ~41 s. Keys land after
  bring-up, and `[at] AT — no reply` lines in that window are the probe loop, not the
  typed command.

The pad checks below are the right order for the next board (node 2, node 3), and node 1
cleared every one of them **without a meter** — the module answered the first probe:

Check in this order (`poc915/README.md`, "the module answers nothing"):

1. **Pad 24 (VDD) = 3.3 V.** Pad 23 is GND and sits beside it. A slid joint shorts the
   rail, and the module answers nothing.
2. **Pad 22 (RST) HIGH.** Low holds the module in reset, and it answers nothing.
3. **Pad 21 (BOOT0) LOW.** Floating BOOT0 puts the STM32 in its UART bootloader, and
   the module answers nothing. This is the most common cause on these boards.
4. **Pads 1/2 not swapped.** Pad 1 is RXD (C3 TX, GPIO21), pad 2 is TXD (C3 RX,
   GPIO20). Swapped, the module answers nothing.
5. **100 µF across VDD and GND** (RA-02), and the RAK draws its peaks from the rail,
   not the LED strip's resistor.

If the power checks pass, use console key `b` (bridge) on the node: type raw AT
commands and see whether anything comes back. A reply there means the firmware's
`atWait` timing is wrong, not the wiring. `poc915/at_probe/` is the minimal AT-only
sketch for the same question, with no firmware state in the way.

Acceptance, base console:

```
[RX] node 1 seq N cat 0 seq M r-97
```

Acceptance: same `seq` and `cat` at both ends, RSSI within ±3 dB. `python3
tools/poc_watch.py` prints the same lines on the host — it sends `v` first, because the
base prints those lines only in verbose mode.

## 2. TP5000 battery and charge status — **firmware done, wiring not**

The node now reads its own cell and reports it, so the firmware half is finished:
`-DBAT_ADC=3 -DBAT_DIV=2 -DCHRG_PIN=<pin>` builds `battMv()`/`chgState()`, the report
carries ` b3325 q1`, base3 publishes `base3_n1_battery` (volts) and
`base3_n1_charging`, the dashboard has both tiles, and `node_check.py` warns below
3.10 V. Key `u` on the node console prints `[batt] mV charge ADC divider`, and the beat
line carries `batt`/`chg`.

What is left is the board, per the wiring plan:

- battery divider → **GPIO3 (A3)**, 100 kΩ/100 kΩ + 100 nF, **not on a strap**
  (GPIO2/8/9 are straps), `BAT_DIV 2`
- TP5000 `CHRG` (open-drain, pulled low while charging) → a GPIO with `INPUT_PULLUP`
  (`CHRG_PIN`)

Then verify: console `u` must print a voltage a multimeter agrees with, and the HA tile
must move when the panel is covered and uncovered. A LiFePO4 curve is flat, so the alarm
is on volts, not on a percentage.

## 3. Entity ids are cosmetically damaged

The first discovery pass ran with a name bug (every label aliased to the last string
parsed), and Home Assistant generated entity ids with `node_3` in them and numeric
suffixes. The names are right; the ids are not, and they cannot be fixed by removing
entities — the discovery record remembers its suggested ids. Removing the *config
entry* is the way, and `config/device_registry/remove_config_entry` returned
`unknown_error` on this install.

So: the ids in the dashboard are the ones in the registry, read through
`tools/cats_dashboard.py` by unique_id. A HA restart, or removing base3's devices
from the UI, is the clean-up. Do not hand-type these ids.

## 4. Not verified yet

- 915 MHz hop node → base: **accepted 7 Nov** — 15 reports in 120 s, `missed 0`, 915
  rssi −21..−16 dBm, all three collars at the house, base-vs-node RSSI inside ±3 dB.
- Node rows on the dashboard: node 1 is live (`packets` climbing every 8 s, `in_range`
  `on`). Node 2 and 3 rows exist, nothing feeds them.
- 30-minute acceptance: not run. 120 s passed; `python3 tools/poc_watch.py 1800` is the
  longer form.
- Node 2 and 3 rows: created, nothing feeds them. If a node is never built, delete the
  rows from the dashboard and shrink `NUM_NODES` in `gateway/state_json.h`.

## 5. Field check, and getting code onto a node

- **Check a node in a tree from the ground:** `python3 tools/node_check.py` (add
  `--watch 120`). It reads `base3_n1_in_range` / `last_seen` / `packets` / `missed` by
  unique_id and prints per-node verdicts. Needs WiFi coverage at the tree — measure it.
- Firmware without a cable, and why BLE is the transport: see
  `docs/plans/remote-nodes/fota-and-field-check.md`. The prerequisite is the missing half
  of the 915 link — a base→node ack, so the node can report `last_ack_age` and answer
  "the house has heard me" rather than "I am sending".
