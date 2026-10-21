#!/usr/bin/env python3
"""Connect to the handheld over BLE and decode the roster it publishes.

    python3 tools/ble_roster.py            read once, then watch 20 s
    python3 tools/ble_roster.py 60         watch for 60 s instead

This is the bench check for the half of the system the phone is not part of:
the handheld decodes a collar's packet, keeps one RSSI history per collar id,
and republishes the lot as a single 20-byte BLE notification. The phone's
decoder is in CatTrackApp/src/signal.ts; this script mirrors it, so anything
that prints a sensible roster here will decode the same way on the phone.

It is worth having as a separate tool because the phone is a poor instrument
for this: an app that shows nothing looks identical to an app that is not
connected, and there is no way to tell "the handheld is not publishing" from
"the phone is not listening". Here you can see the bytes.

Needs bleak (pip install bleak) and the handheld switched on and in range.
"""
import asyncio
import struct
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("pip install bleak")

SERVICE_UUID = "8e7a1b20-3f4d-4c8e-9a1f-2b6d5e4c3a21"
SIGNAL_UUID = "8e7a1b21-3f4d-4c8e-9a1f-2b6d5e4c3a21"
DEVICE_NAME = "CatTracker"

# Must match CatTrackApp/src/signal.ts
ROSTER_VERSION = 0x10
MAX_COLLARS = 3
ENTRY_LEN = 6
FLAG_HAVE, FLAG_FRESH, FLAG_STALE, FLAG_LOW_BATT = 0x01, 0x02, 0x04, 0x08


def decode(data):
    """Decode a roster. Returns a list of collars, or a string saying why not."""
    if not data or len(data) < 2:
        return "too short to be a roster"
    if data[0] != ROSTER_VERSION:
        return f"not a roster (byte 0 is 0x{data[0]:02x})"
    count = data[1]
    if count > MAX_COLLARS:
        return f"implausible collar count {count}"
    if 2 + count * ENTRY_LEN > len(data):
        return f"truncated: {len(data)} bytes for {count} collars"
    if len(data) != 2 + count * ENTRY_LEN:
        return f"{len(data)} bytes for {count} collars - expected {2 + count * ENTRY_LEN}"

    collars = []
    for i in range(count):
        o = 2 + i * ENTRY_LEN
        flags = data[o]
        collars.append({
            "id": i,
            "have": bool(flags & FLAG_HAVE),
            "fresh": bool(flags & FLAG_FRESH),
            "stale": bool(flags & FLAG_STALE),
            "low": bool(flags & FLAG_LOW_BATT),
            "rssi": struct.unpack_from("<h", data, o + 1)[0] / 10,
            "snr": struct.unpack_from("<b", data, o + 3)[0] / 2,
            "age": data[o + 4],
            "batt": data[o + 5],
        })
    return collars


def show(data, label):
    decoded = decode(data)
    print(f"{label}: {len(data)} bytes")
    print("  " + " ".join(f"{b:02x}" for b in data))
    if isinstance(decoded, str):
        print(f"  -> {decoded}")
        return
    for c in decoded:
        if not c["have"]:
            print(f"  id={c['id']}  (never heard)")
            continue
        tags = []
        if c["fresh"]:
            tags.append("FRESH")
        if c["stale"]:
            tags.append("stale")
        if c["low"]:
            tags.append("low battery")
        batt = "unknown" if c["batt"] == 255 else f"{c['batt']}%"
        print(f"  id={c['id']}  rssi {c['rssi']:.1f} dBm  snr {c['snr']:.1f} dB  "
              f"age {c['age']}s  battery {batt}  {' '.join(tags)}")


async def main():
    watch = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

    print(f"scanning for {DEVICE_NAME}...", flush=True)
    devices = await BleakScanner.discover(timeout=8.0)
    device = next((d for d in devices if d.name == DEVICE_NAME), None)
    if device is None:
        print(f"{DEVICE_NAME} not found. Saw: "
              f"{[d.name for d in devices if d.name] or 'nothing with a name'}")
        print("Is the handheld switched on and in range?")
        return 1
    print(f"found {device.name} at {device.address}\n")

    async with BleakClient(device) as client:
        # The read is what the phone does on connect: a snapshot of the whole
        # roster, so a phone that connects late still learns about every
        # collar heard so far.
        try:
            snapshot = await client.read_gatt_char(SIGNAL_UUID)
            show(bytes(snapshot), "READ (the snapshot the app fetches on connect)")
        except Exception as e:
            print(f"read failed: {e}")
        print()

        received = []
        await client.start_notify(SIGNAL_UUID,
                                  lambda sender, data: received.append(bytes(data)))
        print(f"subscribed, watching {watch:.0f} s...", flush=True)
        await asyncio.sleep(watch)
        await client.stop_notify(SIGNAL_UUID)

    print(f"\n{len(received)} notification(s):")
    fresh_count = 0
    for i, data in enumerate(received):
        decoded = decode(data)
        if isinstance(decoded, list):
            fresh = [c["id"] for c in decoded if c["fresh"]]
            fresh_count += len(fresh)
            marker = f"fresh: {fresh}" if fresh else ""
            print(f"  #{i}: {len(data)} bytes  {marker}")
        else:
            print(f"  #{i}: {len(data)} bytes  -> {decoded}")
    print(f"\n{fresh_count} fresh collar(s) seen. A fresh flag means a packet "
          f"arrived;\nthe count over time is the beacon rate.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
