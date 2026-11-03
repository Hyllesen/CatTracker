# poc915 — the 915 MHz hop, proved on two ESP32-C3 + RAK3172 boards

One sketch, two roles. Nothing else on the board: no 433 radio, no battery, no
WiFi, no MQTT. This is the bench hop from `docs/plans/remote-nodes/poc-915-hop.md`,
which has the wiring table, the build commands, the checks after every step, and
the symptom table.

Four roles, one sketch. **Each role is built to its own `--output-dir`** —
`arduino-cli upload` sends the artifacts of the *last* build, so a stray compile can
put the wrong role on a board with no error at all.

```bash
# listener / gateway side        ROLE=0
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=0" --output-dir /tmp/poc_rx poc915

# plain talker, no 433 radio     ROLE=1 + node id
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=1 -DNODE_ID=1" --output-dir /tmp/poc_tx poc915

# NODE: RA-02 on 433 + RAK uplink on 915. ROLE=2. Needs the collar decoder header,
# which resolves next to the YAML in ESPHome, so the include path is passed here.
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=2 -DNODE_ID=1 -I$PWD/homebase/esphome" \
  --output-dir /tmp/poc_node poc915

# BASE: RA-02 on 433 + RAK listening on 915. ROLE=3. Hears collars directly, hears the
# nodes' reports, and matches them on (collar id, COLLAR seq) so the same packet is
# measured twice. Console adds `v` for the comparison stats.
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=3 -DNODE_ID=1 -I$PWD/homebase/esphome" \
  --output-dir /tmp/poc_base poc915

arduino-cli upload --port /dev/cu.usbmodemXXXX --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" --input-dir /tmp/poc_node
```

**Node ids are baked at compile time and must be unique.** Two nodes reporting as
`NODE_ID=1` merge into one row at the house. Record node id → MAC → site in the board
table below.

Wiring, four wires, same on both boards: **GPIO20 (RX) → module pad 2
(`PA2/UART2_TX`), GPIO21 (TX) → module pad 1 (`PA3/UART2_RX`), pad 24 VDD → 3.3 V,
GND**, 100 µF across VDD-GND, 8.2 cm antenna on pad 12, and the console on the USB
cable (DTR low, vendor id `0x303a`).

Console keys, all roles: `h` help, `b` bridge (type AT commands straight to the
module), `p` readback, `r` re-arm RX, `s` send one now, `x` stop RX, `d` dump the
walk log. `s` is talker-side — the listener is in continuous RX and `AT+PSEND` is
rejected there.

Node role adds: `k` 433 register check, `i` 433 dump, `t` self-test TX (sends a real
collar frame out of this board's own RA-02 — RX and TX share the antenna, so a decode
proves SPI, settings and the decoder in one act), `c` collar roster.

`rak_event.h` decodes `+EVT:RXP2P:<rssi>:<snr>:<hex>` and the POC's payload text:

```
CAT<n> <seq> c<id> r<rssi>      e.g.  CAT1 2 c1 r-86
```

`c-1` means **nothing heard in the last 30 s** — a relay that keeps asserting a cat it
stopped hearing looks exactly like a cat standing still, so a stale collar is reported
as `c-1` rather than repeating an old RSSI. A plain talker frame (`CAT1 12`) parses too.

**This ASCII payload is provisional.** The locked plan (`TODO.md` task 2) is a 9-byte
binary frame, magic `0xD1`, node id + collar id + seq + RSSI×10 + SNR×2 + battery +
flags, in a shared host-tested codec. The POC prints text because a text line you can
read on two consoles is what made the two silent firmware bugs visible. Replace it
with the codec when the codec exists; do not treat this format as the contract.

```bash
c++ -std=c++17 -o /tmp/poctest poc915/test_rak_event.cpp && /tmp/poctest
```

Run that before flashing. A parse bug looks exactly like a dead module: the frames
arrive and nothing prints.

## Board table

Record every board here, as the collar id → USB serial → cat table is for collars.

| C3 | MAC | USB | role |
|----|-----|-----|------|
| ESP32-C3 (QFN32) rev v0.4, 4 MB flash | `80:53:e0:a0:be:30` | native USB, VID `0x303a`, PID `0x1001`, USB-Serial/JTAG → **GPIO20/21 are free** | **node 1**, `-DROLE=2 -DNODE_ID=1` (RA-02 soldered) |
| ESP32-C3 (QFN32) rev v0.4, 4 MB flash | `80:53:e0:a2:f8:50` | same board type | 915 listener, `-DROLE=0`; next: node 2, `-DROLE=2 -DNODE_ID=2` |

**The `usbmodem` port suffix is shuffled between reboots and identifies nothing.**
Both boards have appeared as `/dev/cu.usbmodem111301`. Identify a board by MAC:

```bash
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 115200 read_mac
```

