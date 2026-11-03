# Cat Tracker remote node — bench findings (915 MHz hop + 433 MHz collar RX)

State at time of writing. Everything below was **measured on hardware**, not inferred
from datasheets, unless it says otherwise.

Hardware: two **ESP32-C3** boards + **RAK3172** modules (915 MHz), plus one board with
an added **RA-02 / SX1278** (433 MHz). The RAK and the RA-02 share the C3's 3V3 rail.

---

## 1. What works

### 915 MHz RAK3172 P2P hop — PROVEN

Both boards, board A = talker (`-DROLE=1 -DNODE_ID=1`), board B = listener
(`-DROLE=0`), SF7 / BW 125 kHz / CR 4-5 / preamble 8 / 14 dBm, one frame every 8 s:

```
TALKER   [TX] done  seq 4  ok=5
LISTENER [RX] node 1 seq 4  rssi -13 dBm  snr 12 dB  ok=5 miss=0
```

RSSI −13/−14 dBm is saturated near-field (boards side by side), not a range number.

### 433 MHz collar RX — PROVEN

On the node board, with real collar boards pinging in the house:

```
[433] cat 1 seq 178  rssi -92 snr 10  batt 93  aligned=1 misaligned=0  pk=1 unk=0
[433] cat 2 seq  50  rssi -88 snr 12  batt 88  aligned=2 misaligned=0  pk=2 unk=0
```

Two collars, `misaligned=0`, plausible RSSI/SNR/battery. So the RA-02 wiring,
soldering, SPI and antenna are all good.

### Both radios working at once — the node is a working node

Board 2 as node 2, the same build as node 1, verified standalone:

```
POC915 ver 2  role NODE  node 2
[mod] AT+VER=RUI_4.0.6_RAK3172-E   readback AT+P2P=915000000:7:0:0:8:14
[433] ver 12   frf 6C4000   mc1 76  mc2 94  sync 12  pa FF  opmode 85
[TX] payload "CAT2 0 c-1 r0"        ← nothing heard yet, so the report says so
[TX] payload "CAT2 1 c0 r-93"
[TX] done  seq 1  ok=2
[433] cat 1 seq 90 rssi -84 snr 8 batt 91  aligned=1 misaligned=0
```

Three collars, `misaligned=0`, RAK answering on the **first** probe with the reordered
bring-up. **No listener is attached in this state** — two nodes talking with nobody
listening is the state where a silent failure is invisible, so keep one board as the
`-DROLE=0` listener until a third board is available.

On board 1 (`80:53:e0:a0:be:30`): RA-02 hears three collars on 433 and the RAK3172
sends a 915 report every 8 s, simultaneously. The "dead RAK3172" was a firmware
boot-order fault, not hardware — see section 4.

---

## 2. Firmware bugs found and fixed (these cost hours — read this section first)

All three are the kind where the failure is silent and looks like dead hardware.

### 2.1 `RegOpMode` mode codes were inverted — the radio was never in RX

| Mode | Correct value | What the buggy code used |
|---|---|---|
| Sleep | `0x80` | |
| Standby | `0x81` | |
| **TX** | **`0x83`** | used as "continuous RX" |
| FSTX | `0x82` | used as "transmit" |
| **RX continuous** | **`0x85`** | never written |

The radio sat in **TX** the whole time. `0x83` is transmit. Every "received packet"
the firmware reported was actually its own **TxDone** flag being misread.

### 2.2 `RegIrqFlags` bits were inverted

In LoRa mode (SX127x), confirmed against RadioLib 7.8.1 `SX127x.h`:

| Flag | Value |
|---|---|
| RxTimeout | `0x80` |
| **RxDone** | **`0x40`** |
| PayloadCrcError | `0x20` |
| ValidHeader | `0x10` |
| TxDone | `0x08` |
| CadDone | `0x04` |

The firmware used `0x08` as "RxDone" (= TxDone, wrong) and treated `0x10`
(= ValidHeader) as a timeout, clearing it and re-arming RX — **aborting every real
packet mid-reception**. Clear flags with `0xFF` (write-1-to-clear), as
`collar/collar.ino` does.

> Wrong bit used as RxDone + wrong mode = a receiver that reports constant "frames"
> made of noise. That is why an earlier diagnosis of "antenna path failure, RSSI
> −156 dBm" was wrong.

### 2.3 Packet RSSI offset and TX timing

