# Handheld Homing Device — Setup

ESP32-C3 SuperMini + RA-02 (SX1278, 433 MHz). Console-only output for now;
speaker/LED feedback can be layered on later once those parts arrive.

## Board selection in Arduino

Board: **ESP32C3 Dev Module** (`esp32:esp32:esp32c3`)

## Wiring (RA-02 → ESP32-C3 SuperMini)

| RA-02 pin | C3 GPIO | Note |
|-----------|---------|------|
| GND       | GND     | |
| 3.3V      | 3V3     | **3.3V only.** Do not feed 5V. |
| SCK       | GPIO4   | default C3 SPI clock |
| MISO      | GPIO5   | default C3 SPI MISO |
| MOSI      | GPIO6   | default C3 SPI MOSI |
| NSS/CS    | GPIO7   | |
| DIO0      | GPIO3   | RX interrupt |
| RESET     | GPIO10  | |
| ANT       | antenna  | **Antenna is mandatory** — transmitting without one can damage the module. |

### Changes required from the original wiring

Two connections need to change. Everything else stays as-is.

| RA-02 pin | Was | Now | Why |
|-----------|-----|-----|-----|
| DIO0      | *not connected* | **GPIO3** | Required. RadioLib polls DIO0 in `receive()`; without it the radio can never signal a packet, so the handheld will hang or never report a signal. |
| RESET     | GPIO8 | **GPIO10** | GPIO8 is an ESP32-C3 strapping pin *and* drives the SuperMini's onboard LED. Its LED load pulls the pin, and GPIO8=0 with GPIO9=0 is an invalid boot combination that can break flashing. |

Original sketch used NSS=7, DIO0=2, RST=3. GPIO2 is also a strapping pin, so
DIO0 moved off it to GPIO3. On the C3, GPIO2/8/9 are strapping, 18/19 are USB,
20/21 are UART0 — GPIO3/4/5/6/7/10 are all safe.

## Air settings (must match the collar)

| Param | Value |
|-------|-------|
| Frequency | 433.0 MHz |
| Bandwidth | 125 kHz |
| Spreading factor | 9 |
| Coding rate | 4/7 |
| Sync word | `0x12` |
| TX power | 17 dBm |

The sync word `0x12` is a private value — it keeps your network from hearing
random public LoRa traffic.

## Flashing

Board **ESP32C3 Dev Module**, then upload `handheld/handheld.ino`. Open the
Serial Monitor at **115200 baud**.

### IMPORTANT: enable "USB CDC On Boot"

In the Arduino IDE: **Tools → USB CDC On Boot → Enabled**.

Without this, `Serial` is bound to UART0 (GPIO20/21), which on the SuperMini is
*not* wired to the native USB connector — the board boots fine but prints
nothing to the Serial Monitor. With it enabled, `Serial` maps to the native USB
CDC and output appears normally.

Command line:

```
arduino-cli upload --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
                    --port /dev/cu.usbmodem111301 handheld
```

## Audio feedback (buzzer)

Buzzer is driven from **GPIO21** via a transistor. Two buzzer types need
different handling, so the mode is switchable at runtime with **`t`**:

| Mode | Buzzer type | Behaviour |
|------|-------------|-----------|
| `PASSIVE` | No internal oscillator | Pitch rises with signal — 320 Hz far → 2200 Hz close |
| `ACTIVE` | Has internal oscillator (most 2-wire "DC 3-24V" buzzers) | Fixed pitch, so we chop the supply and vary the **duty cycle** — rare faint ticks when far, near-continuous buzz when close |

**Your SFM-27 II is an ACTIVE buzzer.** Confirmed empirically: switching to
PASSIVE made almost no audible difference, because its internal oscillator
overrides any tone applied to it. Only chopping the supply on/off conveys
distance. ACTIVE is therefore the default; `t` still cycles through the others.

### Signal → duty mapping

Calibrated from measured walks, **not** from datasheet theory. The first
attempt used `RSSI_CEIL = -45` (datasheet "point blank"), which pegged the bar
at 100% for everything within ~10 m and squeezed the whole usable range into
the top 20% — inaudible on a fixed-pitch buzzer.