`pyserial` reports the MAC as the port's `serial_number`, so
`serial.tools.list_ports` gives it **without opening the port** — opening it resets the
C3 (`rst:0x15 USB_UART_CHIP_RESET`) and kills whatever was running, which is also why
`tools/poc_watch.py` sets DTR **before** `open()`, not after.

## Bring-up order: the RAK first, then the RA-02

**The RA-02 must not be initialised before the RAK3172 has finished booting.** The
RA-02's cold inrush (100 µF plus the module) lands on the shared 3V3 rail while the
RAK is still in its own power-on, and a RAK that browns out mid-boot comes up **mute**
— not reset-looping, just silent, which is indistinguishable from a dead module. This
cost a session and a soldering session before it was found; the module was never dead.

`setup()` is ordered so the module is on an undisturbed rail when it is first
addressed, and re-checked afterwards so the failure is self-reporting:

```c
bringUp();          // RAK first
delay(500);         // rail recovers before the RA-02 draws
init433();
check433();
if (!atCmd("AT", 1200, 2))
  log("[warn] RAK stopped answering after the RA-02 was initialised\n");
```

**A module can also run phantom-powered through its own ESD diodes.** The SPI/control
inputs (SCK, MOSI, NSS, NRESET) are driven to 3.3 V by the C3 and each has a protection
diode to the chip's VDD rail, so an unpowered SX1278 fed only by signal wires can leak
enough current into VDD to answer SPI reads and decode packets. The fingerprint is VDD
reading **2.0–2.7 V** with the 3V3 wire removed (the missing ~0.7 V is the diode drop).
It is out of spec: the PA is starved and packet-status registers read garbage
(`-289 dBm` was observed). **Do not read a register as proof a module is powered.**

## RAK3172 pads — the 32-pad module (the hardware in this project)

**All modules in this project are the 32-pad RAK3172 stamp** — pad **24 is VDD**,
UART2 is pads **1 and 2**. Proven on board 1 (`80:53:e0:a0:be:30`), which answers
`AT+VER` with pads 1/2 as UART2.

The `RAK3172-SiP` datasheet is a **65-pad package with a different numbering**:
UART2 on pads **29/30**, VDD on pads **3/55**, RF on pad **37**, and pad **24 = PC3
(a GPIO)**. The pin **names** are the same in both tables, the numbers are not. If a
module has pads on **all four edges** it is a SiP; if it has 16 pads on **two** long
edges it is the stamp. **Quote names, not numbers, when checking.**

| Pad | Name | Wire to |
|-----|------|---------|
| **2** | `PA2 / UART2_TX` (TX2) | C3 **GPIO20** (RX) |
| **1** | `PA3 / UART2_RX` (RX2) | C3 **GPIO21** (TX) |
| **24** | **VDD** | 3.3 V (never 5 V, never VIN) |
| 11, 17, 18, 23, 28 | GND | GND |
| **12** | **RF** | antenna, 8.2 cm. `-NI`/`(P)` = no IPEX: coax pigtail, shield to a neighbouring GND pad |
| **21** | **BOOT0** (high active) | must be **LOW**. Floating = STM32 UART bootloader, module answers nothing |
| **22** | **RST** (NRST) | must be **HIGH**. Low = held in reset, answers nothing |

Pad **27 (PB12)** is internally pulled up 10 kΩ on the high-frequency (9xx) variant,
pulled down on the low-frequency one. Side effect of the band, not a fault.

Pads 1 and 2 are the **first two pads counting from the index mark**, on the same
edge, adjacent. Pad 24 (VDD) is on the **far edge** — pad 23 (GND) sits beside it, so
the VDD joint is the one most likely to slide into GND.

**Cross the UART**: C3 **GPIO20 (RX) → pad 2 (module TX)**; C3 **GPIO21 (TX) →
pad 1 (module RX)**.

## Measured on this module — replaces the doc-derived lines

Module label: **RAK3172-9-SM-NI** (9xx = US915, no IPEX, no TCXO). Firmware
`RUI_4.0.6_RAK3172-E` — the `-E` is RAK's **module target** in RUI3
(E / S / T / F), **not the band**. The band is the part number.

| Command | Real reply on this module |
|---------|--------------------------|
| `AT+VER=?` | `AT+VER=RUI_4.0.6_RAK3172-E` |
| `AT+NWM=?` | `AT+NWM=0` (P2P) |
| `AT+P2P=?` | `AT+P2P=915000000:7:0:0:8:14` |
| `AT+PRECV=?` | `P2P_RX_ON` |
| `AT+SN=?` | **empty** on this module |
| `AT+P2P=915000000:7:125:0:8:14` | **`AT_PARAM_ERROR`** — bandwidth is a **code** (0 = 125 kHz, 1 = 250, 2 = 500), not kHz |
| `AT+P2P=…` while RX armed | `P2P_RX_ON already, please disable P2P RX before setting.` + `AT_BUSY_ERROR` |

