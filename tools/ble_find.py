#!/usr/bin/env python3
"""Ask the handheld for find mode over BLE, and watch what the roster says.

    python3 tools/ble_find.py                    find collar 2 for 15 min
    python3 tools/ble_find.py --id 1 --minutes 5 find collar 1 for 5 min
    python3 tools/ble_find.py --stop             stop collar 2
    python3 tools/ble_find.py --watch 120        keep watching for 2 minutes

This is the phone's half of find mode without the phone: it writes the same
three bytes the app writes, to the same characteristic, and then decodes the
roster the same way `tools/ble_roster.py` does. It exists for the same reason
that one does - an app that does nothing looks exactly like an app that never
sent anything, and there is no way to tell the two apart from the screen.

What to expect, and why it is slow:

  * `req` (roster flag 0x10) appears within a second of the write. That is the
    handheld saying "I have a request and I am waiting for that collar's next
    ping".
  * `active` (0x20) appears only after the collar's next ping - which is up to
    one SLOW interval away, a minute by default - because the collar can only
    be commanded inside the short window after its own ping.
  * After that the collar pings every 2 s, so its `age` drops to 0-1 s and the
    stream speeds up.
  * Stop is the same in reverse, but the collar goes back to the slow interval,
    so `active` can stay set for up to a minute after the stop is delivered.

Needs bleak (pip install bleak).
"""
import argparse
import asyncio
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("pip install bleak")

SERVICE_UUID = "8e7a1b20-3f4d-4c8e-9a1f-2b6d5e4c3a21"
SIGNAL_UUID = "8e7a1b21-3f4d-4c8e-9a1f-2b6d5e4c3a21"
FIND_UUID = "8e7a1b22-3f4d-4c8e-9a1f-2b6d5e4c3a21"
DEVICE_NAME = "CatTracker"

ROSTER_VERSION = 0x10
ENTRY_LEN = 6

FLAG_HAVE = 0x01
FLAG_FRESH = 0x02
FLAG_STALE = 0x04
FLAG_LOW_BATT = 0x08
FLAG_FIND_REQ = 0x10
FLAG_FIND_ACTIVE = 0x20

CMD_FIND_START = 0x01
CMD_FIND_STOP = 0x02


def i16(b, o):
    u = b[o] | (b[o + 1] << 8)
    return u - 0x10000 if u >= 0x8000 else u


def decode_roster(b):
    """Same layout as CatTrackApp/src/signal.ts and base's collar_frame.h."""
    if not b or len(b) < 2 or b[0] != ROSTER_VERSION:
        return None
    count = b[1]
    if 2 + count * ENTRY_LEN > len(b):
        return None
    out = []
    for i in range(count):
        o = 2 + i * ENTRY_LEN
        out.append(
            {
                "id": i,
                "flags": b[o],
                "rssi": i16(b, o + 1) / 10,
                "snr": (b[o + 3] - 0x100 if b[o + 3] >= 0x80 else b[o + 3]) / 2,
                "age": b[o + 4],
                "batt": b[o + 5],
            }
        )
    return out


def show(tag, roster):
    if not roster:
        print(f"{tag}: no roster yet")
        return
    for c in roster:
        if not (c["flags"] & FLAG_HAVE):
            continue
        marks = []
        if c["flags"] & FLAG_STALE:
            marks.append("STALE")
        if c["flags"] & FLAG_LOW_BATT:
            marks.append("lowbatt")
        if c["flags"] & FLAG_FIND_REQ:
            marks.append("req")
        if c["flags"] & FLAG_FIND_ACTIVE:
            marks.append("active")
        print(
            f"{tag}: id={c['id']} {c['rssi']:7.1f} dBm  snr {c['snr']:5.1f} dB"
            f"  age {c['age']:3d}s  batt {c['batt']:3d}"
            f"  flags 0x{c['flags']:02x} {' '.join(marks)}"
        )


async def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--id", type=int, default=2, help="collar id (default 2)")
    ap.add_argument("--minutes", type=int, default=15, help="find duration")
    ap.add_argument("--stop", action="store_true", help="stop instead of start")
    ap.add_argument("--watch", type=float, default=90.0, help="seconds to watch")
    args = ap.parse_args()

    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15)
    if not device:
        print(f"no device named {DEVICE_NAME} - is the handheld on?")
        return 1

    roster = {}

    def on_notify(_, data):
        decoded = decode_roster(bytes(data))
        if decoded:
            roster["now"] = decoded

    async with BleakClient(device) as client:
        # Discover first: a characteristic that is missing is the difference
        # between "the app cannot work" and "the app is not trying".
        found = False
        for service in client.services:
            if service.uuid.lower() != SERVICE_UUID:
                continue
            for ch in service.characteristics:
                print(f"char {ch.uuid} {ch.properties}")
                if ch.uuid.lower() == FIND_UUID:
                    found = True
        if not found:
            print("the find characteristic is not on this handheld - reflash it")
            return 1

        await client.start_notify(SIGNAL_UUID, on_notify)
        await asyncio.sleep(1.0)
        show("before", roster.get("now"))

        cmd = CMD_FIND_STOP if args.stop else CMD_FIND_START
        payload = bytearray([args.id & 0xFF, cmd, 0 if args.stop else args.minutes])
        print(f"writing {[hex(x) for x in payload]} to the find characteristic")
        await client.write_gatt_char(FIND_UUID, payload, response=True)

        waited = 0.0
        while waited < args.watch:
            await asyncio.sleep(2.5)
            waited += 2.5
            show(f"t+{waited:5.1f}s", roster.get("now"))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
