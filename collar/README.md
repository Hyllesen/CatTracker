# Cat Collar Transmitter — Setup

TENSTAR ROBOT nRF52840 Pro Micro + RA-02 (SX1278, 433 MHz), powered by a
150 mAh LiPo. Pings every 5 s; the handheld at `../handheld`
receives them and forwards the signal to the phone.

The SPI pins are **bit-banged in software** — the nRF52840's hardware SPI is
not used. See "Why bit-banged SPI" below.

## Board selection in Arduino

Board: **Adafruit Feather nRF52840 Express** (`adafruit:nrf52:feather52840`)

The board is silkscreened *TENSTAR ROBOT Pro Micro* but enumerates as the
Adafruit Feather. Select the Adafruit board; it works.

## Wiring (RA-02 → nRF52840)

The RA-02 SPI pads are identified by their **silkscreen labels**, not by pin
number. Match by label. The five wires below are the complete list.

| RA-02 pad | Function | Pro Micro label | Also written |
|------------|----------|-----------------|--------------|
| 1, 2, 9 | **GND** | GND | use either GND pad |
| 3 | **3.3V** | 3.3V / VCC | **3.3 V only.** Never 5 V. |
| 4 | **RESET** | `031` | P0.31 |
| — | **SCK** | `011` | P0.11 |
| — | **MISO** | `002` | P0.02 |
| — | **MOSI** | `015` | P1.15 |
| — | **NSS / CS** | `013` | P1.13 |
| — | **ANT** | antenna + counterpoise | IPEX socket or ANT pad |
| 5, 6, 7, 8, 10 | DIO0–DIO4 | **not connected** | unused |

**DIO0–DIO3 are deliberately left unconnected.** The firmware polls the SX1278's
IRQ flags register for TxDone instead of using the DIO0 interrupt pin
(`collar/collar.ino:228`), so nothing needs to be attached to them. If you have
a wire on one, it is doing nothing — and a long wire on a floating pad picks up
noise, which is worth knowing when debugging.

The five connected signals are: SCK→`011`, MISO→`002`, MOSI→`015`,
NSS→`013`, RESET→`031`, plus GND, 3.3V and ANT.

### ⚠️ `011` is P0.11, not P1.11

The silkscreen label is the nRF port number followed by the pin, so `002` is
P0.02 and `115` is P1.15. **`011` is P0.11.**

That one is genuinely ambiguous on its face — P0.11 and P1.11 are both real,
adjacent pads, and the label does not say which. The firmware drove **P1.11**
for a long time, a pad on this board nothing is soldered to. The symptom is
silent and deeply confusing: the radio is never clocked, so every register
reads `0x00`, `RegVersion` never equals `0x12`, the firmware reports
`SX1278 NOT detected`, and the collar stops beaconing. That is
indistinguishable from a flat battery or a dead radio, which is how it
survived so long.

Confirmed by brute-forcing the bus: of 128 combinations of the plausible SCK,
MOSI, CS and MISO pads, exactly one returned `RegVersion = 0x12` — SCK on
**P0.11**. MOSI, CS, MISO and RESET were already correct.

The firmware comments this in place (`collar/collar.ino`):

```cpp
const GPin P_SCK  = { NRF_P0, 11 };
```

**Do not trust a silkscreen port number you have not measured.** If the radio
stops answering, sweep the candidate pads rather than re-reading the wiring.

## Antenna

**An antenna is mandatory. Transmitting with none attached can permanently
damage the RA-02's output stage.** Symptoms of a damaged module: it still
receives normally, but has very low transmit power and almost no range.

The collar transmits at 17 dBm, so the antenna carries a lot of power — take
this seriously.

### Quarter-wave monopole + counterpoise

At 433 MHz a quarter wave in air is **17.32 cm**. The RA-02's own ground plane
is only 17 × 16 mm, far too small for a monopole to work properly, so a second
"counterpoise" wire to ground makes a large difference.

| Element | Length | Connects to |
|---------|--------|--------------|
| Radiator | 17.3 cm | **ANT** (IPEX centre pin, or ANT pad) |
| Counterpoise | 17.3 cm | **GND only** |

Measured on the handheld's side at 3 m:

| Counterpoise angle | Gain vs none |
|--------------------|--------------|
| Parallel / badly placed | **−1.9 dB** |
| ~90° to the radiator | **+8.1 dB** |

