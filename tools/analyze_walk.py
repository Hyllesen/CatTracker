#!/usr/bin/env python3
"""Summarise a handheld walk-test CSV.

Usage:
    python3 tools/analyze_walk.py walk.csv

Columns (current):
    t_ms,id,rssi_dbm,snr_db,flags,raw

The id column says which collar sent the packet, so a walk with several cats
on it produces one interleaved file that can be split per cat afterwards.
Older files without the id column are still read - everything is then
reported as a single collar.

The layout tools/monitor.py --csv writes is also read:
    host_time,dev_time,id,name,rssi,rssi_raw,snr,battery,packets,missed,age,flags

Two older layouts are also accepted:
    t_ms,rssi_dbm,snr_db,flags,raw        (no collar id)
    t_ms,rssi_dbm,snr_db,marker,fresh,raw (the original handheld dump)
"""
import sys

NO_DATA = -127.0


def load(path):
    """Read a CSV into rows. Returns (rows, has_id)."""
    rows = []
    has_id = None

    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("host_time"):
            # The header tools/monitor.py --csv writes. Trusting it beats
            # guessing: the name column is a string, so a column-count guess
            # would read the name as a number and drop every row.
            has_id = True
            continue
        if line.startswith("t_ms"):
            # The header says which layout this is. Trusting it beats
            # guessing: the current layout and the original handheld dump
            # have the same number of columns and differ only in what the
            # second one holds.
            has_id = ",id," in line
            continue

        parts = line.split(",")
        if len(parts) == 12:
            # tools/monitor.py --csv. The flags here are the roster flags
            # (bit1 FRESH, bit2 STALE), not the walk-test marker bits, and a
            # USB cable has no BOOT button, so there are no waypoints.
            # rssi_raw is the same reading as rssi now that the handheld stops
            # at the latest packet instead of averaging: both columns are the
            # unsmoothed value, and the spread is the RF noise floor rather
            # than a window the firmware chose.
            try:
                flags = int(parts[11])
                rows.append({
                    "t": int(parts[1]),
                    "id": int(parts[2]),
                    "rssi": float(parts[4]),
                    "snr": float(parts[6]),
                    "marker": False,
                    "fresh": bool(flags & 0x02),
                    "error": False,
                    "raw": float(parts[5]),
                })
            except ValueError:
                continue
        elif len(parts) == 6 and (has_id is True or _is_id(parts[1])):
            # t_ms,id,rssi,snr,flags,raw
            try:
                rows.append({
                    "t": int(parts[0]),
                    "id": int(parts[1]),
                    "rssi": float(parts[2]),
                    "snr": float(parts[3]),
                    "marker": bool(int(parts[4]) & 0x02),
                    "fresh": bool(int(parts[4]) & 0x01),
                    "error": bool(int(parts[4]) & 0x08),
                    "raw": float(parts[5]),
                })
            except ValueError:
                continue
        elif len(parts) == 5:
            # t_ms,rssi,snr,flags,raw
            try:
                flags = int(parts[3])
                rows.append({
                    "t": int(parts[0]),
                    "id": 0,
                    "rssi": float(parts[1]),
                    "snr": float(parts[2]),
                    "marker": bool(flags & 0x02),
                    "fresh": bool(flags & 0x01),
                    "error": bool(flags & 0x08),
                    "raw": float(parts[4]),
                })
            except ValueError:
                continue
        elif len(parts) == 6:
            # older layout: t_ms,rssi,snr,marker,fresh,raw
            try:
                rows.append({
                    "t": int(parts[0]),
                    "id": 0,
                    "rssi": float(parts[1]),
                    "snr": float(parts[2]),
                    "marker": parts[3] == "1",
                    "fresh": parts[4] == "1",
                    "error": False,
                    "raw": float(parts[5]),
                })
            except ValueError:
                continue

    return rows, bool(has_id)


def _is_id(text):
    """True when a column looks like a collar id rather than an RSSI."""
    try:
        value = int(text)
    except ValueError:
        return False
    return -1 <= value <= 99


def sparkline(values, width=72):
    if not values:
        return ""
    blocks = " ▁▂▃▄▅▆▇█"
    n = len(values)
    if n <= width:
        shown = values
    else:
        step = n / width
        shown = [values[min(int(i * step), n - 1)] for i in range(width)]
    lo, hi = min(shown), max(shown)
    if hi - lo < 1e-9:
        return blocks[0] * len(shown)
    return "".join(blocks[int((v - lo) / (hi - lo) * (len(blocks) - 1))] for v in shown)


