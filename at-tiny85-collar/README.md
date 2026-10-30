What it does

It uses the same SX1278 register setup as collar.ino: 433 MHz, SF9, BW125, CR 4/7, sync word 0x12.
It sends the same 6-byte frame, so your handheld, app and monitor.py need no protocol changes.
It sleeps in power-down mode on the watchdog timer and reads the battery about every 10 minutes.
Set the collar id with -DCOLLAR_ID=n. Set the ping interval with -DPING_TICKS (1 ≈ 8 s, 8 ≈ 64 s). Set transmit power with -DTX_POWER.
The wiring, build commands and flashing steps are in the header comment. Programming works as described earlier: ISP on PB0/PB1/PB2 and reset, with NSS idling high so the Ra-02 ignores the bus.

Things to know before you flash

The handheld timing is coupled to the collar. COLLAR_PING_MS and STALE_MS in handheld_ble.ino assume an 8 s ping. The ATtiny’s watchdog oscillator is inaccurate, so a nominal 8 s can really be 7–10 s. Measure the real period on the bench, then set the handheld’s COLLAR_PING_MS to the slowest measured value, or the handheld will miss pings.
Homing and battery life pull in opposite directions. At PING_TICKS=1 homing is responsive but the cell lasts only about 4 days. At 64 s it lasts weeks, but the beeps are too slow to walk towards. I’d range-test at 1, then deploy at 8.
Calibrate the battery reading. The internal 1.1 V reference is only accurate to about ±10%. Compare the reported voltage with a multimeter and scale BATT_VREF_MV before trusting the percentage.
A correction to my earlier note: you don’t need the 150 ms settling wait before sampling the battery, because the divider and its 100 nF capacitor stay connected all the time. The code uses a short reference settle and discards the first reading instead.
Power supply: Check the Ra-02’s 140 ms transmit burst on the 3.0 V rail with the 100 µF capacitor fitted. A sagging rail on a small LiPo is the most likely first-power-up problem.
Fuses: the default 1 MHz fuses are fine. Just never disable the reset pin.

Both diagrams use the same ground, so connect the battery minus, the regulator ground, ATtiny pin 4 and the three Ra-02 ground pins together. A few small parts aren’t drawn:

ATtiny decoupling: 100 nF across ATtiny pin 8 and pin 4.
Rail capacitor: 100 µF (or 47 µF ceramic) across the 3.0 V rail and ground, right next to the Ra-02. It supplies the 140 ms transmit burst.
Pull-ups: 10 kΩ from ATtiny pin 1 to the 3.0 V rail, and 10 kΩ from Ra-02 RESET (pin 6) to the 3.0 V rail.
Antenna: about 16.5 cm of thin wire soldered to the Ra-02 ANT pad. Keep it straight and away from the battery.

Reading the pins

ATtiny85: hold the chip with the dot or notch at the top. Pin 1 is top-left, then count down the left side and up the right side.
Ra-02: confirm the pin numbers against the AI-Thinker datasheet before powering it. The pads are 2 mm pitch, so for breadboarding use a 2 mm-to-2.54 mm adapter board.
MCP1700: the pin order differs by package. TO-92 is GND, VIN, VOUT. SOT-23 is GND, VOUT, VIN. Check the datasheet for the one you have.
LiPo plug: many LiPos have reversed polarity on the plug. Check with a multimeter before connecting.

Build order

Program the ATtiny first, on its own, on a breadboard with no Ra-02 attached. Programming runs at 5 V, which will destroy the Ra-02.
Power-only test: wire just the battery and regulator, and measure about 3.0 V at the rail before adding anything else.
Add the ATtiny and its capacitor and pull-up, then the Ra-02 and its rail capacitor, then the signal wires.

Programming with an Arduino Uno as ISP

First upload the ArduinoISP example sketch to the Uno. Then wire it to the bare chip:

Uno ATtiny85 pin
D10 1 (reset)
D11 5 (PB0)
D12 6 (PB1)
D13 7 (PB2)
5V 8 (VCC)
GND 4 (GND)

Put a 10 µF capacitor from the Uno’s RESET to its GND (plus leg to RESET). Then use the compile and upload commands in the sketch header. A new ATtiny85 already has the right fuses, so you don’t need to change them.

Once flashed, move the chip into the collar circuit. There’s no LED on this build, so the check is whether the handheld hears the collar. Power it up with -DDEV_MODE set and watch tools/monitor.py. If nothing shows up, tell me which step you got to and what the multimeter read at the 3.0 V rail, and I’ll help you narrow it down.