That 10 dB swing is the orientation effect — the angle between the two wires
matters far more than whether the wire exists. **Fix the geometry rigidly**
(solder, heat-shrink or tape it) so it holds regardless of how the collar lies.

### ⚠️ The counterpoise must touch GND and nothing else

This is the single most likely cause of a dead collar. A 17 cm wire soldered
across the module can bridge neighbouring pads. Check specifically that it does
**not** touch any pad that actually has a wire on it:

| If it bridges to | Symptom |
|------------------|---------|
| **ANT / IPEX centre** | short across the PA — damaged or dead TX |
| **3.3V** | short across the rail — board won't boot |
| **RESET (`031`)** | held in reset — board appears completely dead |
| **NSS/CS (`013`)** | register writes never land — radio never initialises |
| **SCK (`011`)** or **MOSI (`015`)** | same — radio never initialises |
| **MISO (`002`)** | radio initialises but stays permanently silent |

Bridging a **DIO** pad does nothing, since none of them are connected.

If you suspect a bridge, **unpower the board before inspecting it.**

## Air settings (must match the handheld)

| Param | Value |
|-------|-------|
| Frequency | 433.0 MHz |
| Bandwidth | 125 kHz |
| Spreading factor | 9 |
| Coding rate | 4/7 |
| Sync word | `0x12` |
| Preamble | 8 |
| TX power | 17 dBm |

Sync word `0x12` is a private value, so the network ignores random public LoRa
traffic.

### `PING_INTERVAL_MS` is a field value

It is currently **5000 ms**. With a 15-byte payload at SF9/BW125 each ping
occupies ~214 ms of airtime, so at 1 s the collar transmitted roughly 21% of
the time and the 150 mAh cell lasted about 5 hours. At 5 s the airtime duty
cycle is ~4%, giving roughly 16 hours.

The handheld's `RX_TIMEOUT_MS` must stay slightly above this value
(5200 ms), otherwise the receive window closes before the ping arrives.

### Serial logging is off by default — press `v`

Both boards run with **no serial output** until you send `v` (115200 baud)
over USB. This is deliberate and load-bearing:

The serial port is native USB CDC. When the board is powered from a power
bank or wall charger, *nothing drains that port*. An unconditional
`Serial.print()` therefore fills the CDC buffer and blocks — and because it
blocks inside the transmit path, the collar stops beaconing altogether. The
symptom is indistinguishable from a flat battery: the tracker simply goes
silent minutes after you walk away from it.

This was actually observed, stalling the handheld's main loop for 52 s.

Attach a cable, press `v`, and diagnostics appear. Press `v` again to silence.
The collar additionally requires `TinyUSBDevice.mounted()` before it writes
anything, since it normally runs with no USB attached at all.

**For any enclosure or field build, never re-enable unconditional printing.**

## Battery

### Connecting the cell: use `B+` / `B-`

This board (SAMIROB / FEIYANG nRF52840 Pro Micro) has `B-` and `B+` pads
flanking the USB-C connector, alongside separate `RAW` (unregulated USB 5 V) and
`VCC` (regulated system rail) pads. Those pads exist so a single-cell LiPo
connects there **instead of** `VCC`.

| Cell | Board pad |
|------|-----------|
| `+` | **`B+`** |
| `-` | **`B-`** |

Do not connect the cell to `VCC`. Doing so ties the cell to the supply rail
with no charging path, and back-feeds 5 V from USB into a 4.2 V cell.

### ⚠️ Confirm three things with a multimeter before the first solder

The presence of `B+`/`B-` and a charge LED is strong evidence of a LiPo-capable
design, but the pads alone do not prove a charger is populated. Three checks,
about a minute total, each with a pass condition:

| Check | Method | Pass |
|-------|--------|------|
| **Does it charge?** | Cell on `B+`/`B-`, meter DC across `B+` and `GND`, plug in USB | Reading creeps toward **4.2 V**. Flat at 4.1 V = pads present but charging is external |
| **Is the rail safe?** | Cell connected **and** USB plugged, meter the `VCC` pad | **~3.3 V**. If it reads 4.1 V or 5 V, the battery rail is hard-tied to VCC — stop |
| **Is it protected?** | Run the tracker flat in normal use, then measure the cell | **≥3.0 V.** Below ~2.7 V means no undervoltage protection |

That last one matters most for a cat collar. The board will cheerfully transmit
until the cell is exhausted, and a LiPo taken below ~3.0 V suffers permanent
internal damage — copper plating strips off the plates. If the cell measures
below 2.7 V after a run, fit a 1-cell protection board (a few cents, roughly
postage-stamp sized) before putting it on a cat.