Three facts that change how the code has to work, all measured here:

1. **Bandwidth is encoded.** `AT+P2P` wants `…:7:0:…` for 125 kHz. Typing `125` is
   rejected, and the readback never shows `125`. Compare readbacks in the module's
   own encoding.
2. **`AT+PRECV` is persistent across reboot.** The module boots **in continuous RX**
   from the previous session, so bring-up must be `AT+PRECV=0` → set P2P → read back
   → arm RX. In the old order the set is silently rejected and the module keeps its
   defaults.
3. **The defaults on this part are already the target settings** (915.000 MHz, SF7,
   125 kHz, CR 4/5, preamble 8, 14 dBm), which is why a rejected set looked
   harmless: the readback matched. **A readback that matches what you want does not
   prove you set it** — that is the trap.

## First hop measured (bench, boards side by side)

`-DROLE=1 -DNODE_ID=1` talker, `-DROLE=0` listener, SF7 / 125 kHz / CR 4/5 /
preamble 8 / 14 dBm, one frame every 8 s:

```
TALKER   [TX] done  seq 4  ok=5
LISTENER [RX] node 1 seq 4  rssi -13 dBm  snr 12 dB  ok=5 miss=0
```

**RSSI −13/−14 dBm is saturated near-field**, not a range number. Field numbers have
to come from the far end of the property, and `tools/tune_antenna.py` logs mean RSSI
per antenna length at a fixed distance.

Bring-up fact worth keeping: **the RAK boots slower than the C3's first probes.** A
short probe prints a false `module not answering on UART2` and silently skips the
P2P setup, while bridge mode answers `AT` in one second. Bring-up now probes ~40 s.


## RSSI register: 0x1A, not 0x1F

Packet RSSI on the SX1278 is **RegPacketRssiValue at 0x1A**. For **433 MHz** the
conversion is **`-164 + signed(value)`** (RadioLib's `-156` is for the >868 MHz
band and reads ~8 dB low here), plus `SNR` when SNR is negative. Reading `0x1F` gives a plausible-looking number that is not
a signal measurement: `0x00` at 0x1A means **no packet**, and every RX detection on
this board read `0x00`, which is what separated "receiver deaf" from "receiver
healthy, antenna path dead".

RadioLib's RX read sequence, in bit-bang form, is what fixes payload alignment:
read `RegFifoRxCurrentAddr` (0x10), write it to `RegFifoAddrPtr` (0x0D), then read the
FIFO. Reading the FIFO without seeding the pointer returned the payload shifted 2-4
bytes, which the frame decoder correctly rejected on the magic byte.

## The two bugs that made the 433 RX look dead

Both were in the firmware, and the earlier conclusion ("antenna-path failure") was
wrong — the collars decode now on the same wiring.

**1. `RegOpMode` mode codes were swapped.** `0x83` is **transmit**, not receive, and
`0x82` is FSTX. The radio therefore sat in TX the entire time; `0x85` (RX continuous)
was never written. Every "detection" the firmware reported was its own **TxDone** flag
being read as a received packet. Correct codes:

| Mode | Value |
|------|-------|
| Sleep | `0x80` |
| Standby | `0x81` |
| TX | `0x83` |
| **RX continuous** | **`0x85`** |

**2. `RegIrqFlags` bits were swapped.** In LoRa mode RxDone is **bit 6 (`0x40`)** and
bit 3 is **TxDone**; bit 4 is ValidHeader and bit 5 is PayloadCrcError. Using `0x08`
as "RxDone" meant receptions were never seen, and treating `0x10` as a timeout
cleared ValidHeader and re-armed RX, aborting each real packet mid-reception.

| Flag | Value |
|------|-------|
| RxTimeout | `0x80` |
| **RxDone** | **`0x40`** |
| PayloadCrcError | `0x20` |
| ValidHeader | `0x10` |
| TxDone | `0x08` |

Clear with `0xFF` (write-1-to-clear), as `collar/collar.ino` does.

**Also fixed:** Packet RSSI is `-164 + signed(0x1A)` for 433 (the `-157` constant is
for >868 MHz), plus SNR when SNR is negative. `-156` reads ~8 dB wrong. And the TX
path must clear **all** flags before transmitting, with a 3 s TxDone budget.

Verified on the bench: two collars decoding with `misaligned=0`, RSSI −88/−92 dBm,
SNR 10/12 dB, battery 88/93 %.

Settings were never the problem: `RegDetectOptimize 0x03`, threshold `0x0A`,
`RegInvertIQ 0x27`, AGC on, LNA boost, SF9 / BW125 / CR4-7 / sync `0x12` /
preamble 8 match the handheld that decodes these collars.
