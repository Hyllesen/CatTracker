# Find mode — Phase 0a bench findings

Measured 2026-11-19 on the bench, with `collar_rx_probe/` (nRF52840 + RA-02, id 2)
and `handheld_ble/` (ESP32-C3 + RA-02). This file records what the hardware said,
because several of these facts contradict the plan's assumptions and the plan has
been edited to match.

## 1. The collar can receive. Phase 0a's central risk is retired

The nRF collar had never received anything in this repository: both collar
builds are transmit-only, the RA-02 there has no DIO0 wire, and the register
table did not even define the FIFO pointers. It works:

```
[RX] wait=135.7ms bytes=6 crc=ok rssi=-79.0 snr=11.25 data=C1 9A 01 42 00 00 irq=0x50
```

Polled RX continuous (`RegOpMode = 0x85`), `REG_IRQ_FLAGS` read every ~1 ms over
the bit-banged SPI bus, IRQ `0x50` = RxDone + ValidHeader, payload read from the
FIFO, no DIO0 needed. It decoded a neighbouring collar at -79 dBm with SNR
~11 dB, repeatedly, for a 5 s window (≈4100 polls).

## 2. The handheld's turnaround is ~4 ms, not 30 ms

`setup=` is packet-complete to the `transmit()` call; `to_txdone=` is
packet-complete to end of transmission. Subtracting the command's own airtime
gives the key-up delay:

| command preamble | airtime (measured) | `to_txdone` | key-up |
|---|---|---|---|
| 8 symbols | 143.0 ms | 143.0 ms | ~4 ms |
| 16 symbols | 176.0 ms | 176.0 ms | ~4 ms |
| 32 symbols | 241.5 ms | 242.5 ms | ~4 ms |

`setup` measured 0.81–0.86 ms every time, with BLE connected. The plan's 30 ms
budget was pessimistic by 7x, so no long preamble is needed for timing margin.

It does **not** follow that a 100 ms window is enough - see §8. The turnaround is
only the start of the story: the collar has to stay in receive until the packet
is complete, which is 148 ms after its own TxDone.

## 3. A command preamble longer than 8 symbols is not decoded at all

This was the one real surprise. The same collar, same window, same handheld, at
-28 dBm:

| preamble | result |
|---|---|
| 8 symbols | decoded: `data=C2 02 01 0F 01 00`, crc ok, 3 of 3 |
| 16 symbols | not decoded, 0 of 3 |
| 24 symbols | not decoded, 0 of 3 |
| 32 symbols | not decoded, 0 of 6 across two runs |

The collar's `radioInit()` writes `RegPreambleLsb = 8`, and it only accepts a
transmission whose preamble is 8. The working hypothesis is that in LoRa the
*receiver's* preamble-length register sets where the demodulator expects the sync
word, so a longer preamble never locks. Not proved, but the rule is repeatable
and simple:

**The command preamble must equal the collar's configured preamble (8).** If a
longer preamble is ever wanted — for timing margin on a slower handheld, say —
the collar must write the same value to `REG_PREAMBLE` before arming RX, and the
pair must be re-verified at the bench. Do not assume that a longer preamble is
"safer"; here it was silently fatal, and it looked exactly like "the handheld is
not transmitting".

## 4. `REG_MODEM_STAT` bit 2 is always set in RX, so the extension logic is dead

Every window reported `msfirst=0x04@0ms`, including windows where nothing was
transmitted at all:

```
[RX] none polls=5121 msfirst=0x04@0ms mslast=0x04 irqany=0x00
```

So mask `0x0E` (bit 2 included) is true from the first poll regardless of what
is on the air, and the plan's "extend the window while the modem shows activity"
would extend *every* window to `MAX_CMD_RX_MS` — tripling the receive cost.
Either use bit 3 alone (`0x08`) after verifying it, or drop the extension
entirely. Given finding 2, dropping it is the right answer: a fixed 100 ms
window catches a command whose preamble ends at ~37 ms.

## 5. Two RA-02s on one desk overload each other in one direction

While the handheld transmitted from ~20-30 cm, the collar's instantaneous RSSI
peaked at **-14 dBm** and it decoded nothing; the same command from a metre-plus
away (findings 3's runs) arrived at -28 dBm and decoded. The handheld meanwhile
heard the collar at -35 dBm. So:

- bench tests must not put the two boards nose to nose: keep them more than a
  metre apart, or attenuate. -25 dBm is workable, -14 dBm was not. Use
  `rssiwin`/`peak` in the probe's `s` (listen-only) mode to see the level you
  are testing at;
- a collar that "cannot hear" a transmitter 20 cm away is not evidence of a
  broken transmitter. Check the reported level first.

## 6. Probe keys that earned their place

- `s` — listen-only 30 s window, no ping, **every** packet reported, plus the
  window's peak RSSI. The first version returned after the first packet, so a
  window that opened at 0 s was closed by the first neighbouring collar ping at
  1 s and every later test "proved" the handheld was not radiating. That cost a
  session; the loop now keeps listening to the deadline.
- `V` / `w` — logging on/off, idempotent. A bare `v` toggle silently flipped
  logging *off* between runs and produced three sessions of empty output.
- `l` — a 30 s window after the next ping, so a hand-typed command can land
  inside one.
- `r` — register dump.

## 7. What is still unproven

- Whether the 120 ms + ValidHeader extension receives as reliably as the probe's
  5 s window, and the actual receive-window current (§8).
- Whether `REG_OCP`'s 60 mA trim is clamping the PA below its rated +17 dBm
  (87-120 mA in the datasheet). If the meter reads ~60 mA, raise the trim and
  re-check both current and range.
- Whether the collar decodes the handheld at field range with real antennas —
  needs the field test, not the bench.
- The RA-02's actual TX current at 17 dBm (needs a meter; §9 of the plan still
  assumes ~130 mA, calibrated from the measured 3-day battery life).

