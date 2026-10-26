# Final plan: collar battery percentage and curve

Status: **final plan. Nothing implemented.** Written 2026-10-25.
Supersedes `deepseek-batt-curve-proposal.md` (same analysis, with two claims
corrected — see §7) and rejects `improved-batt-percentage-accuracy.md` (§8).

Evidence base: `chichi-battery-level-data.csv`, HA history for
`sensor.chichi_chichi_4`, 2026-10-21 19:18 → 2026-10-24 18:45 UTC.

---

## 1. What the data established

Method: drop the 26 `unavailable`/`unknown` rows, collapse consecutive equal
values, cut at the charge event (10-24 16:04 UTC) → **187 value changes over
68.8 h, 65% → 21%**, i.e. **1.56 h per percentage point**.

1. **The readout is not proportional to charge.** It took 47.5 h to go 65 → 55
   and 21.3 h to go 55 → 21. The load is essentially fixed (periodic LoRa burst,
   no accelerometer, no BLE), so elapsed time is a coulomb counter: the curve
   gives 34 of its points to a stretch that consumed 31% of the charge and 10 to
   a stretch that consumed 69% — a **12× misallocation**. This comparison uses
   only time, so it is independent of any voltage-mapping assumption.
2. **The error peaks at 21 points.** Against a constant-rate reference, the
   current table reads **55.9% where the cell holds 34.6%** at 3.806 V. It was
   optimistic by 8–21 points for roughly 30 hours, in the band a cat collar
   lives in.
3. **The readout dithers more than it moves.** RMS residual 1.0 point against a
   6 h median; 81 of 187 transitions went *up* during a discharge; a 12 h trend
   of 0–2 points inside a within-window spread of 3.
4. **The whole observed cliff is ~3 ADC counts wide** (3.826 → 3.806 V, at
   6.5 mV/count). Any corrected curve will step in **6–7 point jumps** in that
   band unless sub-count resolution is recovered first.
5. **Two numbers in the repo are wrong** (see §7): the documented airtime
   (correct at 140 ms) and the implied runtime (~156 h vs a 44 h power budget).

## 2. Three unknowns that gate the work

| Unknown | Why it gates | How it gets answered |
|---|---|---|
| Is the 3.81–3.82 V step the **cell** or the **instrument**? | decides whether a corrected curve is right or actively harmful | Stage 2: mV telemetry vs a multimeter, across a temperature change |
| Is the cell **150 mAh at 2.9 mA**, or ~500 mAh? | 3× error in every runtime and energy figure | cell label, or one series-current measurement |
| What sits **below 21%**? | the 15% alarm is unmeasured | Stage 2: one discharge to the alarm |

Until the first one is answered, **no curve change ships to firmware.**

---

## 3. Stage 0 — Home Assistant, no reflash

**Goal:** a number that behaves, today, without touching a board.

Replace direct use of the battery percentage with a state-of-charge estimator:

- **Anchor.** Any rise of >20 points marks "full" and records the timestamp
  (this log has exactly one: 10-24 16:34 → 18:14).
- **Countdown.** `soc = 100 − rate × hours_since_anchor` with `rate` fitted from
  the last full discharge. Start at **0.64 %/h** (1.56 h/point) from this log.
- **Two-sided correction.** Pull toward the measured percentage at **0.1
  points/hour when voltage is above the estimate, 0.5 points/hour when below.**
  Asymmetric, but *not* one-way: a one-way clamp would let a cold night ratchet
  the estimate down permanently and never recover, which matters because §2's
  first unknown is unresolved.
- **Charging guard.** Freeze the countdown and expose `charging` while
  `derivative > 0`; without it the 21 → 45 → 66 jumps poison any rate fit.

**Prerequisite:** the entity ids are still `sensor.chichi_chichi_4` (and `_2`,
`_3`, `_5`). Remove the orphaned registry entries and restart HA so automations
can reference `sensor.chichi_battery` — see
`docs/plans/battery-life-estimate.md` §Entity naming.