The TENSTAR manual confirms the first two checks: the board takes the cell on
`B+`/`B-`, charges at **100 mA**, and runs a 3.3 V LDO. It does not document
the protection circuit, so that one stays a bench check.

**Charge current is 100 mA** — ~0.67C for a 150 mAh cell, which is a sensible
rate. No change needed.

### Entering the bootloader

There is no BOOT button on this board. **Short the `RST` pad to a `GND` pad
twice, within half a second.** On the right edge `GND` is 3rd from the USB end
and `RST` is 4th, so they are adjacent — bridge them with a jumper and flick it
twice quickly.

The board then appears as a **USB mass storage drive** (named `NICENANO` on
these clones, containing `CURRENT.uf2`, `INDEX.HTM` and `INFO_UF2.TXT`).
Flash by dragging a `.uf2` onto it — no port selection needed.

**LED tell:** the bootloader makes the status LED *fade*; the running
application makes it *blink*. A steadily blinking blue LED is the app, not the
bootloader.

### Battery sense: `029` → app

The app shows collar charge as a percentage, carried in the payload
(`CAT-01;bat=NN`) and relayed by the handheld. The cell's positive terminal
needs a divider onto a free ADC pin.

**Use the pad labelled `029` (P0.29).** It is silkscreened with the raw port
number, so the pad you solder to and the `BATT_PIN` constant in the firmware
are the same number with no translation in between. `029` is free — the radio
uses `002`, `011`, `013`, `015` and `031`.

```
   cell +            "B+" pad
      │
     [100k]  R1
      │
      ├──────────────  "029" pad   ← net B, the junction
      │
     [100k]  R2
      │
   cell −            "B-" pad
      │
    "GND" pad                     ← net C, also the cell negative
```

A **100 nF capacitor from `029` to GND** is required. It acts as a local charge
reservoir for the SAADC's sampling cap. Without it, the 50 kΩ source impedance
means the ADC's internal cap does not fully charge during the 3 µs acquisition
window, and the reading is consistently ~7% low. Place it as close to the `029`
and GND pads as practical.

Resistors are not polarised — orientation does not matter.

**Five solder joints, no more:**

| # | From | To |
|---|------|-----|
| 1 | cell `+` | `B+` pad |
| 2 | cell `−` | `B-` pad |
| 3 | **R1** | one leg to `B+`, other leg to `029` |
| 4 | **R2** | one leg to `029`, other leg to any `GND` pad |
| 5 | **100 nF cap** | one leg to `029`, other leg to any `GND` pad |

`R2`'s free leg goes to a `GND` pad rather than `B-` because those two are
**already the same net inside the board** — the cell negative and the system
ground are tied through the protection circuit. Do not add a wire between them;
nothing needs connecting there.

What matters is that `029` ends up touching **three** things: R1's leg, R2's
leg, and the pad. That shared junction is what makes this a divider instead of
a wire.

