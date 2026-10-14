# Collar battery life — plan to exceed 24 hours

Status: **planned, not implemented.** No source has been changed.

Written 2026-10-14. Revised 2026-10-14 after review — see Revision notes.

## Goal

Get the collar past 24 hours on a 150 mAh LiPo, without losing range.

24 h needs **≤ 5.4 mA** average — 150 mAh × ~0.85 usable capacity (down to a
3.4 V cutoff) ÷ 24 h. The current build draws ~9.4 mA and lasts ~16 h, so this
needs a 1.75× cut.

## Constraints

- **Nothing that could affect range.** This rules out the two biggest levers
  (see Rejected below): dropping TX power to 14 dBm, and raising the data rate
  to SF7.
- An 8 s beacon interval is acceptable if it helps. It is *not* needed to reach
  24 h — see Step 4.
- No current measurement hardware. Validation is by runtime plus the existing
  RSSI rig in `tools/`.
- Larger LiPo cells are already on order, so a bigger-cell workaround is not
  worth designing around now. The firmware work below is independent of cell
  size.

## Where the power goes today

Anchored on the numbers already in `collar/README.md` (150 mAh, ~5 h at a 1 s
interval, ~16 h at 5 s → ~9.4 mA average):

| Item | Current | Duty | Average |
|---|---|---|---|
| SX1278 TX @ 17 dBm | ~87 mA | 196 ms / 5 s = 3.9 % | **3.4 mA** |
| SX1278 parked in standby | 1.6 mA | ~100 % | **1.6 mA** |
| nRF52840 busy-spinning the loop | ~3.5 mA @ 64 MHz | ~100 % | **3.5 mA** |
| SAADC left enabled by `analogRead` | ~0.3 mA | ~100 % | 0.3 mA |
| Battery divider (100 k + 100 k) | 20 µA | ~100 % | 0.02 mA |
| **Total** | | | **~8.8 mA** |

Two things stand out:

- The radio sits in **standby for 96 % of its life**. Sleep mode is 0.2 µA, so
  ~1.6 mA is being spent doing nothing.
- `loop()` never blocks, so the FreeRTOS idle task never runs and the core
  never drops to System ON idle. That is the other ~3.5 mA.

**The standby figure is the single largest uncertainty in this document.** It is
1.6 mA typ / 1.8 mA max (`IDDST`, crystal oscillator enabled) per the
SX1276/77/78/79 datasheet. Two other rows in that datasheet are easy to confuse
with it and are *not* standby:

| Datasheet row | Meaning | Value |
|---|---|---|
| `IDDSL` | **Sleep** mode | 0.2 µA typ, 1 µA max |
| `IDDIDLE` | **Idle** mode, RC oscillator | 1.5 µA |
| `IDDST` | **Standby** mode, crystal enabled | **1.6 mA** typ, 1.8 mA max |

If standby were 1.5 µA there would be nothing to fix on the radio and this plan's
Step 1 would be worthless. It is 1000× that.

Airtime is ~196 ms for the 15-byte `CAT-01;bat=NN` payload at SF9 / BW125:
48.25 symbols (8 preamble + 4.25 sync + 36 payload) × 4.096 ms, where symbol
time is 2⁹/125 kHz.

## Budget

| Step | Δ | Est. total | Est. runtime @ 5 s |
|---|---|---|---|
| Baseline | — | 9.4 mA | 16 h |
| 1. Radio → sleep between pings | −1.6 mA | 7.8 mA | 19 h |
| 2. Core → idle between pings | −3.3 mA | 4.5 mA | 26 h |
| 3. SAADC off between samples | −0.3 mA | 4.2 mA | **~28 h** |
| 4. `PING_INTERVAL_MS` 5 → 8 s | −1.3 mA | 2.9 mA | **~39 h** |

Steps 1–3 clear the 24 h target on their own. Step 4 is optional and is a
single-constant revert if the responsiveness turns out to matter more.

The status LED was the one open unknown in this budget. It is now resolved —
see Step 0 — and there is no LED load in field operation, so the figures above
stand as written.

## Steps

### 0. Status LED — resolved, no action needed

**Answered 2026-10-14 by physical inspection:** there is no LED lit or blinking
on the board. The LED appears only with USB plugged in, plus a brief flash at
the moment battery power is first applied.

That retires the largest unknown in this document, and it disproves the
hypothesis this step was written to test. Nothing in `collar.ino` has ever
touched an LED — `git log -S"LED" -- collar/collar.ino` returns no commits —
and the earlier guess that `variant.cpp:109`'s unconditional
`digitalWrite(PIN_NEOPIXEL_POWER, HIGH)` (index 34 = P1.14) was permanently
energising something on the `114` pad is wrong. If it were, the LED would be on
at all times. P1.14 is either unbonded on this board or gates a load with no
indicator.

