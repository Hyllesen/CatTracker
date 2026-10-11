#!/usr/bin/env python3
"""Summarise a handheld walk-test CSV (piped from the board's 'd' command).

Usage:
    python3 tools/analyze_walk.py walk.csv

Columns: t_ms,rssi_dbm,snr_db,marker,fresh
"""
import sys

NO_DATA = -127.0


def load(path):
    rows = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("t_ms"):
            continue
        parts = line.split(",")
        if len(parts) == 5:
            # t_ms,rssi,snr,flags,raw
            try:
                flags = int(parts[3])
                rows.append({
                    "t": int(parts[0]),
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
                    "rssi": float(parts[1]),
                    "snr": float(parts[2]),
                    "marker": parts[3] == "1",
                    "fresh": parts[4] == "1",
                    "error": False,
                    "raw": float(parts[5]),
                })
            except ValueError:
                continue
    return rows


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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    rows = load(sys.argv[1])
    if not rows:
        print("No samples found.")
        return 1

    have = [r for r in rows if r["rssi"] != NO_DATA]
    lost = [r for r in rows if r["rssi"] == NO_DATA]
    fresh = [r for r in rows if r["fresh"]]
    markers = [r for r in rows if r["marker"]]

    t0 = rows[0]["t"]
    # Timestamps restart on every boot, so a log spanning two sessions goes
    # backwards. Split into sessions and report the longest.
    sessions, cur = [], [rows[0]]
    for prev, r in zip(rows, rows[1:]):
        if r["t"] < prev["t"]:
            sessions.append(cur)
            cur = []
        cur.append(r)
    sessions.append(cur)
    longest = max(sessions, key=len)
    rows = longest if len(longest) > 1 else rows
    t0 = rows[0]["t"]
    span = (rows[-1]["t"] - t0) / 1000.0

    print(f"samples        : {len(rows)}  (fresh packets {len(fresh)}, "
          f"no-data {len(lost)})")
    print(f"duration       : {span:.1f} s")
    if len(sessions) > 1:
        print(f"note           : log contained {len(sessions)} boot sessions "
              "(board reset mid-log); showing the longest")
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
    return 0


if __name__ == "__main__":
    sys.exit(main())