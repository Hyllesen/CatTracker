# Bench TODO — node 1 and the solar base

State as of 2026-11-06 evening. Base3 (`gateway/`) is flashed, on WiFi, publishing to
Home Assistant, and **hears all three collars on 433 MHz**. The node board is the only
thing missing, and every item below is a physical check, so it is written for hands,
not for me.

## 1. Node board (`80:53:e0:a0:be:30`, `/dev/cu.usbmodem211301`)

Flashed as `poc915` with `-DROLE=2 -DNODE_ID=1`. The RA-02 side is healthy: `ver 12,
frf 6C4000, mc1 76, mc2 94, sync 12, inviq 27, pa FF, opmode 85`, and it hears collars.
**The RAK3172 never answers AT** — 24 probes over 40 s, no reply, so the node sends
nothing and `sensor.cat_base_3_node_1_node_3_packets` stays at 0.

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
`atWait` timing is wrong, not the wiring.

Then the acceptance test, base console:

```
[RX] node 1 seq N cat 0 r-97
```

Acceptance: same `seq` and `cat` at both ends, RSSI within ±3 dB. `python3
tools/poc_watch.py` prints the same lines on the host.

## 2. TP5000 battery and charge status

`BAT_ADC` and `CHRG_PIN` are compiled as `-1`, so `sensor.cat_base_3_battery` is
`unknown` and `binary_sensor.cat_base_3_charging` is `off` forever. The board has a
TP5000 fitted. Per the wiring plan:

- battery voltage divider → ADC pin (`BAT_ADC`), with `BAT_DIV` matching the divider
  ratio (2 for the 100 kΩ/100 kΩ pair)
- TP5000 `CHRG` (open-drain, pulled low while charging) → a GPIO with `INPUT_PULLUP`
  (`CHRG_PIN`)

Rebuild and flash with `-DBAT_ADC=<pin> -DBAT_DIV=2 -DCHRG_PIN=<pin>`, then console `v`
must print a battery voltage that matches what a multimeter reads on the cell.

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

- 915 MHz hop node → base: blocked on item 1.
- Node rows on the dashboard: created (node 1 tiles exist), showing 0.
- 30-minute acceptance: 1 report/8 s, 0 missed seq. `base3` packets is 35 and climbing
  on 433 alone.
- Node 2 and 3 rows: created, nothing feeds them. If a node is never built, delete the
  rows from the dashboard and shrink `NUM_NODES` in `gateway/state_json.h`.
