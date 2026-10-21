# Estimating time-to-low-battery from the drain rate

Status: **not started.** Write this when all three collars are in hand and the
base station has a few days of history. Nothing here is decided; the plan is the
analysis, so the decision can be made from data rather than from a guess.

## The question

Home Assistant reports each collar's battery as a percentage. Can we turn that
into "Chichi has 11 days left"?

Short answer: yes, but not by fitting the percentage. Fit the voltage.

## Why fitting the percentage is wrong

The collar does not measure charge. It measures cell voltage and looks the
percentage up in a table (`collar/collar.ino`, `BATT_CURVE`):

```
{4200, 100}, {4000, 85}, {3800, 55}, {3700, 25}, {3600, 10}, {3400, 0}
```

A LiPo under a steady load loses voltage roughly linearly, so `dV/dt` is close
to constant. Percentage per millivolt is not:

| Range | % per mV |
| --- | --- |
| 4200 → 4000 | 0.075 |
| 4000 → 3800 | 0.15 |
| 3800 → 3700 | **0.30** |
| 3700 → 3600 | 0.15 |
| 3600 → 3400 | 0.05 |

That is a **6× spread**. A collar draining at a constant 1 mV/hour loses
percentage 6× faster in the 3.8–3.7 V band than at 4.2–4.0 V. A straight-line
fit on `%` is therefore only valid locally:

- in the top plateau it **under-predicts** the remaining time, because `%` is
  falling slowly there and the fit extrapolates that slow rate forever;
- through the steep middle it **over-predicts**, because the rate is about to
  triple;
- and extrapolating to 0% is worse still, because below 3.6 V the cell falls off
  a cliff. The last 10% is not the same shape as the first 90%.

**Fit voltage, and predict the knee.** Report "days to 10%" (3.60 V), not "days
to 0%". 10% is the point where the collar already sets `FLAG_LOW_BATT` (`<15%`),
so it is the point where a warning is actionable.

## The data we have, and the data we need

`batteryMv` is computed in `readBatteryPercent()` and **never transmitted**. The
6-byte frame carries only the percentage:

```
byte 0   0xC1        magic
byte 1   seq
byte 2   id
byte 3   battery     0..100, 0xFF = no reading
byte 4   flags       bit0 = LOW_BATT
byte 5   reserved   <- free
```

Byte 5 is unused, and `mV / 10` fits in a byte (340–420 mV → 34–42). Carrying
it costs one byte and the frame stays 6 bytes. The README notes 5 and 6 bytes
cost identical airtime at SF9/BW125, so it is free on the air and free in power.

Per the project rules, a frame change has to land in **four** places at once:

1. `collar/collar.ino` — `buildFrame()`
2. `handheld_ble/` — the parser
3. `CatTrackApp` — the roster decoder
4. `homebase/homebase.ino` — `applyPacket()` and the state JSON

A mismatch is silent: the frame carries the collar id, so a crossed field merges
two cats into one row or hides one. `tools/test_monitor.py` replays the
firmware's exact bytes through the parser, so it has to be updated too.

## Sampling rate

- Battery is sampled every **10 min** in production (`BATT_SAMPLE_MS = 600000`),
  every 5 s in `-DDEV_MODE`.
- The collar pings every **8 s** (`COLLAR_PING_MS = 8000 + (MAX_COLLARS-1)*137`).

So Home Assistant sees the same battery value repeated ~75 times before it
changes. A derivative over a short window is a staircase. Fit over **2–3 days**.

## What will bite

- **Charging.** On a charger the rate goes negative. Guard `rate <= 0` and report
  "charging", not a number.
- **Load is not constant.** A collar with a live BLE connection (handheld or
  phone nearby) drains faster than one idle in the jungle, and LoRa transmit
  bursts sag the voltage. The fitted rate is the rate for whatever the collar
  happened to be doing during the window.
- **Cell age and temperature.** The curve is a nominal open-circuit curve for a
  healthy cell. An aged cell sags more under the same load and reads lower. The
  comment in `collar.ino` says the sag is "the safe direction — it warns early
  rather than late", which is true for a warning and false for a prediction.

## Two options

**Option A — no firmware change.** Home Assistant `derivative` platform on
`sensor.chichi_battery` gives %/day. A template sensor computes
`(pct - 10) / rate` in hours, guarded for charging.

- Pro: no reflash, no four-way frame change, testable today.
- Con: inherits the 6× slope error. Good in the middle band, bad at the ends.

**Option B — publish mV in byte 5.** Fit `dV/dt`, predict the 3.60 V crossing.

- Pro: the fit is on the quantity that is actually linear.
- Con: four-way frame change, and every board has to be reflashed together.

**Recommendation: do A first, measure the error against the table above, then
decide.** If the % fit tracks within a day over a full discharge, A is enough.
If it drifts at the ends, do B.

## Entity naming, before any of this

The battery entities are currently `sensor.chichi_chichi_4` (and `_2`, `_3`,
`_5`), because the discovery records used the cat name as the entity name. The
friendly names are fixed (`Chichi battery`), but Home Assistant does not rename
an entity when a discovery name changes, so the ids stay. Automations reference
ids, so remove the registry entries and restart Home Assistant to get
`sensor.chichi_battery` before writing any alert.

## Acceptance test

Put a collar on a known discharge (not charging), run it for 3 days, and compare
the prediction against the point where it actually hits 10%. A prediction that
is within 20% of the true remaining time is worth automating. One that is not is
a curiosity, and the honest answer is to report 10% as a binary sensor and let
the human do the arithmetic.