The observed behaviour is instead the standard USB-connected indicator. The
core drives `PIN_LED2` (index 4) from the USB stack, so it lights when a host
is attached and gives a brief flash at power-on while that stack initialises.
This also explains the misleading note in `collar/README.md:218` and
`AGENTS.md:29` that a "steadily blinking blue LED is the app" — that was
observed on the bench with a cable attached and written down as though it were
battery operation. Worth correcting in those docs.

### 1. Radio sleeps between pings — `collar/collar.ino`

`sendPacket` leaves the radio in standby on both exit paths
(`collar.ino:363` on success, `collar.ino:370` on timeout) and it stays there
until the next ping.

Write `RegOpMode = 0x80 | 0x00` (LoRa | sleep) on the way out instead, and
treat the wake as "standby + `radioInit()`".

Re-initialising the full modem register set on every wake, rather than trusting
which registers survive sleep mode, is deliberate: it costs ~3 ms and ~25 µC
per ping and it removes any need to know whether `RegFifoAddrPtr`,
`RegIrqFlags` or `RegModemStat` were retained. The values written are exactly
what `radioInit()` writes today, so the air interface is unchanged.

### 2. Core idles between pings — `collar/collar.ino`

The core is already built with tickless idle enabled —
`FreeRTOSConfig.h:52` (`configUSE_TICKLESS_IDLE`) and `:55`
(`configTICK_RATE_HZ 1024`). So there is no need to write `WFE`, use System OFF,
or build an RTC1 timer: the only thing needed is to stop busy-spinning.

`delay()` is `vTaskDelay()` (`delay.c:48`), so it yields to the idle task, which
suppresses ticks and WFEs the core. `millis()` is `tick2ms(xTaskGetTickCount())`
(`delay.c:30`) — FreeRTOS-tick based, so it **stays correct across suppressed
ticks**. No scheduler rewrite is required.

Replace the bare `if (millis() - lastPingMs >= PING_INTERVAL_MS)` test with a
blocking wait that yields for at most ~100 ms at a time and polls
`Serial.available()` between naps, so the `v` command still works over a bench
cable.

Do not reach for System OFF here — see Gotchas.

### 3. SAADC off between samples — `collar/collar.ino`

`analogRead()` leaves the SAADC enabled, which burns a few hundred µA against
production samples that happen every 10 minutes. Set
`NRF_SAADC->ENABLE = 0` after each read and back on before the next one.

### 4. Beacon interval 5 → 8 s — `collar/collar.ino:128`

`PING_INTERVAL_MS = 5000` → `8000`. TX duty goes 3.9 % → 2.45 %.

Optional. See the coupled changes below before doing this.

## Coupled changes that step 4 forces

### Which handheld matters

`tools/build_handheld.sh:5` builds **`handheld_ble/`** by default, "because that
is the one the [app uses]". `handheld/` is the serial-only bench build used by
`tools/log_dbm.py` and `tools/tune_antenna.py`. It has no BLE, and the app never
sees it.

The two have different coupling, and the one that actually breaks the product
is the BLE build:

| File | Line | Current | Change | Severity |
|---|---|---|---|---|
| `handheld_ble/handheld_ble.ino` | 59 | `COLLAR_PING_MS = 5000` | → `8000` | 🔴 **Breaks the link outright** |
| `handheld_ble/handheld_ble.ino` | 60 | `RX_TIMEOUT_MS = COLLAR_PING_MS + 200` | follows automatically | 🔴 follows line 59 |
| `handheld_ble/handheld_ble.ino` | 47 | `SMOOTH_N = 12` | → `4` | 🟡 feel, not function |
| `CatTrackApp/App.tsx` | 134 | `DEFAULT_BEEP_MS = 5000` | → `8000` | 🟡 countdown wrong until 2 beacons land |
| `handheld/handheld.ino` | 61 | `STALE_MS = 8000` | → `16000` | 🟢 bench build only |
| `handheld/handheld.ino` | 60 | `SMOOTH_N = 12` | → `4` | 🟢 bench build only |

**Why `COLLAR_PING_MS` is the critical one.** `handheld_ble.ino:52-56` says it
directly: `RX_TIMEOUT_MS` is derived from the collar's ping interval, and
RadioLib's `receive()` blocks for that long waiting. Leave it at 5200 ms while
the collar pings every 8000 ms and the receiver is asleep for 3.2 s of every
cycle — **roughly half of all pings are dropped**. The source already warns at
line 58: "these two boards are coupled — change one interval, change the other."