## 8. The window has to cover RxDone, not the preamble

The measured command really does finish late:

```
collar [RX] wait=148.4ms bytes=6 crc=ok rssi=-25.0 snr=10.00 data=C2 02 01 0F 01 00 irq=0x50
hand   [auto] id=2 setup=0.85ms to_txdone=144.32ms seq=1 left=19 ok
```

143 ms of air (8-symbol preamble) + ~4 ms key-up + ~1.4 ms of detection = 148 ms.
So a **100 ms window cuts the packet in half** and decodes nothing, no matter how
early the preamble is seen. An earlier revision of the plan (and of these
findings) said 100 ms was "ample" by comparing the window against the preamble
finishing at ~37 ms - the wrong end of the packet.

Two fixes, in order of preference:

1. **LISTEN_MS 120 ms, extended to MAX_CMD_RX_MS 300 ms only while IRQ bit 4
   (`0x10`, ValidHeader) is set.** ValidHeader is raised when a real header has
   demodulated; arithmetic puts it at 85-90 ms after TxDone, inside the 120 ms
   window, and the bench logs show it travelling with RxDone as `irq=0x50`. It
   cannot be raised by an empty channel, so the extension is free in normal use.
2. **A fixed 200 ms window.** Simpler, no extension logic; ~2.2 mAs per ping
   instead of ~1.3, i.e. ~37 uA average at 60 s (~0.9 mAh/day).

Not measured yet: the exact moment ValidHeader appears. The probe could print it
(`vh_ms`) in one line if the margin in option 1 ever needs checking.

## 9. Phase 1 on hardware (two-speed collar, find requests)

Implemented and smoke-tested the same day. Collar 73,160 B flash / 8.8 kB RAM,
handheld 706 kB (53% of the app partition, so the old 99% scare does not apply
to this build).

What worked, first try, once the parser bug was out:

```
hand   [cmd] find id=2 for 1 min - sent in the window after the next ping
hand   [find] sent id=2 START 1/3 ok
collar [find] START for 1 min = 30 pings at 2 s (seq 1)
collar [TX] ping #48 sent (id 2, seq 47, listened, FIND 1min)
hand   [find] acked id=2 active, 1 min left
...
collar [find] duration elapsed, back to slow
collar [TX] ping #77 sent (id 2, seq 76, listened, slow 0)
```

- **The find duration is exactly 30 pings for a 1-minute request** at a 2 s fast
  interval, and the revert needs no command. Pings land at ~2.5 s, not 2.0 s,
  because the period is measured from the end of the previous cycle: 2.274 s
  (2 s + the id offset) plus ~0.18 s of transmit and receive.
- **6 find/stop cycles**: 11 commands sent, 0 TX timeouts, 0 failures, every
  START accepted. Not the full 20 attempts, and not with the phone connected.
- Two things only hardware found:

**The serial parser took one character as the verb.** `find 2 15` was parsed as
verb `f` and argument `ind 2 15`, so `strtol` returned 0 and the command went to
**collar 0** - silently, with a cheerful "sent ... ok". The verb is now a whole
word. Anything that greets a bench operator with "ok" while doing the wrong
thing deserves a test.

**Retrying on three consecutive pings needs a window on every ping.** With
`FIND_LISTEN_EVERY = 5`, three retries can all land outside the one window in
five, and `stop` then reports FAILED 3/3 while the collar keeps shouting. The
default is now 1: listening every 2 s cycle costs 1.3 mAs against ~18 mAs of
transmit, i.e. ~0.26 mAh/day, which is not worth that failure mode.

Still open: 20 attempts with the phone connected, and the real receive-window
current.

## 10. The phone's half, verified without the phone

The app writes three bytes to `8e7a1b22-…` and reads the answer out of the
roster, so the whole path can be driven from the bench:

```
$ python3 tools/ble_find.py --id 2 --minutes 2
char 8e7a1b21-... ['read', 'notify']
char 8e7a1b22-... ['write', 'write-without-response']
writing ['0x2', '0x1', '0x2'] to the find characteristic
t+  5.0s: id=2  -32.0 dBm  age  48s  flags 0x11 req
t+ 20.0s: id=2  -32.0 dBm  age   0s  flags 0x23 active
t+ 22.5s: id=2  -33.0 dBm  age   1s  flags 0x21 active
```

`req` appears immediately (the handheld is holding the request), `active` only
after the collar's next ping - 20 s here, up to a slow interval in general - and
then the collar's age drops to 0-1 s because it is pinging every 2 s.

One wrinkle this exposed, now handled in the app: the roster repeats the flags
of the *last ping received*, so after find mode ends (by duration or by stop)
the roster keeps saying FIND_ACTIVE until the next slow ping arrives, up to a
minute later. The app therefore reads a queued stop as "stopping" ahead of the
collar's stale "active", and a finished countdown as "ending" rather than
"finding · 0 min".