**Acceptance:** replay `chichi-battery-level-data.csv` through the estimator.
Through the 68.8 h window it must stay within ±5 points of the measured value,
must not follow the ±1 dither, and must reset on the 21 → 45 charge. Then leave
it running for a week against the live sensor before Stage 1.

**Touches:** HA only.

## 4. Stage 1 — one firmware drop: send the measurement

**Goal:** publish what the collar measures, stop publishing only the
interpretation.

### 4.1 Frame: 6 → 8 bytes, free

At SF9 / BW125 / CR 4/7, CRC on, explicit header, LDRO off:

```
payloadSymbNb = 8 + ceil((8·PL − 4·36 + 28 + 16) / 36) · 7
PL = 6 → 8 + 2·7 = 22 symbols
PL = 7 → 8 + 2·7 = 22 symbols
PL = 8 → 8 + 2·7 = 22 symbols      ← identical
PL = 9 → 8 + 3·7 = 29 symbols
T = (8 preamble + 4.25 sync + 22 payload) × 4.096 ms = 140.3 ms
```

So 6, 7 and 8-byte payloads all cost 22 symbols and 140 ms. **Two extra bytes are
free on the air and in power.**

```
byte 0   0xC1        magic
byte 1   seq
byte 2   id
byte 3   battery     0..100, 0xFF = unknown        (unchanged, still what the app shows)
byte 4   flags       bit0 LOW_BATT
                     bit1 CHARGING
                     bit2 MV_VALID
                     bit3 TEMP_VALID
byte 5   tempC       int8, degrees C               (~die temperature, see 4.3)
byte 6   batteryMv   uint16 little-endian, millivolts at the cell, 0 = no reading
byte 7
```

**Compatibility:** `handheld_ble.ino:225` gates on `len >= FRAME_LEN`, so an
un-reflashed handheld parses the first six bytes of an 8-byte frame and keeps
working. It will not forward mV or temperature — see §4.4.

### 4.2 Sampling: recover sub-count resolution

`readBatteryPercent()` (`collar/collar.ino:274`) currently takes 6 reads and
integer-divides. Change to:

1. **Discard the first read.** The comment at `:285` claims averaging "cancels"
   the warm-up artefact; it does not — averaging keeps it, biasing every reading
   low by `(x−low)/6`, and a fixed offset is not reduced by averaging. Take 33
   reads, throw the first away, average 32.
2. **Average 32–64 samples in float**, not `uint32_t`. The ±1 LSB dither visible
   at 10-minute spacing proves there is noise to average; the converter's
   reference and quantisation noise is added after the 100 nF reservoir cap, so
   fast samples do decorrelate it even though the node itself has a 5 ms time
   constant (50 kΩ × 100 nF). Expect ~1 mV at the cell.
3. **Keep `NRF_SAADC->ENABLE = 0` after the read** (`:284`) — that is a real
   power fix, do not lose it.
4. Do **not** move to 12-bit: the core runs RESP=Bypass, so it is a 10-bit value
   with a 12-bit label (`:242`).
5. Sampling position is already correct — 8 s after the previous burst and
   before `radioInit()` (`:642` vs `:667`), so there is no transmit sag. Do not
   move it.

### 4.3 Charging and temperature

- **CHARGING:** the nRF52840 detects VBUS in hardware without USB enumeration —
  `NRF_POWER->USBREGSTATUS & VBUSDBETECT_Msk`. That marks the 10-24 charge
  event for the app too, not just for HA.
- **TEMP:** `float readCPUTemperature()` exists in the installed core
  (`adafruit/hardware/nrf52/1.7.0/cores/nRF5/wiring.c:178`), no new dependency.
  It is the **die**, which self-heats 1–3 °C above the board, and its absolute
  accuracy is only a couple of degrees — send it as a covariate for detecting
  the *step*, not as a cell temperature. Sample it in the same 10-minute block
  as the battery.

### 4.4 The roster constraint — decide now

`ROSTER_MAX_LEN = 2 + 3 × 6 = 20`, exactly the default ATT payload, held there
by `static_assert` (`handheld_ble.ino:98`) with `MAX_COLLARS = 3`. Entry:
`flags, rssi(2), snr, age, battery`.