This is worse than a stale-flag flicker. An earlier revision of this document
got the severity backwards by targeting `handheld/handheld.ino`'s `STALE_MS`,
which only affects the serial build's own diagnostic line.

`handheld_ble.ino:50 STALE_MS = 15000` needs **no** change — it already tolerates
one missed 8 s ping.

### The responsiveness cost of 8 s

`SMOOTH_N` is the honest cost: you cannot smooth 12 s of history when each
sample is 8 s apart. 4 samples ≈ 32 s is the compromise — snappier than today,
less steady. If that feels wrong, drop Step 4.

## Optional: over-current protection

`collar.ino:273` sets `RegOcp = 0x20 | 3`, which is `Imax = 45 + 5×3 = 60 mA`.
A 17 dBm PA_BOOST transmission draws ~87 mA. Per the SX1276/78 datasheet, the
SX1278 disconnects the PA when `Imax` is exceeded, so the protection may
currently be engaging and the build is paying full price for output that is
being clipped. RadioLib's default is `OcpTrim = 11` → 100 mA.

This cannot lose range — it can only restore the 17 dBm already being asked for.
It could cost up to +0.7 mA, which is affordable at 2.9 mA.

Test protocol, since current cannot be measured: run `tools/log_dbm.py` at a
fixed distance before and after. If the mean RSSI does not move, OCP was not
clipping — revert it and keep the 0.7 mA. If it moves, keep it and record the
new baseline.

## Rejected

These were left out because they trade range or complexity for runtime, which
the brief excludes. Recorded so they are not re-derived later.

**TX power 17 → 14 dBm.** ~87 → ~45 mA during TX, worth ~1.6 mA at a 5 s
interval. Costs 3 dB of range. The antenna work is already calibrated at 3 m, so
this would probably have been free in practice.

**SF9 → SF7.** Airtime drops from ~196 ms to ~60 ms, cutting TX energy 3.3× —
more than the power change buys. Costs 6 dB of sensitivity (−139 → −133 dBm)
and requires matching changes in `collar.ino:116` and `handheld_ble.ino:44`, or
the link breaks.

**Shorter payload (15 B → 9 B).** Revisited and downgraded. At SF9/BW125 with CR
4/7, airtime falls from ~197.6 ms to ~169.0 ms — a saving of **~29 ms**, worth
~0.28 mA at an 8 s interval:

| Payload | Payload symbols | Total symbols | Airtime |
|---|---|---|---|
| 15 B | 8 + ceil(128/36)×7 = 36 | 48.25 | 197.6 ms |
| 9 B | 8 + ceil(80/36)×7 = 29 | 41.25 | 169.0 ms |

Symbol time is 2⁹/125 kHz = **4.096 ms**, not 8.192 ms (that figure is SF10, or
SF9 at BW62.5). Some sources quote 4/7 as a 5.5× multiplier rather than the
Semtech CR+4 = 7 convention and get ~45 ms instead of ~29 ms; either way the
saving is 0.3–0.45 mA, not the 1.2 mA a symbol-time error would suggest. At
SF9 the preamble and sync symbols dominate, so payload length barely moves
airtime. It also needs a matched parser change on the handheld. Not worth it.

Also rejected as not worth the code: gating the battery divider (20 µA, 0.5 %
of budget), and trimming the preamble (8.2 ms of 196 ms, and it touches
receiver sync).

**System OFF is deliberately not part of this plan.** See Gotchas.

## If you want System OFF later

Some review has pushed RTC1 + System OFF with a compare interrupt as the wake
source. It is *not* needed — Step 2 gets the core to System ON idle with
tickless idle already compiled in — but if anyone does take that route, these
are the mistakes worth pre-empting:

- **`millis()` does not freeze in System ON sleep.** It is
  `tick2ms(xTaskGetTickCount())` and keeps advancing. It *does* effectively stop
  in System OFF, which is a full reset. If you only need System ON sleep, the
  scheduler rewrite is unnecessary.
- Clear `EVENTS_COMPARE[0]` **before** arming `CC[0]`, or a compare that already
  fired races the new value.
- `NVIC_EnableIRQ(RTC1_IRQn)` is required; setting `INTENSET` alone is not
  enough to make the interrupt fire.
- Confirm LFCLK is actually running. The Arduino core does not guarantee it for
  RTC1 use on every variant.
