# Cat Tracker

A LoRa location system for free-roaming cats: collars transmit pings, a
handheld receives them and tells a phone how strong each cat's signal is. No
internet, no server, no database — the jungle has none of them.

## Language

**Collar**:
A transmitting device on a cat. One-way: it sends pings and never hears. Carries
its own id in every packet.
_Avoid_: Transmitter, tag, beacon (as a noun for the device)

**Collar id**:
The small integer (0, 1, 2) baked into a collar at flash time. It is the index
of the collar's slot in the handheld's state, and the position of its entry in
the roster.
_Avoid_: Address, node number, MAC

**Handheld**:
The receiver a person carries: ESP32-C3 plus a LoRa module. It is the only
device that ever holds collar data, and the only thing the phone talks to.
_Avoid_: Receiver, base station (a base station is a separate, unbuilt thing)

**Beacon**:
One packet heard by the handheld from one collar. A beacon is an event, not a
device. The `FRESH` flag marks the beacon in the roster so a phone counts each
packet once.
_Avoid_: Ping (fine in firmware, not in prose), heartbeat (that is the LED)

**Roster**:
The 20-byte payload the handheld publishes to the phone over BLE: one entry per
collar. Capped at 20 bytes because that is the payload of the default ATT MTU,
so it is deliberately lossy — it carries no packet counts.
_Avoid_: Signal packet, broadcast, state

**Report**:
The richer per-collar view the handheld writes to the USB serial port for a
Host. A superset of the Roster: same numbers, same units, plus the counters the
roster has no room for (packets, missed, unknown-id drops, BLE counters).
_Avoid_: Stats, log, telemetry, dump

**Host**:
A computer on the end of the USB cable, reading the Report. Not a phone, and not
a BLE client.
_Avoid_: Terminal, monitor, client, debugger

**Subscriber**:
A phone subscribed to the roster's BLE notifications.
_Avoid_: Client, peer, listener, Host

**Fresh**:
A collar whose entry was updated by the packet that just arrived.
_Avoid_: New, live, active

**Stale**:
A collar that was heard, and has since gone quiet for longer than one ping
interval plus margin. Stale means "not answering", which is a different problem
from "never heard".
_Avoid_: Lost, offline, missing

**Missed**:
A packet a collar sent that the handheld never heard, counted from gaps in the
collar's `seq` counter. An upper bound on collisions, not a count of them.
_Avoid_: Dropped, lost, collision

**Unknown id**:
A packet naming a collar id this handheld's build does not know. Counted and
dropped, never folded into another collar's row.
_Avoid_: Phantom cat, bad packet