- Packet RSSI is `-164 + signed(reg 0x1A)` for 433 MHz (the `-157` constant is for
  > 868 MHz). `-156` reads ~8 dB wrong. Add SNR when SNR is negative.
- Packet SNR is reg **`0x19`**, value = SNR × 4, signed. Reg `0x1A` is packet RSSI.
  Reg `0x1F` is **not** an RSSI register (it is SymbTimeoutLsb) — reading it yields a
  plausible-looking meaningless number.
- Before transmitting, clear **all** IRQ flags with `0xFF`, and allow **3000 ms** for
  TxDone (the frame itself needs ~200 ms at SF9/BW125).
- Reading the RX FIFO requires RadioLib's sequence: read `RegFifoRxCurrentAddr`
  (`0x10`), write it to `RegFifoAddrPtr` (`0x0D`), then read the FIFO. Reading the
  FIFO without seeding the pointer returns the payload shifted **2–4 bytes**, which
  the frame decoder correctly rejects on the magic byte.

### 2.4 Other measured facts

- **The RAK3172 boots slower than the C3's first probes.** A short probe prints a
  false "module not answering" and silently skips setup while the module is fine
  (bridge mode answered `AT` in one second). Bring-up probes for ~40 s.
- **`AT+PRECV` is persistent across reboot** — the module boots in continuous RX.
  Bring-up order must be `PRECV=0` → set `AT+P2P` → readback → arm RX, or `AT+P2P` is
  rejected with `AT_BUSY_ERROR`.
- **Bandwidth in `AT+P2P` is a code**, not kHz: `0` = 125, `1` = 250, `2` = 500.
  Typing `125` is rejected and the module keeps its defaults.
- **A readback matching your target does not prove you set it** — this part's defaults
  already match the target, so a rejected set looks harmless.
- `AT+SN=?` returns **empty** on these modules. Firmware is `RUI_4.0.6_RAK3172-E`
  (`-E` = RAK module target, not the band).
- RAK3172 pad numbering (32-pad stamp module): UART2 = pads **1** (`PA3/UART2_RX`) and
  **2** (`PA2/UART2_TX`), **VDD = pad 24**, GND = 11/17/18/23/28, RF = 12,
  BOOT0 = 21, RST = 22. The **SiP** package is a different 65-pad numbering (UART2
  29/30, VDD 3/55) — quote pad **names**, not numbers.
- **ESP32-C3 hardware SPI failed on the RA-02 pads** (SCK 4, MISO 5, MOSI 6, NSS 7):
  RadioLib `begin()` returned `-2` and every register read `0x00`. Bit-banged SPI on
  the same wires reads the correct registers. So the 433 driver is bit-banged.

---

## 3. Verified hardware register state (RA-02, healthy)

```
[433] ver 12 want 12   frf 6C4000 want 6C4000 (433.000 MHz)
[433] mc1 76 want 76   mc2 94 want 94   sync 12 want 12
[433] inviq 27 want 27  pa FF want FF  flags 00  opmode 85
```

Matching the reference values in `AGENTS.md`: FRF `0x6C4000`, MC1 `0x76`,
MC2 `0x94`, Sync `0x12`, InvIQ `0x27`, PA `0xFF`, RegVersion `0x12`
(**RegVersion is register `0x42`**, not `0x12` — `0x12` is RegIrqFlags).

Pins in use on the node (all disjoint from ESP32-C3 boot straps 2/8/9 and USB 18/19):

| Function | GPIO |
|---|---|
| RA-02 SCK / MISO / MOSI / NSS | 4 / 5 / 6 / 7 |
| RA-02 NRESET | 1 (driven low 2 ms, then released to pull-up — never tied to 3V3) |
| RA-02 DIO0 | 0 |
| RAK UART2 RX (C3 side) | 20 |
| RAK UART2 TX (C3 side) | 21 |
| USB D− / D+ | 18 / 19 |

---

## 4. RESOLVED: the RAK3172 "dead module" was a boot-order fault

### Symptom

Board 1 (`80:53:e0:a0:be:30`) had a working RAK3172 all morning. After the RA-02 was
soldered onto the same board, the RAK stopped answering AT entirely — no reply to
`AT`, `AT+VER=?`, `AT+PRECV=?`, in firmware probe **and** in hand-typed bridge mode,
with `AT+PSEND` failing too. Pad 24 measured 3.3 V, no shorts, UART crossing correct
(module `TX` pad → GPIO20, module `RX` pad → GPIO21).

### Cause