def split_sessions(rows):
    """Timestamps restart on every boot, so a log spanning two sessions goes
    backwards. Split into sessions and report the longest."""
    sessions, cur = [], [rows[0]]
    for prev, r in zip(rows, rows[1:]):
        if r["t"] < prev["t"]:
            sessions.append(cur)
            cur = []
        cur.append(r)
    sessions.append(cur)
    return sessions, max(sessions, key=len)


def report(rows, label):
    """Print the stats for one collar's samples."""
    have = [r for r in rows if r["rssi"] != NO_DATA]
    lost = [r for r in rows if r["rssi"] == NO_DATA]
    fresh = [r for r in rows if r["fresh"]]
    markers = [r for r in rows if r["marker"]]

    t0 = rows[0]["t"]
    span = (rows[-1]["t"] - t0) / 1000.0

    print(f"[{label}]")
    print(f"samples        : {len(rows)}  (fresh packets {len(fresh)}, "
          f"no-data {len(lost)})")
    print(f"duration       : {span:.1f} s")

    if have:
        vals = [r["rssi"] for r in have]
        snrs = [r["snr"] for r in have if r["snr"] != NO_DATA]
        print(f"rssi range     : {min(vals):.1f} .. {max(vals):.1f} dBm "
              f"(avg {sum(vals)/len(vals):.1f})")
        if snrs:
            print(f"snr range      : {min(snrs):.1f} .. {max(snrs):.1f} dB "
                  f"(avg {sum(snrs)/len(snrs):.1f})")
        # A monotonic fall-off is the thing we care about for homing.
        rises = sum(1 for a, b in zip(vals, vals[1:]) if b > a + 0.5)
        print(f"rssi rises     : {rises} of {len(vals)-1} steps "
              f"({100.0*rises/max(1,len(vals)-1):.0f}% - lower is better)")

        # Spread among consecutive raw samples is the multipath noise floor.
        raw = [r["raw"] for r in fresh if r["raw"] != NO_DATA]
        if len(raw) > 3:
            mean = sum(raw) / len(raw)
            var = sum((v - mean) ** 2 for v in raw) / len(raw)
            print(f"raw noise      : +/-{var ** 0.5:.1f} dB stddev over "
                  f"{len(raw)} packets (multipath floor)")
        print()
        print("rssi over time (higher = stronger signal = closer):")
        print("  " + sparkline(vals))
        print(f"  {min(vals):.0f} dBm .. {max(vals):.0f} dBm "
              f"over {span:.0f}s")

    if markers:
        print()
        print("waypoints (BOOT pressed):")
        for m in markers:
            print(f"  t+{(m['t']-t0)/1000.0:7.1f}s")

    if lost:
        print()
        print(f"WARNING: {len(lost)} of {len(rows)} samples had no signal. "
              "Large gaps mean the collar went out of range or stopped pinging.")
    errs = [r for r in rows if r["error"]]
    if errs:
        print()
        print(f"WARNING: {len(errs)} samples recorded a receive error - "
              "check wiring or the radio config.")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    rows, has_id = load(sys.argv[1])
    if not rows:
        print("No samples found.")
        return 1

    sessions, longest = split_sessions(rows)
    if len(longest) > 1:
        rows = longest
    else:
        rows = sessions[0]

    if len(sessions) > 1:
        print(f"note           : log contained {len(sessions)} boot sessions "
              "(board reset mid-log); showing the longest")

    # One block per collar. With a single collar - or a file with no id
    # column - this is the same report it has always been.
    #
    # Marker rows carry id -1: a waypoint belongs to the walk, not to a cat,
    # so it is reported once at the end rather than as a collar of its own.
    markers = [r for r in rows if r["id"] < 0]
    groups = {}
    for r in rows:
        if r["id"] < 0:
            continue
        groups.setdefault(r["id"], []).append(r)

    if has_id and len(groups) > 1:
        print(f"collars in log : {len(groups)}")
        print()
    for collar_id in sorted(groups):
        label = f"collar {collar_id}" if has_id else "collar"
        report(groups[collar_id], label)
        print()

    if markers:
        print("waypoints (BOOT pressed):")
        for m in markers:
            print(f"  t+{(m['t']-rows[0]['t'])/1000.0:7.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
