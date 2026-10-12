# Cat Collar Transmitter — Setup

TENSTAR ROBOT nRF52840 Pro Micro + RA-02 (SX1278, 433 MHz), powered by a
150 mAh LiPo. Pings every 1 s while bench-testing; the handheld at `../handheld`
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

### `PING_INTERVAL_MS` is a test value

It is currently **1000 ms**, which is for bench testing only. With a 15-byte
payload at SF9/BW125 each ping occupies ~214 ms of airtime, so a 1 s interval
means the collar transmits roughly 21% of the time. **Put it back to 5000 ms**
before it goes anywhere near a 150 mAh battery.

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

Payload is `CAT-01;bat=100` — a literal string with a **hardcoded** 100%
battery. There is no binary framing, no CRC of our own, and no multi-cat
support yet. Real battery measurement still needs to go in.