`setup()` initialised the **RA-02 first**, then the RAK. The RA-02's cold inrush
(100 µF plus the module) lands on the shared 3V3 rail **while the RAK3172 is still
booting**, and a RAK that browns out during its own power-on comes up mute — not
reset-looping, just silent, which is indistinguishable from a dead module.

### Fix

Bring the RAK up **before** anything touches the RA-02, then settle, then init the
RA-02, then re-check the module:

```c
bringUp();          // RAK first, on an undisturbed rail
#if ROLE == ROLE_NODE
  delay(500);       // let the rail recover before the RA-02 draws
  init433();
  check433();
  if (!atCmd("AT", 1200, 2))
    log("[warn] RAK stopped answering after the RA-02 was initialised\n");
#endif
```

### Verified after the fix, both radios live at once

```
POC915 ver 2  role NODE  node 1
[mod] AT+VER=RUI_4.0.6_RAK3172-E
[mod] readback AT+P2P=915000000:7:0:0:8:14
[433] ver 12 want 12   frf 6C4000 want 6C4000 (433.000 MHz)
[433] cat 2 seq 67  rssi -88 snr 10  batt 88  aligned=1 misaligned=0  pk=1 unk=0
[433] cat 0 seq 132 rssi -90 snr  9  batt 71  aligned=2 misaligned=0  pk=2 unk=0
[TX] done  seq 0  ok=1
```

Three collars decoding on 433 (`misaligned=0`, `crc_err=0`, `drops=0`) while the RAK
acknowledges every 915 send (`ok=6`). The warning line never fires with a proper 3V3
feed on the RA-02.

### Warning: an SX1278 can run phantom-powered

Mid-investigation the RA-02's 3V3 was disconnected and the module **kept answering**:
six registers still read back the exact values the firmware wrote (`FRF 6C4000`,
`MC1 76`, `MC2 94`, `Sync 12`, `InvIQ 27`, `PA FF`) and collars still decoded.

The mechanism is the **ESD protection diodes on the SPI/control inputs** (SCK, MOSI,
NSS, NRESET are all driven to 3.3 V by the C3) leaking current into the chip's VDD
rail, buffered by the 100 µF. The fingerprint is a VDD of about **2.0–2.7 V** with the
3V3 wire removed — the missing ~0.7 V is the diode drop.

This is out of spec: the PA is starved, the supply is noise, packet-status registers
read garbage (`-289 dBm` was observed), and it back-feeds the C3's pins. Do not take
RSSI or range numbers in that state, and do not diagnose "the module is unpowered"
from a register read alone.

### Diagnostic sequence that located it (worth keeping)

1. **Minimal AT-only sketch** (`poc915/at_probe/`) — no RA-02 code, no retries. It
   produced 26 consecutive `OK`, proving the module and UART were fine and that the
   fault was in the larger firmware.
2. **Reordered bring-up** — RAK first, RA-02 second, with a re-check afterwards. This
   both fixed it and made the failure mode self-reporting.

---

## 5. End-to-end acceptance — **PASSED** (bench, 5 minutes)

Board 1 = node 1 (`-DROLE=2 -DNODE_ID=1`, RA-02 + RAK), board 2 = house listener
(`-DROLE=0`), `python3 tools/poc_watch.py 300`:

```
frames 37  seq 0..36  missed 0
915 hop rssi -25..-23 dBm   snr 12..13 dB
collars seen at the house: [0, 1, 2]  (37 reports carried a cat)
ACCEPT
```

**Every field arrived bit-for-bit identical** — the node's echoed payload and the
house's decoded line were compared per `seq`: 37 matched, 37 identical, 0 differing.

```
node   [TX] payload "CAT1 3 c2 r-90"
house  [RX] node 1 seq 3  cat 2 rssi -90  | 915 rssi -23 dBm snr 12 dB
```

**What this does NOT prove:** the ±3 dB RSSI agreement in `TODO.md` task 6 is node-433
vs house-433, which needs a house 433 receiver (`base3` / `cat-base2`) alongside the
node. The node transmits the RSSI it measured, so the number arriving at the house is
the same integer by construction — matching it proves the codec and the hop, not the
radio calibration. Field RSSI numbers need the far end of the property; −23/−25 dBm
is saturated near-field.

**Tooling note:** `tools/poc_watch.py` had two reporting bugs found by this run. The
`no collar` line crashed the parser (regex group unset, fed to `int()`), and `missed`
was computed as `max(seq)+1 − count`, which counted frames sent **before the listener
was armed** as losses — a healthy 10-frame run read as `MISSED PACKETS`. It now counts
gaps inside the received range.

