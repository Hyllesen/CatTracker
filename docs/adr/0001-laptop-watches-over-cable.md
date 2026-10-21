# The laptop watches over the cable, not over BLE

Watching the tracker from a Mac means getting the collar state up a cable. We
send a new ASCII stream (`RPT …`) over the handheld's existing USB CDC port,
in parallel with the BLE roster, rather than having the Mac connect to the
handheld as a BLE client. The rejected alternative was extending
`tools/ble_roster.py` into a live view: no reflash, no new wire format. It
loses on three counts — the Mac would take the phone's single BLE client slot,
the field set is stuck at the 20-byte roster (no packet counts, no `missed`,
no unknown-id drops), and a terminal that needs the BLE stack working is useless
in exactly the situation where you want a terminal: a wedged BLE stack.

Two consequences worth recording.

**The cable and the phone do not compete.** The USB CDC peripheral and the
Bluetooth controller are separate hardware, and `publishRoster()` publishes to
each independently, so plugging in does not take the phone offline. Unplugging
needs no switch: the firmware prints only when a Host is on the cable, so
removing the cable stops the output by itself and the handheld goes back to
battery-only.

**Every print in the handheld is gated on `Serial.isPlugged()`, not on DTR.**
The long-standing rule "never print unconditionally, because a board on a
charger wedges when its CDC buffer fills" was a habit; gating makes it
structural. `isPlugged()` is driven by the USB SOF watchdog, which is why it is
usable at all: the handheld must run with DTR **low**, since asserting DTR
resets the ESP32-C3 and kills the phone's connection, so a DTR-based "a host is
here" signal is not available. `Serial.setTxTimeoutMs(0)` covers the other half
— a Host that opens the port and stops reading.

The cost is a fourth copy of the collar state (collar frame, roster, app
decoder, Report) and a reflash. The Report is the only copy with room for the
counters, because a cable has no MTU. It must stay in step with
`tools/monitor.py`; `tools/test_monitor.py` replays the firmware's exact bytes
through the parser so the two cannot drift silently.