- **Do not drop the core to 16 MHz as a fallback.** `delayMicroseconds()` is
  `DWT->CYCCNT / 64` (`delay.h:60`), hardcoded to `VARIANT_MCK = 64000000`. At
  16 MHz every `delayMicroseconds(2)` in the bit-banged SPI returns 4× too
  small, driving SCK toward the SX1278's 10 MHz ceiling. The divisor has to be
  parameterised first.

## Validation

1. Flash the Steps 1–3 build. Run the cell flat on the bench with no USB
   attached and time it. Expect ~28 h at a 5 s interval.
2. If the current build is still flashed somewhere, record a 5 s baseline run
   first so the comparison is against a real number rather than the README's
   16 h.
3. Optionally add Step 4 **together with** the `COLLAR_PING_MS` change, and
   confirm the handheld still hears every ping. Expect ~39 h.
4. Confirm the link still works — the handheld's `'v'` register dump and
   `tools/log_dbm.py` should show no change in RSSI spread or SNR from Steps
   1–3.

Watch for the failure mode recorded in `ec36bff`: at these lower currents a
power bank will switch itself off sooner than ever. Use a wall charger or a
timed USB supply.

**If you ever do put a meter on it, measure in series with the cell — not on
VBUS.** The LED is USB-powered (Step 0), so a USB-attached reading includes the
USB PHY and that LED, neither of which a LiPo run ever sees. VBUS current
overstates field draw, and the gap widens as this plan succeeds.

The weakest numbers in the budget are the ~0.2 mA assumed for the nRF idling
and the 1.6 mA standby figure, neither of which has been measured on this
board. The LED is no longer among them — that one is settled. If the nRF idles
2× worse the estimate lands at ~24 h rather than ~28 h, so treat ~24 h as the
conservative floor of steps 1–3.

## Gotchas

- **Do not use System OFF for this.** `setup()` waits up to 2 s for `Serial` to
  mount (`collar.ino:396-397`), which never happens off-USB. Every System OFF
  boot would therefore cost 2 s of CPU activity — 10 mAh per ping on a 5 s
  cycle. That wait needs shortening to ~200 ms first if deep sleep is ever
  wanted, which would then make Steps 2–3 redundant.
- **The 2 s boot wait is also worth shortening on its own.** It is ~2 s of
  activity every cold start regardless.
- `DEV_MODE` still works — the battery sample interval is independent of the
  ping interval.
- Nothing here needs a change to `tools/`, `tools/analyze_walk.py` or the
  antenna rig.
- `handheld/` and `handheld_ble/` drift from each other. Grep both when touching
  anything ping-interval related.

## Docs to update once implemented

- `collar/README.md`: the air-settings table, the `PING_INTERVAL_MS` paragraph
  (including its stale `RX_TIMEOUT_MS` claim, which describes the *serial*
  handheld's coupling and not the BLE one that matters), the "Power" section
  that currently says the collar draws ~9 mA, and the LED tell at line 218 that
  describes a bench-with-USB observation as if it were field behaviour.
- `AGENTS.md:29`: same LED tell, same correction.
- `collar/collar.ino:127`: the comment above `PING_INTERVAL_MS` cites a 5 s
  interval and a ~16 h runtime.
- `handheld_ble/handheld_ble.ino:47`: the smoothing window comment.
- `handheld_ble/handheld_ble.ino:69`: the wire-format comment already says
  "stale (>8s since last)" while `STALE_MS` is 15000 — stale documentation
  independent of any change here, worth fixing while in the file.
- `CatTrackApp/src/signal.ts:5`: carries the same stale `>8s` claim in the app's
  copy of the wire format.

## Revision notes

Revised after a review round. Four substantive corrections to the first draft:

1. **The coupled-change table targeted the wrong handheld.** It listed
   `handheld/handheld.ino:61 STALE_MS` as the item that breaks, when
   `handheld_ble/handheld_ble.ino:59 COLLAR_PING_MS` is the one that actually
   breaks the link — and breaks it far more hard than a stale-flag flicker.
2. **The LED was missing entirely**, then added as Step 0 and then resolved.
   Physical inspection settled it: no LED in field operation, only with USB
   attached. See Step 0.
3. **The payload option was misjudged.** First draft rejected it on a rough
   airtime estimate; the corrected figure is ~29 ms (~0.28 mA), which confirms
   the rejection but for a defensible reason rather than a wrong one.
4. **The standby figure was fragile.** It has been misquoted as 1.5 µA (the
   datasheet's *Idle* row) more than once during review, which would collapse
   Step 1 to zero value. The datasheet rows are now tabulated side by side so
   the distinction cannot slip again.