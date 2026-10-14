# Collar battery life — findings and plan

Analysis of `collar/collar.ino`, `collar/README.md`, `handheld/handheld.ino`
and `CatTrackApp/src/signal.ts`. Constraints from the owner: bigger battery is
off the table; beacon interval of 8 s is acceptable; avoid anything that
affects range.

## Current power budget (~9 mA → ~16 h on the 150 mAh cell)

| Consumer | Current |
|---|---|
| nRF52840 MCU at 64 MHz, active `loop()` + USB CDC | **~5.5–6 mA** (biggest item, and the only *always-on* one) |
| SX1278 TX, 17 dBm (~78 mA during the ~214 ms ping) at 5 s interval | ~3.3 mA average |
| Battery divider (100k + 100k) | 20 µA |
| SX1278 standby | ~1.5 µA |

## Recommendations, ranked by payoff (none touch range)

### 1. Sleep the MCU between pings — saves ~5 mA (the big one)

Today the nRF52 just busy-runs `loop()` at 64 MHz for ~5 s between 214 ms of
TX. Put it in **STOP mode** with an **RTC1 compare interrupt** as wake source
(RTC1/LFCLK keeps running in STOP; peripherals are retained, so USB/SPI/radio
need no re-init — simpler than SYSTEMOFF):

- **The gotcha:** the core's `millis()` is SysTick-based and **freezes in
  sleep**, so the ping scheduler has to move from `millis()` to RTC1 counter
  ticks (LFCLK 32.768 kHz). Battery sampling rides the same wake schedule
  (every 10 min of ticks instead of `millis()`-based).
- Sleep current is ~3–6 µA; the 20 µA divider and USB quiescent (~15 µA) are
  the only things still drawing.
- `v`-command logging still works — USB activity wakes the CPU.
- Fallback if deep sleep turns out fiddly: drop the HF clock 64 → 16 MHz,
  which saves ~2–3 mA in a few lines. The bit-banged SPI timing uses
  DWT-based `delayMicroseconds`, so it is clock-independent.

### 2. Interval 5 s → 8 s — saves ~1.2 mA

One line on the collar: `PING_INTERVAL_MS = 8000` (`collar/collar.ino`).
No range impact — identical packet shape, just lower duty cycle.

The handheld is in continuous RX with a 1 s re-armed window
(`RX_TIMEOUT_MS = 1000`, `handheld/handheld.ino:181`), so it needs **no
receive change**. **One catch:** `STALE_MS = 8000`
(`handheld/handheld.ino:61`) is shorter than the new ping period + airtime +
jitter, so the app will flap its "stale" flag constantly. Bump to ~12 s, and
update the "stale (>8s since last)" wording in `CatTrackApp/src/signal.ts:5`.

### Combined estimate

~2.3 mA average → **~60+ h (2–3 days) instead of 16 h**, roughly 4× with no
air-time change. All figures are datasheet-based estimates; a one-time
multimeter measurement of collar current before/after each change would
confirm.

## Smaller / optional

- **Check the blinking status LED with a multimeter.** Nothing in
  `collar.ino` touches an LED — the app-state blink is hardware- or
  core-driven. If the LED is on a significant fraction of the time at 2 mA,
  that's ~20% of the whole budget. Measure current with and without the LED
  lit; this is the biggest unknown in the budget.
- **TX power 17 → 14 dBm** saves ~0.5 mA but costs 3 dB (~1.4× distance) —
  likely skip given the range preference.
- **Shorter payload** (`CAT-01;bat=NN`, 15 B → ~9 B) saves ~85 ms airtime per
  ping (~0.6 mA at 8 s) and is slightly *more* robust (fewer symbols to
  decode). But it is a protocol change needing the handheld parser matched —
  optional.
- **Unrelated reliability note:** the 3.3 V LDO has almost no headroom over a
  cell sagging toward 3.0 V — near end-of-life the board could reset. Sleep
  mode reduces load/sag, but worth knowing.

## Implementation status

Not yet implemented. Plan is: collar MCU sleep (RTC1 wake) + 8 s ping
interval, plus handheld `STALE_MS` bump to ~12 s and the app wording.