## 6. 915 payload format (node -> house)

```
CAT<n> <seq> c<id> r<rssi>      e.g.  CAT1 2 c1 r-86
```

- `<n>` node id (`NODE_ID`, baked in at flash time)
- `<seq>` increments every send, so the house can count misses
- `c<id>` which collar this node last heard on 433, **-1 if nothing within 30 s**.
  A stale claim is reported as `c-1` rather than repeating an old RSSI: a relay that
  keeps asserting a cat it stopped hearing looks exactly like a cat standing still.
- `r<rssi>` that collar's RSSI in dBm as the RA-02 reported it

Both ends now print the same string, so one line each is the whole acceptance test:

```
node    [TX] payload "CAT1 2 c1 r-86"
house   [RX] node 1 seq 2  cat 1 rssi -86  | 915 rssi -13 dBm snr 12 dB  ok=3 miss=0
```

Parser (`rak_event.h`) accepts both the relay form and a plain talker frame
(`CAT1 12`), and `poc915/test_rak_event.cpp` covers both on the host:
`c++ -std=c++17 -o /tmp/poctest poc915/test_rak_event.cpp && /tmp/poctest`.

**Known limitation:** one report carries **one** collar — whichever was heard most
recently. With three collars pinging every 8 s, a given cat appears in roughly every
third report. Sending the full roster instead would need a longer payload (RAK3172
P2P allows up to 255 bytes, but air time grows and the roster has to be encoded
compactly, cf. the 20-byte BLE roster cap in `multi-collar.md`).

## 7. Build and flash commands

```bash
# 433 node (RA-02 RX + RAK TX)
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=2 -DNODE_ID=1 -I$PWD/homebase/esphome" \
  --output-dir /tmp/poc_node poc915

# 915 listener
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=0" --output-dir /tmp/poc_rx poc915

# 915 talker
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DROLE=1 -DNODE_ID=1" --output-dir /tmp/poc_tx poc915

# upload (upload does NOT compile — it sends the last build of that sketch)
arduino-cli upload --port /dev/cu.usbmodemXXXX \
  --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" --input-dir /tmp/poc_node
```

Node console keys: `k` register check, `i` full 433 dump, `t` self-test transmit,
`c` roster, `p` RAK readback, `b` bridge (type AT by hand), `r` re-arm RX,
`s` send one now, `d` dump walk log, `x` stop RX.

---

## 8. Board identity (was mislabelled once, so it is written down)

| Board | MAC | Role |
|---|---|---|
| 1 | `80:53:e0:a0:be:30` | **node 1** — RA-02 433 RX + RAK 915 TX (`-DROLE=2 -DNODE_ID=1`) |
| 2 | `80:53:e0:a2:f8:50` | **node 2** — same hardware, `-DROLE=2 -DNODE_ID=2` |

Node ids are baked at compile time. **Two nodes reporting the same `NODE_ID` merge into
one row at the house**, and the ids are not in the module, so the table above is the
record of which board carries which id.

**The `usbmodem` suffix is shuffled between reboots and is NOT a board identifier.**
Both boards have been seen on `/dev/cu.usbmodem111301` at different times, and the
earlier table here claimed `111301` = board 1, which caused a mislabel. Identify a
board by MAC:

```
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 115200 read_mac
```

`pyserial` reports the MAC as the port's `serial_number`, so
`serial.tools.list_ports` gives it without resetting the board (opening the port resets
the C3 — `rst:0x15 USB_UART_CHIP_RESET` — and drops whatever was running). Prefer that
when you only need to know *which* board is plugged in.

## 9. Gotchas that wasted time (do not repeat)

- **Opening the C3 serial port with DTR asserted resets the board.** In pyserial, set
  `dtr = False` **before** `open()`; setting it after is too late. This silently wiped
  a walk-test log.
- **A background reader holding the port** causes "device reports readiness to read
  but returned no data" / "multiple access on port".
- **`Serial.isPlugged()` gates every print.** A board on a power bank prints nothing,
  so field data must be buffered in RAM and dumped later (`d`).
- **The RAK probe blocks ~41 s in `setup()`**, and `bridge()` only runs from
  `loop()`. Keys typed during that window are queued, and the `[at] AT — no reply`
  lines seen during it are the **probe loop**, not answers to typed commands.
- **`arduino-cli upload` does not compile.** `--build-property` is compile-only, so
  each role/id must come from its own `--output-dir`.
