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
| — | **SCK** | `011` | P1.11 |
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

### ⚠️ Pro Micro silkscreen labels are not pin numbers

`011` means **P1.11**, `002` means **P0.02**. The label is the nRF port number
followed by the pin, and the port is the important half: P0 and P1 are different
GPIO ports that share the same pin numbering. A wire soldered to P0.11 instead
of P1.11 is a completely different pin and the radio will never initialise.

The firmware comments this in place (`collar/collar.ino`):

```cpp
// Silkscreen "111" = P1.11, "002" = P0.02, etc.
const GPin P_SCK  = { NRF_P1, 11 };
```

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

**Charge current:** if the board uses a TP4056 at its default 1 A, that is ~7C
for a 150 mAh cell — well above the 0.5C a small pouch cell tolerates, and it
will swell over many cycles. The charge-current resistor is `R_prog` near the
charge IC; ~1.2 kΩ gives 1 A, ~10 kΩ gives ~100 mA. If it cannot be changed,
keep charge sessions short and do not top the cell up daily.

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
     [1M]  R1
      │
      ├──────────────  "029" pad   ← net B, the junction
      │
     [1M]  R2
      │
   cell −            "B-" pad
      │
    "GND" pad                     ← net C, also the cell negative
```

Resistors are not polarised — orientation does not matter.

**Four solder joints, no more:**

| # | From | To |
|---|------|-----|
| 1 | cell `+` | `B+` pad |
| 2 | cell `−` | `B-` pad |
| 3 | **R1** | one leg to `B+`, other leg to `029` |
| 4 | **R2** | one leg to `029`, other leg to any `GND` pad |

`R2`'s free leg goes to a `GND` pad rather than `B-` because those two are
**already the same net inside the board** — the cell negative and the system
ground are tied through the protection circuit. Do not add a wire between them;
nothing needs connecting there.

What matters is that `029` ends up touching **three** things: R1's leg, R2's
leg, and the pad. That shared junction is what makes this a divider instead of
a wire.

Two equal 1 MΩ resistors give the 1:2 the firmware expects. That divides
4.2 V down to 2.1 V, inside the 3.0 V ADC reference, and costs only ~2 µA
against ~10 mA for the radio. **Do not substitute a 10 KΩ pair**: that draws
210 µA and costs roughly 3 mAh over a 16-hour run, which is real on a 150 mAh
cell.

**Verify by the reading, not by trusting the pin number:**

| Voltage at `029` | Meaning |
|------------------|---------|
| **~2.05 V** | Correct — a 4.1 V cell reads ~95% in the app |
| 0 V | Wrong pin, or not connected |
| 3.3 V | **Divider missing** — the ADC is being fed the full cell |

Until that pin is wired, the collar reports `--` and the app shows
**`battery --`**. That is deliberate: a false low-battery warning on a cat
tracker is worse than no warning at all, because it trains you to ignore it.

The cell is sampled once a minute, not every 5 s ping, leaving the SAADC idle
between reads.

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

```
arduino-cli upload --fqbn "adafruit:nrf52:feather52840" \
                   --port /dev/cu.usbmodemXXXXXX collar/collar.ino
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
| Nothing at all | Not booting → power, or RESET (`031`) shorted |

**3. Compare modem registers.** Both boards print a register dump at boot. A
mismatch there is the cause far more often than anything physical.

**4. Check the SPI joints.** A cold or bridged solder joint on `CS` (`013`)
stops register writes landing, so the radio never initialises. Same symptom on
`SCK` (`011`) or `MOSI` (`015`); a loose `MISO` (`002`) leaves the radio
initialised but permanently silent.

## Commands (Serial Monitor, 115200)

| Key | Action |
|-----|--------|
| `BOOT` pin | Ping immediately instead of waiting for the interval |

The BOOT pin is `0`, configured `INPUT_PULLUP`, so the button pulls it low.

## Output

```
[TX] ping #488 sent
```

Payload is `CAT-01;bat=NN`, where `NN` is the cell charge measured through the
`029` divider (see "Battery sense" above) — or `--` when that pin is not wired.
It is plain ASCII: no binary framing, no CRC of our own, and no multi-cat
support yet.