# Cat Tracker — agent notes

## Hardware reference: read this before advising on the collar board

The collar is a **TENSTAR nRF52840 Development Board** (Pro Micro form factor;
branded SAMIROB / FEIYANG on the silkscreen and packaging).

**Primary source — the TENSTAR nRF52840 Development Board User Manual:**
<https://manuals.plus/ae/1005007738886550>

Note: that URL currently returns 403 to scripted fetches. Use a web search for
the title if you need to re-read it, and prefer this file's summary for
routine questions so we are not guessing.

Secondary, for the Nice!Nano/Pro Micro clones these boards are based on:
<https://www.beachyuk.com/blog/connecting-and-testing-promicro-nrf52840-clones>

### Confirmed specs

| Question | Answer |
|----------|--------|
| Cell connection | `+` → `B+`, `−` → `B-` |
| Charge rate | **100 mA** onboard (good for a 150 mAh cell — ~0.67C) |
| Rail | 3.3 V LDO; `VCC` pad reads ~3.3 V |
| Bootloader | **Short `RST` to `GND` twice within 0.5 s** |
| Bootloader appears as | USB mass storage (`NICENANO` on clones; `CURRENT.uf2`, `INDEX.HTM`, `INFO_UF2.TXT`) |
| Flashing | Drag a `.uf2` onto the drive, or `arduino-cli` over USB CDC |
| Power pins | `B-`, `B+`, `RAW`, `GND`, `RST`, `VCC` |
| Bootloader LED tell | **Fades** in bootloader; the application **blinks** |

There is no BOOT button to press on this board — the `RST`-to-`GND` double
short is the way in, and it is why bootloader entry looked unavailable.

### Pin silkscreen

Pads are labelled with **raw nRF port numbers** (`002`, `011`, `029`, `104`…),
not Arduino aliases. `029` means P0.29, `104` means P1.04. The firmware's pin
constants and the pad labels are therefore the same number — what you solder to
is what you write in code.

The manual labels `009`/`010`/`011` as SDA/SCL/INT, while Nice!Nano-based
documentation labels `009`/`010` as D9/D10 and `011`/`104` as D7/D8. Both agree
on the underlying pin number; only the *function name* differs, so trust the
number and ignore the function label.

### Arduino build

- Board: `adafruit:nrf52:feather52840` (the Adafruit core enumerates this clone)
- The collar is flashed over USB CDC with the `nrfutil` upload protocol
- See `collar/README.md` for the complete wiring table and build notes

## Project rules

- **Never enable unconditional `Serial.print` on either board.** Serial is
  native USB CDC; powering from a charger or power bank means nothing drains
  it, so the write blocks and silently stops the board. Logging defaults off and
  is enabled by sending `v`. See `collar/README.md`.
- Match the two firmwares on every radio change. See
  "Air settings (must match the handheld)" in `collar/README.md`.