Two equal 100 kΩ resistors give the 1:2 the firmware expects. That divides
4.2 V down to 2.1 V, inside the 3.6 V ADC full-scale (0.6 V bandgap × 6
gain, the core's default SAADC config), and costs ~20 µA against ~10 mA for
the radio. **Do not substitute a 10 kΩ pair**: that draws
210 µA and costs roughly 3 mAh over a 16-hour run, which is real on a 150 mAh
cell. 100 kΩ is 10× below that trap and 10× lower impedance than 1 MΩ,
which is what keeps the ADC reading within ~1% of a multimeter.

**Verify by the reading, not by trusting the pin number:**

| Voltage at `029` | Meaning |
|------------------|---------|
| **~2.05 V** | Correct — a 4.1 V cell reads ~95% in the app |
| 0 V | Wrong pin, or not connected |
| 3.3 V | **Divider missing** — the ADC is being fed the full cell |

Until that pin is wired, the collar reports `--` and the app shows
**`battery --`**. That is deliberate: a false low-battery warning on a cat
tracker is worse than no warning at all, because it trains you to ignore it.

The cell is sampled every 10 min in production (every 5 s in `DEV_MODE`),
leaving the SAADC idle between reads.

## Power

**The collar will not run reliably from a power bank.** It draws only ~9 mA
average, and most power banks switch themselves off below a 50–100 mA load
threshold. It then appears to die after a minute or two.

Use a **USB wall charger** for field tests, or a bank with an "always on" mode.
The handheld does not have this problem — it draws more (BLE + RX + a blinking
LED), enough to keep the same banks awake.

Both boards are happiest on USB for bench work.

## Flashing

Board **Adafruit Feather nRF52840 Express**, then upload `collar/collar.ino`.
Serial Monitor at **115200 baud**.

The collar supports two build modes — controlled by a compile-time flag:

| Mode | `BATT_SAMPLE_MS` | Use case |
|------|------------------|----------|
| **production** (default) | 10 min | Battery life; normal field use |
| **development** (`DEV_MODE`) | 5 s | Debugging; see voltage changes immediately |

```bash
# Production (default)
arduino-cli upload --fqbn "adafruit:nrf52:feather52840" \
                   --port /dev/cu.usbmodemXXXXXX collar/collar.ino

# Development (battery every 5 s)
arduino-cli compile --fqbn "adafruit:nrf52:feather52840" \
                    --build-property "compiler.cpp.extra_flags=-DDEV_MODE" \
                    collar/collar.ino
arduino-cli upload -p /dev/cu.usbmodemXXXXXX --fqbn "adafruit:nrf52:feather52840" \
                   collar/collar.ino
```

⚠️ **USB ports swap between the two boards.** With both plugged in, `/dev/cu.usbmodem*`
suffixes get reassigned on every reboot. Identify the board by what it prints,
not by port name:

| Board | Enumerates as | Prints |
|-------|---------------|--------|
| Collar | Adafruit Feather nRF52840 Express | `[TX] ping #N sent` |
| Handheld | ESP32 Family Device | `Cat Tracker - Handheld + BLE` |

### `Serial` needs TinyUSB

The sketch includes `<Adafruit_TinyUSB.h>`. Without it the build fails with
`undefined reference to Adafruit_USBD_CDC`.

## Why bit-banged SPI

The RA-02 is wired to arbitrary nRF pins, not the hardware SPI peripheral, so
the firmware toggles the pins directly. Two consequences worth knowing:

- **SPI is slow** (~2 µs per bit). This is irrelevant for a 15-byte packet sent
  once per second.
- **Register writes must be field-by-field, never whole-byte.** The SX1278
  powers up with bits set that matter. Writing a full byte zeroes them and the
  receiver fails with `RADIOLIB_ERR_LORA_HEADER_DAMAGED` (-24). The collar's
  `setRegBits()` mirrors RadioLib's `SPIsetRegValue()` semantics for this reason.

## Troubleshooting: handheld receives nothing

Work down this list — it splits the problem in half at each step.

**1. Is the collar powered?**
Charger LED lit, board LED blinking. This board has failed on power more than
once. No LED means stop here.

**2. Plug the collar into a laptop and read its serial.** This is the single
most informative test and takes 20 seconds.

| Serial output | Meaning |
|---------------|---------|
| `[TX] ping #N sent` | Board boots, SPI fine → problem is power or antenna |
| `SX1278 NOT detected` | Board boots but the radio does not answer on SPI — check the pads, see above |
| Nothing at all | Not booting → power, or RESET (`031`) shorted |

`SX1278 NOT detected` means `RegVersion` read something other than `0x12`.
That is the exact state a wrong SCK pad produces, so check `011` = P0.11 before
suspecting the module.

**3. Compare modem registers.** Both boards print a register dump at boot. A
mismatch there is the cause far more often than anything physical.

**4. Check the SPI joints.** A cold or bridged solder joint on `CS` (`013`)
stops register writes landing, so the radio never initialises. Same symptom on
`SCK` (`011`) or `MOSI` (`015`); a loose `MISO` (`002`) leaves the radio
initialised but permanently silent.

## Commands (Serial Monitor, 115200)

| Key | Action |
|-----|--------|
| `v` | Toggle verbose logging: TX status, battery, modem register readback |

There is no BOOT button on this board. The firmware used to poll Arduino pin 0
for one, but pin 0 is P0.11 — which is SCK — so reading it as a button input
configured the clock line as a pull-up and stopped the radio responding. That
poll has been removed.

## Output

```
[TX] ping #488 sent
```

Payload is `CAT-01;bat=NN`, where `NN` is the cell charge measured through the
`029` divider (see "Battery sense" above) — or `--` when that pin is not wired.
It is plain ASCII: no binary framing, no CRC of our own, and no multi-cat
support yet.