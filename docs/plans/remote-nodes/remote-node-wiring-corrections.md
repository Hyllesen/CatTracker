# remote-node-wiring.md: corrections

> **Status:** applied to `remote-node-wiring.md` and `final-remote-plan.md` on 3 Nov.
> Kept as the record of what changed and why; the two documents above are the ones
> to follow.

Replacement text for sections 0-3 of `remote-node-wiring.md`, plus the 915 MHz
settings that `final-remote-plan.md` section 6 and the sketches must agree on.
Everything here is untested on the bench; items marked **verify** need a
measurement or a look at the actual board before soldering.

## What changed, and why

| Was | Now | Why |
|-----|-----|-----|
| 915 antenna 19.2 cm | **8.2 cm** | quarter wave = 299.8 / (4 x 915) = 8.19 cm |
| RA-02 RST tied to 3V3 | RST on **GPIO1** | GPIO20/21 are free, so a GPIO is available; the SX1278 can drive NRESET during power-on reset, so a hard tie to 3V3 is a contention risk |
| RAK UART on GPIO1/10 | RAK UART on **GPIO20/21** | same pins as `node_link.ino` and the gateway; frees GPIO1 for RST |
| 915 at SF9 (~145 ms frame) | **SF7** (~41 ms frame) | channel occupancy 22% -> 6% with 4 nodes x 3 collars |
| CR "4/7" and readback `:4:` | CR value **0** (4/5) | RUI3 CR field is 0-3 (0 = 4/5, 2 = 4/7); 4 is not valid |
| no regulator or cell protection | buck-boost 3.3 V + protected cell | a 18650 runs 3.0-4.2 V |
| 2 antennas, 10 kohm pull-up | **3 antennas**, no pull-up | node 433, node 915, gateway 915; nothing uses the pull-up |
| RAK3172 "(H)" check via `AT+VER` | check the part number | `RAK3172-9-SM-NI` is the US915/AU915/KR920/AS923 part; **-NI = no IPEX, antenna goes on the RF pad** |

## 0. Parts (replacement table)

| Qty | Part | Where it goes |
|-----|------|---------------|
| 2 | RAK3172-9-SM-NI (915 band), on a breakout with an RF-pad antenna path | node (TX) + gateway (RX) |
| 2 | ESP32-C3 board with **native USB** (GPIO20/21 must not go to a USB-UART bridge) | node + gateway |
| 1 | RA-02 (SX1278, 433 MHz) | node |
| 3 | Antennas: node 433 (17.3 cm), node 915 (8.2 cm), gateway 915 (8.2 cm) | label each at the pigtail |
| 1 | Protected 18650 (with PCB) + holder | node |
| 1 | 3.3 V buck-boost module, 3.0-4.2 V in, >= 500 mA | node |
| 1 | CN3065 (or TP4056) charge module | node |
| 1 | Solar panel 5 V, 5-10 W | node |
| 4 | 100 uF electrolytic, one at each radio | |
| 3 | 100 nF ceramic | ADC divider, one at each radio |
| 2 | 100 kohm resistors | battery divider |
| 1 | 5 V USB supply | gateway |
| - | IP65 box, desiccant, pole | node |

## 1. Node pin map (ESP32-C3 <-> RA-02 <-> RAK3172)

The C3 has pins 0-10 plus GPIO20/21. GPIO2, GPIO8, GPIO9 are boot straps and
GPIO18/19 are USB D-/D+: **do not use any of those five.** With USB CDC as the
console and GPIO20/21 free (native-USB boards only), the node has 10 usable
pins and needs 9 signals.

| Signal | C3 pin | Goes to | Note |
|--------|--------|---------|------|
| SCK | GPIO4 | RA-02 SCK | |
| MISO | GPIO5 | RA-02 MISO | |
| MOSI | GPIO6 | RA-02 MOSI | |
| NSS | GPIO7 | RA-02 NSS | |
| DIO0 | GPIO0 | RA-02 DIO0 | RX-done interrupt; not a strap |
| RA-02 RST | **GPIO1** | RA-02 RESET | lets firmware reset the 433 radio |
| UART RX | **GPIO20** | RAK3172 TX2 | |
| UART TX | **GPIO21** | RAK3172 RX2 | |
| Battery divider | GPIO3 | 100k/100k midpoint + 100 nF | ADC1_CH3, not a strap |
| Spare | GPIO10 | - | suggested use: RAK3172 NRST (**verify** the pad name on your breakout) |
| Console | USB-C | laptop | `CDCOnBoot=cdc` |
| 3V3 / GND | | radios | never 5 V, never VIN |

Cross-check every row against your board's silkscreen: some clones label
GPIO numbers, some label only functions. Trust the number.

RadioLib init for this map (`RADIOLIB_NC` is the "no pin" constant):

```cpp
SX1278 radio = new Module(/*cs*/ 7, /*irq (DIO0)*/ 0, /*rst*/ 1, /*gpio*/ RADIOLIB_NC);
// setup(): SPI.begin(/*sck*/ 4, /*miso*/ 5, /*mosi*/ 6, /*ss*/ 7);
```

For an unattended node, read `RegVersion` about once a minute. If it is not
`0x12`, pulse RST and re-init the radio, and count the event in the heartbeat.

## 2. Gateway and bench base

| Signal | Gateway (ESP32-C3) | Bench base (NodeMCU) |
|--------|--------------------|----------------------|
| UART RX <- RAK TX2 | GPIO20 | D1 (GPIO5) |
| UART TX -> RAK RX2 | GPIO21 | D2 (GPIO4) |
| Console | USB-C | UART0 |