So **mV and temperature reach homebase/HA only; the phone keeps showing byte 3,
i.e. the firmware curve, forever.** If the curve moves to HA (§6), the app is
permanently on the old one. Two ways out, pick one now:

- **A (recommended):** accept it. The app shows the coarse firmware number; HA
  and automations use the good one. Costs nothing, no second four-way change.
- **B:** roster v2 — swap `snr` + `age` (2 bytes) for `mV` (2 bytes), still
  exactly 20. Only worth it once §2's first unknown is settled.

**Acceptance:** `[RAW]` in `-DDEV_MODE` prints mean and spread; 32-sample mean
agrees with a multimeter within 20 mV at the pad; a full 8-byte frame is decoded
by the homebase and appears in HA as an entity; charging flips bit 1 within one
10-minute sample of plugging in.

**Touches:** `collar/collar.ino` (`buildFrame`, `readBatteryPercent`,
`FRAME_LEN`, flags, temp), `handheld_ble/handheld_ble.ino` (parse, ignore extra
bytes), `CatTrackApp` (decoder, unchanged behaviour), `homebase/homebase.ino`
(`applyPacket`, state JSON, HA discovery), `tools/test_monitor.py` (replay the
new bytes — the Report line changes with any frame change).

## 5. Stage 2 — measurement campaign (no code)

Run one collar with mV telemetry, logging `[BATT]`/`[RAW]`/temp to serial and to
HA:

1. **Cold night beside a multimeter.** Board and meter at the `029` pad, one
   room, temperature swinging 10–15 °C. Compare the reported mV to the meter
   across the swing. If the gap moves with temperature, the step is the SAADC
   reference (1% of 4 V = 40 mV) and the curve is innocent.
2. **One discharge from full to the 15% alarm**, undisturbed, no charging. This
   fills the 21% → 0 gap and tests the 15% trip point.
3. **Record the cell.** Label or capacity test. Settles §7.2.

**Acceptance:** a mV series covering 4.1 → 3.5 V, with the temperature gap
quantified, and at least one discharge reaching the alarm.

## 6. Stage 3 — refit the curve, in HA first

Fit the curve from Stage 2's data and implement it as an HA template sensor.
Only mirror it into firmware if §4.4 option A was chosen **and** the app's
number is considered worth a reflash.

Provisional tables from the current log (Stage 2 supersedes both). Top and
bottom anchors are untouched because there is no data for either:

**Conservative — smooths the step over 47 mV instead of 14 mV:**

```c
const BattPoint BATT_CURVE[] = {
  {4200, 100}, {4000, 85}, {3860, 65}, {3813, 41},
  {3710, 25},  {3600, 13}, {3400, 0},
};
```

**Data-derived — reproduces this log exactly, including the suspiciously sharp
step; do not ship without Stage 2:** `{4200,100}, {4000,85}, {3860,65},
{3840,60}, {3827,55}, {3820,50}, {3813,40}, {3807,35}, {3771,30}, {3710,25},
{3673,21}, {3600,13}, {3400,0}`.

The 15% alarm moves from ≈3633 mV to ≈3619 mV — about an hour later at the
current rate. So this is close to a display-only change for the warning, which
is the property to preserve.

**Acceptance:** a replay of the Stage 2 discharge has ≤3-point deviation from a
constant-rate reference (against 21 today) and the alarm still trips with enough
margin to act.

## 7. Corrections to earlier documents

These are recorded so they are not re-derived wrong.

### 7.1 The airtime is 140 ms, not 90 ms

An earlier draft of the proposal claimed the documented 140 ms was wrong. It is
right — `payloadSymbNb` starts at `8 +`, and 22 payload symbols is what 6, 7 and
8 bytes all produce. The README's `2*11` is a mislabel that happens to be
arithmetically correct (8 header + 2×7 = 22). **No change needed to the power
budget for airtime.**

### 7.2 The runtime is inconsistent with the power budget by ≥3×