| RSSI | Signal | Duty (of 4095) | Sounds like |
|------|--------|----------------|-------------|
| −34 dBm | 100% | 4095 | solid buzz (arm's length) |
| −42 dBm | 94% | 3859 | solid buzz |
| −50 dBm | 71% | 2915 | obvious gaps |
| −59 dBm | 46% | 1892 | even beeps |
| −65 dBm | 29% | 1199 | sparse ticks |
| −75 dBm | 0% | silent | silent |

The Serial Monitor prints the live duty value, so you can confirm the mapping
without walking: walk away and watch `tone` fall from 4095 toward 32.

### ⚠️ ESP32-C3 LEDC resolution limit (cost real debugging time)

The buzzer silently did nothing until this was understood. On the ESP32-C3 the
**LEDC divider** — not the duty bits — is the limiting factor. Measured on this
chip with `ledc_probe/ledc_probe.ino`:

| Resolution | Lowest reachable frequency |
|---|---|
| 8-bit | ~156 Hz |
| 10-bit | ~39 Hz |
| 11-bit | ~20 Hz |
| **12-bit** | **~10 Hz** |

So:
- **ACTIVE mode needs 12-bit** to gate the supply at 10 Hz.
- **PASSIVE mode can stay 8-bit**, because 320–2200 Hz tones are well above the
  8-bit floor of ~156 Hz.

If you request a frequency the divider can't reach, `ledcAttach()` **fails**,
and `ledcWrite()` to an unattached pin is a **silent no-op**. The buzzer then
just reacts to a floating pin and appears to "ignore changes". The firmware
now checks the `ledcAttach()` return value and disables the buzzer loudly if
it fails, so this can never fail quietly again.

Run `ledc_probe/ledc_probe.ino` to re-derive this table on your own chip.

### Wiring (2N3904 as a low-side switch)

The GPIO pin cannot supply the buzzer's current, so a transistor is used as a
valve. **The buzzer's `+` wire goes to 3V3, not to the transistor.**

Connection list:

| Connect this | to this |
|---|---|
| Buzzer **RED (+)** | **3V3** on the C3 |
| Buzzer **BLACK (−)** | **Collector (C)** of the transistor |
| C3 **GPIO21** | one side of the **1 kΩ** resistor |
| Other side of the **1 kΩ** resistor | **Base (B)** of the transistor |
| **Emitter (E)** | **GND** |

Drawn out:

```
        3V3 (C3)
          |
          |          Buzzer
          +--------- RED (+)
          |
        BLACK (-)
          |
          C
     GPIO21--[1k]--B      2N3904 (or 2N222A)
                     E
                     |
                    GND
```

How it works: the buzzer's `+` always has 3V3. Its `−` only reaches ground
when GPIO21 drives the base high, which lets the transistor conduct. So GPIO21
high = buzzer sounds; GPIO21 low = buzzer silent. That is how the firmware
turns it on and off to make beep patterns.

- **Use the 2N3904 or 2N222A (NPN).** Leave the 2N3906 (PNP) alone — it's not
  the right part for a low-side switch here.
- The **1 kΩ base resistor is important.** Without it you can destroy the
  transistor's base junction, and the buzzer may never get full current.
- Leg order with the flat side facing you is usually **E-B-C**, but it varies
  by manufacturer. If it doesn't beep, swap the collector and emitter — that's
  a faster check than trying to identify the legs.

### Checking which type you have

Wire it up, then listen while packets arrive. If you get **one steady pitch**,
it's ACTIVE (leave it there). If it makes **no sound at all** on DC, it's
PASSIVE — press `t` until the banner says PASSIVE and it will sing tones.

### Notes

- Tone/beeps go silent when nothing has been received for ~8 s.
- The buzzer is rated 3-24 V, so at 3.3 V it runs at minimum drive and will be
  quiet. For more volume later, drive it from a higher rail through the same
  transistor once the battery supply is in place.
- Current pin budget: radio 3,4,5,6,7,10 · LED 8 · BOOT 9 · buzzer 21.
  GPIO1, GPIO2, GPIO20 and GPIO0 remain free (GPIO0 = ADC1_CH0, reserved for
  battery monitoring).

## Verifying it works

The collar firmware (`collar/collar.ino`) now transmits, so you can verify the
real link directly — no second board needed:

1. Flash `collar/collar.ino` to the nRF52840 + RA-02.
2. Flash `handheld/handheld.ino` to the ESP32-C3 + RA-02 (with USB CDC on boot).
3. The collar pings every 5 s; the handheld should log each one.

`transmitter/transmitter.ino` is a spare ESP32-C3+RA-02 transmitter for bench
testing the handheld on its own, if you want a second board.

### Walking test (RSSI validation)

Before adding a speaker or display, confirm RSSI actually tracks distance. The
handheld logs to flash so it survives being unplugged, which means no laptop
cable is needed while you walk.

**Setup**

1. Keep the collar plugged into the laptop at your desk — it's stationary,
   simulating a cat somewhere out in the trees. It pings every 1 s while
   bench-testing.
2. Plug the handheld into the laptop, Serial Monitor **115200**.
3. Send `c` to clear the log, then **`w` to arm walk-test mode**.
4. Unplug the handheld and plug it into a **power bank** (or any 5 V USB
   source). It boots into walk-test mode because it was armed.
5. **LED gives you the status:** double-blink on startup, then one short flash
   per logged sample (~2/s). A steady LED means it is *not* recording — send
   `x` and go back to step 2.
6. Press **BOOT** at each waypoint you want to mark. No need to measure
   distances — use time and landmarks instead.
7. Walk back, unplug the power bank, replug into the laptop.
8. Send `d` to dump CSV, then:

```
python3 tools/analyze_walk.py walk.csv
```

### Why you must arm walk mode with `w`

Walk mode used to be inferred from "no USB host detected", but **power banks
enumerate as USB hosts**, so the board sat in console mode and logged nothing.
Arming stores the mode in NVS, which survives the unplug/replug that starts
the walk. Replugging into a laptop auto-disarms, so you only arm once per walk.

### Commands (Serial Monitor, 115200)

| Key | Action |
|-----|--------|
| `d` | Dump the stored log as CSV |
| `c` | Clear the log (do this before each walk) |
| `r` | Print modem registers |
| `w` | Force walk-test mode without unplugging |
| `x` | Leave walk-test mode (only while serial is attached) |

### CSV columns

`t_ms,rssi_dbm,snr_db,flags,raw_dbm`

- `rssi_dbm` — smoothed (5-packet average), or `-127` before the first packet
- `raw_dbm` — un-smoothed RSSI from the last received packet
- `flags` — bit0 = fresh packet this interval, bit1 = waypoint marker,
  bit2 = receive timed out, bit3 = receive error

### Notes

- `t_ms` restarts at zero on every boot, so **clear the log before each walk**
  (`c`) or timestamps from separate walks won't line up. Data does survive
  unplugging, so you can also just dump and clear whenever.
- `raw_dbm` is the un-smoothed value if you want to judge the noise yourself.
- The log holds ~120,000 samples (1.375 MB), about 17 hours at 2 samples/s.
- The collar is set to ping every **1 s** for bench testing. Put
  `PING_INTERVAL_MS` back to `5000` before running it on a battery — it
  quadruples transmit energy use, which matters a lot on a 150 mAh collar.

### What you should see

```
[+] pkt #7  payload=CAT-01;bat=100
    RSSI raw -29.0 dBm | SNR 13.8 dB | smoothed -27.2 dBm | peak -26.0 dBm
    signal [####################] 100%
    very close - check under bushes
```

Interpretation:

- **RSSI closer to 0 dBm = closer.** Around -30 dBm is very close (desks apart);
  -110 dBm is near the detection limit.
- **SNR** should stay positive (above 0 dB) for a usable link; negative means
  the link is near the noise floor.
- After ~8 s without a packet the handheld prints a "no collar signal" warning.
  A live cat should keep pinging; silence likely means you're out of range or
  the collar is asleep.

## Register-level gotchas (cost hours to find)

If you change either end's modem setup, these bite:

- **Write modem registers field-by-field, never whole-byte.** The SX1278 powers
  up with bits set that matter: `RegModemConfig1` bit2 (payload CRC),
  `RegModemConfig2` bit2 (CRC aggregation), `RegModemConfig3` bit2 (AGC),
  and the upper bits of `RegInvertIQ` / `RegDetectOptimize`. RadioLib only
  overwrites specific bit ranges and leaves the rest. Writing a full byte
  zeroes those defaults and the receiver fails with
  `RADIOLIB_ERR_LORA_HEADER_DAMAGED` (-24) — TX reports success, nothing decodes.
  The collar's `setRegBits()` mirrors RadioLib's `SPIsetRegValue()` semantics
  (`collar/collar.ino`) for exactly this reason.
- **`radio.receive(str, len, timeout)` — pass `len = 0`.** If you pass a non-zero
  `len`, RadioLib does *not* query the real packet length and pads the string
  with trailing FIFO bytes.
- **nRF52 `Serial` needs `#include <Adafruit_TinyUSB.h>`.** Without it the link
  fails with `undefined reference to Adafruit_USBD_CDC`.

Both boards print a modem register dump at boot. When debugging an RF problem,
compare those dumps first — a mismatch there is the cause far more often than
anything physical.

## Current limitations

- Console only — no speaker/LED yet (parts not received).
- The payload format is still ad-hoc: the collar sends `CAT-01;bat=100` as a
  literal string with a hardcoded 100% battery. There is no binary framing, CRC
  of our own, or multi-cat support yet. The handheld echoes whatever arrives, so
  it will adapt once the collar format is defined properly.
- The collar currently draws full transmit power every 5 s with no duty cycle or
  sleep, which will flatten a 150 mAh collar battery in hours. It needs a real
  power budget (and a battery-voltage reading) before it goes on a cat.
- Homing is RSSI-based proximity only. This tells you *how close* the cat is,
  not *which direction* — direction-finding needs a directional antenna or
  multiple base stations (part 3 of the project). For now, walk toward a
  stronger signal.
- Jungle foliage attenuates and scatters 433 MHz badly; expect noisier RSSI
  than an open-field test.