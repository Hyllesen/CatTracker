# POC: two ESP32-C3 + RAK3172 boards talking on 915 MHz

The shortest version of steps A and B in `remote-node-wiring.md`: **no 433 radio,
no battery, no solar, no MQTT, no ESPHome.** Two boards, four wires each, one
sketch, two roles. Done means: the talker sends every 8 s, the listener prints
every frame with RSSI/SNR, and **0 missed seq for 30 minutes**.

Sketch: `poc915/` (`poc915.ino`, `rak_event.h`, `test_rak_event.cpp`). It compiles
for both roles and the RX parse has a host test — run it before flashing.

## Parts

- 2 × ESP32-C3 board with **native USB** (GPIO20/21 must not go to a USB-UART bridge)
- 2 × RAK3172 **9xx** module (US915/AU915 — a `…-8-…` part is 868 and hears nothing). On the **-NI** part the antenna is soldered to the **RF pad (pad 12)**; there is no IPEX to click on.
- 2 × antenna at **8.2 cm** (λ/4 at 915 MHz). Label them. A 17 cm wire is the 433 antenna.
- USB cable to each board, and 100 µF across each module's 3V3-GND.

**Antenna before power.** A RAK3172 transmitting with no antenna can permanently
wreck its output stage. 3.3 V only, never 5 V, never VIN.

## Wiring (identical on both boards)

| C3 pin | Goes to |
|--------|---------|
| GPIO20 (RX) | module pad **2** = `PA2/UART2_TX` |
| GPIO21 (TX) | module pad **1** = `PA3/UART2_RX` |
| 3V3 | module pad **24** = VDD |
| GND | module pad **11/17/18/23/28** = GND |

Four wires. The console is the USB cable, not a second UART.

**Pad numbers are package-specific.** The 32-pad `RAK3172`/`RAK3172-T` has UART2 on
pads **1 and 2**; the 65-pad `RAK3172-SiP` datasheet numbers them **29 and 30**. The
names (`PA2/UART2_TX`, `PA3/UART2_RX`) are identical, so match the **name**, and
tell which datasheet you are reading.

Two pads decide whether the module speaks at all, and both failures are silent:
**BOOT0 (pad 21, high active) must be LOW** — floating, the module sits in the STM32
UART bootloader and answers nothing; **RST (pad 22) must be HIGH** — low, the module
is held in reset. A proper breakout pulls both. Check them before blaming the wires.

## Build and flash

```bash
# listener (the gateway side) — ROLE=0
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=0" --output-dir /tmp/poc_rx poc915
arduino-cli upload --input-dir /tmp/poc_rx --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" /dev/cu.usbmodemXXXX

# talker (the node side) — ROLE=1
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=1 -DNODE_ID=1" --output-dir /tmp/poc_tx poc915
arduino-cli upload --input-dir /tmp/poc_tx --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" /dev/cu.usbmodemXXXX
```

Find the port by USB vendor id `0x303a`, not by the `usbmodem` suffix (shuffled
between reboots). `python3 tools/serial_cmd.py` opens the C3 with **DTR low** —
asserting DTR resets the board. It also opens the port to watch the output.

`compile --output-dir` + `upload --input-dir`, because `arduino-cli upload` sends
the artifacts of the **last** build of that sketch: flash ROLE=0 and ROLE=1 from
separate output dirs or you can put the wrong role on a board with no error.

## Step 1 — listener only: does the module answer?

Flash ROLE=0, open the console. Expected:

```
POC915 ver 1  role LISTENER  node 1
RAK on GPIO20(RX)/GPIO21(TX), 915 MHz SF7 BW125 CR0 preamble 8, 14 dBm
[mod] AT+VER=RUI_4.x.x_RAK3172-E
[mod] sn 9181004E20900057
[mod] readback AT+P2P=915000000:7:125:0:8:14
[mod] continuous RX armed
```

If it prints `[warn] module not answering on UART2`, that is the wiring step, not
a dead module: swap RX/TX, check 3V3 and GND, press `b` (bridge) and type `AT` by
hand. Bridge mode forwards keystrokes to the module and prints its replies — the
fastest way to prove the four wires. `q` quits.

**Record `AT+VER`, `AT+SN` and the `AT+P2P=?` readback.** `AT+VER` reports the
firmware, not the band; the **label's part number** is the band. Read back, never
trust the string you typed.

## Step 2 — talker: does anything arrive?

Flash ROLE=1, power both, antennas on, a few metres apart. Listener should print
one line every 8 s:

```
[RX] node 1 seq 12  rssi -71 dBm  snr 11 dB  ok=12 miss=0
```

`miss` counts skipped seq, so a dropped frame is visible as a number, not as a
feeling. Press `p` for a readback, `r` to re-arm RX, `s` to send one now.

## Step 3 — walk away

Take the talker to the far end of the property and watch RSSI fall and `miss`
climb. Note the RSSI at the distance you actually need. **Then raise SF on the
talker only** (`-DSF=9`), and confirm the listener goes completely deaf — silence
with no error is the symptom you are recording, because that is exactly what a
parameter mismatch looks like in the field. Put SF back to 7 on both sides.

## Step 4 — 30 minutes, no drops

Leave it running 30 minutes. Acceptance: **one line per 8 s, `miss` 0**. Then the
915 hop is proven and the next step is `remote-node-wiring.md` step C (add the
433 RA-02 to the node).

## Traps in this POC (all silent)

| Symptom | Cause |
|---------|-------|
| No console at all | board is fine, `Serial.isPlugged()` is false — DTR low, and nothing is reading the port |
| `miss` climbs, frames arrive | one `AT+P2P` field differs between boards |
| One frame received, then silence | RX not re-armed (`r`). Continuous RX re-arms on `r`, and a new `AT+P2P=` **while armed kills reception** |
| `AT+PSEND` rejected | the module is in RX: `AT+PRECV=0` first |
| Everything fine, zero frames | one module is an 8xx (868) part, or the 433 antenna is on the 915 pad |
| Board resets mid-transmit | 100 µF missing at the radio |

Rules: **antenna before power**, DTR **low** on the C3, every print gated on
`Serial.isPlugged()`, `Serial.setTxTimeoutMs(0)` in `setup()`, and the 915 settings
must be identical on both boards. This POC touches nothing on the 433 side: no
collar firmware, no air settings, no 6-byte frame.