The collar ran **68.8 h from ~65% to ~21%**. `collar/README.md` §Power says
**2.9 mA average** on a **150 mAh** cell → 0.85 × 150 ÷ 2.9 = **44 h full to
empty**. The observed stretch alone is longer than the documented entire run, so
no choice of percentage anchors can reconcile them. The alternative is also
blocked: 87 mA × 0.2 s ÷ 8 s = **2.2 mA** for the transmit burst alone, so the
~0.8 mA that 156 h on 150 mAh would require is impossible.

A ~500 mAh cell fits almost exactly (0.85 × 500 ÷ 2.9 = 147 h). Until this is
settled, treat 1.56 h/point as measured and every documented run time as
unreliable — including the "~16 h" and "60+ h" figures in `collar/README.md` and
the two batt-life plan documents.

### 7.3 The cliff is 3 counts wide, and that makes averaging mandatory

3.826 → 3.806 V = 20 mV ÷ 6.5 mV/count ≈ 3 counts. With a corrected curve, one
count = 6–7 percentage points there, so 32–64-sample averaging is a precondition
for any curve change, not an optimisation.

### 7.4 Averaging six fast samples *does* help

The 5 ms divider time constant only filters noise on the node. Converter
reference noise is added after the cap, is white, and is what the ±1 LSB dither
at 10-minute spacing demonstrates. Averaging fast reads recovers sub-count
resolution.

## 8. Rejected: `improved-batt-percentage-accuracy.md`

1. Density is not accuracy — `readBatteryPercent()` already interpolates between
   every pair of points; rows on the line change nothing, rows off it are a
   different curve.
2. Its numbers are invented, not measured, and move the wrong way: at 3.80 V it
   says 45% where this log says ~35% and below 3.80 V it *steepens* the band
   (0.30 %/mV) that the data says is the gentlest (0.08–0.15). Applied as
   written it sharpens the cliff it was meant to remove.
3. It targets quantisation below the noise: the readout dithers ±1–2 points.
4. The 50 µs settle delay does nothing against a 5 ms time constant.
5. "Discards the first warm-up sample implicitly via averaging" is backwards —
   averaging keeps it (`collar.ino:285` has the same error).
6. It does not compile as written: it redefines `struct BattPoint` and
   `BATT_CURVE_N`, which exist at `collar.ino:260,265`.
7. "Zero modification to Home Assistant" is false in effect: every battery
   entity drops ~10 points and the alarm moves.

## 9. Deliberately not doing

- No denser `BATT_CURVE` (see §8).
- No curve change in firmware before Stage 2 answers §2's first unknown.
- No monotone latch in firmware. "Only let the percentage fall while
  discharging" puts interpretation back on the board, makes the app's byte 3
  un-undoable, and hides real recovery. Do it in HA, where it can be compared
  against the raw value. The USB half of that idea is kept as the CHARGING flag.
- No 12-bit ADC (it is 10-bit wearing a 12-bit label).
- No change to the sampling position relative to the radio.

## 10. Open decisions

1. **§4.4 A or B** — accept that the app keeps the firmware curve (A), or a
   roster v2 that trades `snr`+`age` for mV (B).
2. **Cell capacity** — label, or a series measurement. Blocks every energy
   claim.
3. **Is the collar worn outdoors?** Decides whether temperature is worth
   modelling at all, and whether §5.1 is the likely explanation for the step.
4. **Is one more full discharge (to the 15% alarm) acceptable before Stage 1
   ships?** It is the only way to fill the 21% → 0 gap.

---

## Appendix — reproducing §1

Parse `last_changed`, drop non-numeric states, collapse consecutive duplicates,
cut at 2026-10-24 16:04Z. Voltage axis = `inverse(BATT_CURVE)(pct)`; reference =
linear in time between the window's own endpoints; smoothing = centred median
(2 h for band tables, 6 h for the 15-minute grid). Nothing is fitted beyond
that, which is why the derived curve's peak slope should be read as "the shape
is certainly different from the table", not "this is the curve".