Using the same UART pins on the gateway and the node means one `PIN_RAK_RX/TX`
pair in the shared code. The bench base is unchanged.

## 3. 915 MHz settings (must match on node, gateway and bench base)

```
AT+P2P=915000000:7:125:0:8:14
```

915.000 MHz, SF7, BW 125 kHz, CR 4/5, preamble 8, 14 dBm. Read `AT+P2P=?` back
on both modules and compare the strings; never trust what you typed.

Airtime of the 9-byte backhaul frame (explicit header, CRC on, BW 125, CR 4/5):

| SF | Frame airtime | Occupancy, 4 nodes x 3 collars per 8 s |
|----|---------------|----------------------------------------|
| 7 | ~41 ms | ~6% |
| 8 | ~72 ms | ~11% |
| 9 | ~144 ms (CR 4/7: ~169 ms) | ~22% |
| 10 | ~248 ms | ~37% |

Start at SF7 and raise one step at a time, only if the measured hop needs it.
SF7 is roughly 5-6 dB less sensitive than SF9. Aim for hop SNR at least ~6 dB
above the demodulator limit at the worst node, measured in rain (limits:
SF7 -7.5 dB, SF8 -10, SF9 -12.5, SF10 -15).

Stagger: each node transmits `node_id x 250 ms` after hearing a ping. Keep a
small queue (4 entries) of pending reports; drop the oldest if it fills and
count the drops. At SF7 a 41 ms frame sits comfortably inside a 250 ms slot.

The two bench sketches (`node_link.ino`, `base_link.ino`) now use SF7 to match.

## 4. Antennas (replacement for section 3)

- **433 MHz:** 17.3 cm radiator + 17.3 cm counterpoise to GND only (unchanged).
- **915 MHz:** **8.2 cm** radiator + 8.2 cm counterpoise. Cut slightly long and
  trim if you have a way to check SWR; insulation shortens the wire a little.
- **-NI module:** there is no IPEX connector. Solder a short coax pigtail to the
  RF pad, shield to the neighbouring GND pad, and keep the unshielded part short.
- **Separation on the node:** put the 433 and 915 antennas **tens of cm apart**
  (opposite sides of the pole), not a few cm. 14 dBm at 915 MHz next to the
  RA-02 input can raise its noise floor and make it miss collar pings.
  **Verify:** read the RA-02's RSSI/noise floor with the RAK idle and while it
  transmits; they should match.
- Never power either radio without its antenna. Label every antenna at the
  pigtail before it goes on.

## 5. Power (replacement for section 2)

- **Supply:** battery -> 3.3 V **buck-boost** -> the board's 3V3 pin and both
  radios. A plain LDO board regulator is not safe to rely on: an AMS1117 needs
  about 4.4 V in for 3.3 V out and would die as soon as the cell sags. **Verify**
  the regulator marking on your C3 board either way.
- Feeding the 3V3 pin directly means the board's own USB regulator can fight
  the buck-boost. Flash with the battery disconnected, or disable the
  buck-boost's EN while USB is plugged in. **Verify** on your board.
- **Protection:** use a protected 18650, or a protection board, and keep the
  buck-boost's undervoltage cutoff around 3.2 V. The CN3065 only charges; it
  does not protect against over-discharge.
- 100 uF across 3V3-GND at each radio, 10 uF at the board's power entrance,
  100 nF at each radio and at the ADC pin.
- Battery divider: 100k/100k to GPIO3 with 100 nF at the pin, as before.

**Budget (estimates, measure to replace them):**

| Item | Current |
|------|---------|
| RA-02 continuous RX | ~12 mA |
| ESP32-C3, CPU running, WiFi off | ~20 mA |
| RAK3172 awake, idle | a few mA (**measure**); ~2 uA if put in its low-power mode |
| Planning figure | **35-40 mA** -> ~0.85-0.95 Ah/day -> ~3.2-3.5 Wh/day at 3.7 V |

With about 90% buck-boost and ~85% charger efficiency the panel must deliver
roughly 4.4 Wh/day. A 5 W panel needs about 0.9 equivalent full-sun hours, which
under dense canopy is marginal, not comfortable. Mount the panel above the
canopy edge, use 10 W, and measure the real average current and charge current
before the first field install. With no sun, expect about 2 days from a 2000 mAh
cell.

If the RAK3172's low-power mode is used, wake its UART with a throwaway byte
before each AT command, because the first byte after sleep can be lost.
**Verify** on your firmware.

## 6. Added rows for the symptom table

| What you see | What it usually is |
|--------------|--------------------|
| 915 hop works on the bench, dies at a few metres outdoors | 915 radiator cut to 19 cm instead of 8.2 cm |
| Node resets or goes dark as the cell sags | no 3.3 V regulator (AMS1117-class dropout), or no cell cutoff |
| Node hears collars well until it transmits | 433 and 915 antennas too close; compare RA-02 noise floor with the RAK idle vs transmitting |
| RA-02 stops responding after weeks, rest of node fine | no software reset path; RST must be on a GPIO, and the `RegVersion` watchdog must be running |
| Gateway receives nothing, both modules "OK" | `AT+P2P` readback differs on one field (including CR 0 vs another value) |
| Gateway drops many reports with 4+ nodes | SF too high for the number of nodes; check frame airtime vs occupancy |
