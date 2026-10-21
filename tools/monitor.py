#!/usr/bin/env python3
"""Watch the handheld from a laptop: the Report, over the USB cable.

    python3 tools/monitor.py                  live dashboard, Ctrl-C to stop
    python3 tools/monitor.py --csv > walk.csv  raw rows, for analyze_walk.py
    python3 tools/monitor.py --json            one JSON object per report
    python3 tools/monitor.py --duration 120    stop after 120 s
    python3 tools/monitor.py --names cats.json  show cat names
    python3 tools/monitor.py --port /dev/cu.usbmodem111301

WHAT THIS SEES AND THE PHONE DOES NOT

The phone gets the 20-byte roster, which is capped at 20 bytes because that is
the payload of the DEFAULT ATT MTU. The cable has no MTU, so the Report carries
the fields the roster has no room for: packets heard per collar, seq gaps
(packets the collar sent that we never heard), total packets, packets naming an
id this build does not know, and the BLE subscription counters.

So this is a superset of the phone's view, not a copy of it. The numbers the two
do share are the same numbers, in the same scaling (dBm * 10, dB * 2), read from
one snapshot of the collar state - so the terminal and the phone cannot disagree
about what a collar's age was.

THE CABLE AND THE PHONE DO NOT FIGHT

The USB CDC peripheral and the Bluetooth controller are separate hardware, and
the handheld publishes to each independently. Plug the cable in, run this, and
the phone keeps working over BLE. Unplug, and the handheld goes back to
battery-only with nothing else to change: the firmware prints only when a Host
is on the cable, so unplugging stops the output by itself.

DTR IS LOW, ON PURPOSE

Opening the port does not assert DTR. Asserting DTR resets the ESP32-C3, which
restarts the board and kills the phone's BLE connection. The firmware does not
need DTR to know a host is here - it uses the USB SOF watchdog
(Serial.isPlugged()), which is true as soon as the host enumerates the device.
So this script opens the port with DTR low, the board is not reset, and the
stream starts.

Needs pyserial (pip install pyserial).
"""
import argparse
import glob
import json
import os
import re
import shutil
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pip install pyserial")

# USB vendor id of the Espressif USB JTAG/serial debug unit - the handheld.
# The nRF52840 collar is 0x239a, so the two are told apart by the vendor id
# rather than by the usbmodem suffix, which is shuffled between reboots.
VID_ESPRESSIF = 0x303A
VID_ADAFRUIT = 0x239A

# Must match handheld_ble/handheld_ble.ino
REPORT_VERSION = 1
MAX_COLLARS = 3
BATTERY_UNKNOWN = 255
FLAG_HAVE, FLAG_FRESH, FLAG_STALE, FLAG_LOW_BATT = 1, 2, 4, 8

# Must match CatTrackApp/src/signal.ts, so the bar here means the same thing as
# the bar on the phone.
RSSI_CEIL, RSSI_FLOOR = -45.0, -180.0

# Fields per collar, in the order the firmware writes them.
FIELDS = ("r10", "s2", "batt", "packets", "missed", "age", "flags", "r10raw")

ANSI = sys.stdout.isatty() and os.environ.get("TERM", "") != "dumb"


def rssi_to_percent(rssi):
    if rssi <= RSSI_FLOOR:
        return 0
    if rssi >= RSSI_CEIL:
        return 100
    return round((rssi - RSSI_FLOOR) * 100 / (RSSI_CEIL - RSSI_FLOOR))


def default_name(i):
    return f"Cat {i + 1}"


def default_names_path():
    """cats.json in the repo root, if it is there.

    The app keeps its own cats.json on the phone, written by the pencil button,
    and that one cannot be read from a Mac. This file is the terminal's copy of
    the same mapping, so the names have to be typed once in each place - which
    is cheap, and is what makes the terminal readable while walking.
    """
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "cats.json")
    return path if os.path.exists(path) else None


def load_names(path):
    if not path:
        return {}
    try:
        with open(path) as f:
            data = json.load(f)
        return {int(k): str(v) for k, v in data.items() if str(v).strip()}
    except (OSError, ValueError) as e:
        print(f"names file {path}: {e} - using Cat 1, Cat 2, Cat 3", file=sys.stderr)
        return {}


def find_port():
    """The handheld's serial port, by USB vendor id.

    There are usually two boards plugged in - the collar is an nRF52840
    (0x239a) - so the usbmodem suffix is not a reliable way to pick one.
    """
    ports = [p for p in list_ports.comports() if p.vid == VID_ESPRESSIF]
    if ports:
        return ports[0].device
    collars = [p.device for p in list_ports.comports() if p.vid == VID_ADAFRUIT]
    if collars:
        print("only the collar is plugged in (0x239a). Plug the handheld in.",
              file=sys.stderr)
    return None


def parse_report(line):
    """Parse one RPT line. Returns (globals, collars) or raises ValueError."""
    parts = line.split()
    if not parts or parts[0] != "RPT":
        return None
    if len(parts) < 2:
        raise ValueError("no version")
    ver = int(parts[1])
    if ver != REPORT_VERSION:
        raise ValueError(f"report version {ver}, this tool understands {REPORT_VERSION}")

    g = {}
    collars = {}
    for tok in parts[2:]:
        if "=" not in tok:
            raise ValueError(f"stray token {tok!r}")
        key, val = tok.split("=", 1)
        if re.fullmatch(r"c\d+", key):
            nums = val.split("/")
            if len(nums) != len(FIELDS):
                raise ValueError(f"{key}: {len(nums)} fields, expected {len(FIELDS)}")
            collars[int(key[1:])] = {f: int(n) for f, n in zip(FIELDS, nums)}
        else:
            g[key] = int(val)
    return g, collars


class Collar:
    def __init__(self, i):
        self.id = i
        self.flags = 0
        self.have = False
        self.rssi = None
        self.rssi_raw = None
        self.snr = None
        self.batt = None
        self.packets = 0
        self.missed = 0
        self.age = 0
        self.stale = False
        self.low = False
        self.fresh = False
        self.samples = []      # smoothed RSSI, one per report that had a packet
        self.first = None
        self.last = None

    def update(self, c):
        self.flags = flags = c["flags"]
        self.have = bool(flags & FLAG_HAVE)
        self.fresh = bool(flags & FLAG_FRESH)
        self.stale = bool(flags & FLAG_STALE)
        self.low = bool(flags & FLAG_LOW_BATT)
        if not self.have:
            return
        self.rssi = c["r10"] / 10
        self.rssi_raw = c["r10raw"] / 10
        self.snr = c["s2"] / 2
        self.batt = None if c["batt"] == BATTERY_UNKNOWN else c["batt"]
        self.packets = c["packets"]
        self.missed = c["missed"]
        self.age = c["age"]
        self.samples.append(self.rssi)
        if self.first is None:
            self.first = self.rssi
        self.last = self.rssi


def bar(percent, width=14):
    n = max(0, min(width, int(round(percent * width / 100))))
    return "█" * n + "░" * (width - n)


def colour(text, pct):
    if not ANSI:
        return text
    if pct >= 60:
        return "\033[32m" + text + "\033[0m"
    if pct >= 25:
        return "\033[33m" + text + "\033[0m"
    return "\033[31m" + text + "\033[0m"


def render(state, names, args, width):
    """The dashboard block. One line per collar, redrawn in place."""
    lines = []
    g, collars, reports, last_report = state

    port = os.path.basename(args.port)
    head = f" Cat Tracker - handheld monitor   {port}   reports {reports}"
    lines.append(head)

    dev_s = g["t"] / 1000
    since = (time.time() - last_report) if last_report else 0
    lines.append(f" handheld up {int(dev_s) // 60}m{int(dev_s) % 60:02d}s   "
                 f"last report {since:4.1f} s ago   "
                 f"phone connected={g['pc']} subscribed={g['ps']}")
    lines.append(f" packets {g['pk']}   unknown-id drops {g['unk']}   "
                 f"notify ok {g['ok']} fail {g['fail']}")
    lines.append("")

    for i in range(MAX_COLLARS):
        c = collars[i]
        name = names.get(i, default_name(i))
        if not c.have:
            row = f"  {name:<10} -- no packets heard --"
            lines.append(row.ljust(width))
            continue
        pct = rssi_to_percent(c.rssi)
        tags = []
        if c.fresh:
            tags.append("FRESH")
        if c.stale:
            tags.append("STALE")
        if c.low:
            tags.append("LOWBAT")   # short: a long tag pushes the row past 80 columns
        batt = "--" if c.batt is None else f"{c.batt}%"
        avg = sum(c.samples) / len(c.samples)
        # Two lines per cat. Everything on one line runs to ~130 columns and
        # wraps on an 80-column terminal, which turns a dashboard into a scroll
        # of fragments. The counters go on the second line: they change slowly
        # and are read after the signal, not alongside it.
        # Sized to fit 80 columns with the longest tag. Names longer than 10
        # characters push the row over, which is the cost of a long name.
        top = (f"  {name:<10} {c.rssi:7.1f} dBm {bar(pct)} {pct:3d}%  "
               f"snr {c.snr:4.1f}  age {c.age:3d}s  batt {batt:>4}  "
               f"{' '.join(tags)}")
        bot = (f"  {'':<10} raw {c.rssi_raw:7.1f}  avg {avg:7.1f}  "
               f"pk {c.packets:5d}  miss {c.missed:4d}")
        lines.append(colour(top, pct).ljust(width))
        lines.append(bot.ljust(width))

    lines.append("")
    lines.append(" Ctrl-C to stop. --csv / --json for a stream you can pipe.")
    return "\n".join(lines)


def summary(state, names, file=None):
    # Resolved at call time, not at definition time: a default of sys.stdout is
    # bound when the module is imported, so it writes through any later
    # redirection of sys.stdout.
    g, collars, reports, _ = state
    out = file if file is not None else sys.stdout
    print(f"\n{reports} reports   {g['pk']} packets   "
          f"{g['unk']} unknown-id drops   notify ok {g['ok']} fail {g['fail']}",
          file=out)
    for i in range(MAX_COLLARS):
        c = collars[i]
        name = names.get(i, default_name(i))
        if not c.have:
            print(f"  {name}: never heard", file=out)
            continue
        spread = max(c.samples) - min(c.samples)
        print(f"  {name}: {c.packets} packets, {c.missed} missed   "
              f"rssi min {min(c.samples):.1f} max {max(c.samples):.1f} "
              f"avg {sum(c.samples)/len(c.samples):.1f} spread {spread:.1f}   "
              f"last {c.last:.1f} dBm, battery "
              f"{'--' if c.batt is None else str(c.batt) + '%'}", file=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: find the ESP32-C3 by vendor id)")
    ap.add_argument("--csv", action="store_true", help="print raw rows as CSV")
    ap.add_argument("--json", action="store_true", help="print one JSON object per report")
    ap.add_argument("--duration", type=float, help="stop after N seconds")
    ap.add_argument("--names",
                    help="JSON file mapping collar id -> cat name "
                         "(default: cats.json in the repo root, if it exists)")
    ap.add_argument("--no-color", action="store_true")
    args = ap.parse_args()

    global ANSI
    if args.no_color:
        ANSI = False

    port = args.port or find_port()
    if port is None:
        sys.exit("No handheld found. Is it plugged in over USB?")

    # DTR low: asserting it resets the ESP32-C3, which restarts the board and
    # drops the phone's BLE connection. The firmware does not need DTR.
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()

    path = args.names or default_names_path()
    names = load_names(path)
    if path:
        print(f"names from {path}", file=sys.stderr)
    args.port = port

    collars = [Collar(i) for i in range(MAX_COLLARS)]
    state = [None, collars, 0, None]
    start = time.time()
    first_report = None
    csv_header = False
    buf = ""
    warned = False

    if args.csv:
        print("host_time,dev_time,id,name,rssi,rssi_raw,snr,battery,"
              "packets,missed,age,flags")

    try:
        while True:
            if args.duration and time.time() - start > args.duration:
                break

            chunk = s.read(4096)
            if chunk:
                buf += chunk.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line.startswith("RPT"):
                        continue
                    try:
                        g, cs = parse_report(line)
                    except ValueError as e:
                        if not warned:
                            print(f"cannot parse the handheld's output: {e}\n"
                                  f"  line: {line}\n"
                                  "Is the handheld running the build with the Report "
                                  "stream (tools/build_handheld.sh)?", file=sys.stderr)
                            warned = True
                        continue

                    if first_report is None:
                        first_report = time.time()
                    state[0] = g
                    state[2] += 1
                    state[3] = time.time()

                    for i, c in cs.items():
                        if i < MAX_COLLARS:
                            collars[i].update(c)

                    if args.json:
                        print(json.dumps({"host_time": time.time(),
                                          "dev_time_ms": g["t"],
                                          "link": g,
                                          "collars": cs}))
                    elif args.csv:
                        for i in range(MAX_COLLARS):
                            c = collars[i]
                            if not c.have:
                                continue
                            print(f"{time.time():.3f},{g['t']},{i},"
                                  f"\"{names.get(i, default_name(i))}\","
                                  f"{c.rssi:.1f},{c.rssi_raw:.1f},{c.snr:.1f},"
                                  f"{'-' if c.batt is None else c.batt},"
                                  f"{c.packets},{c.missed},{c.age},{c.flags}")
                    elif ANSI:
                        width = shutil.get_terminal_size().columns
                        sys.stdout.write("\033[H\033[J" + render(state, names, args, width))
                        sys.stdout.flush()

            # Redraw on a timer even when nothing arrived, so the ages tick and
            # a silent handheld is visibly silent.
            if not args.csv and not args.json and ANSI and state[0] and time.time() - state[3] > 1.0:
                width = shutil.get_terminal_size().columns
                sys.stdout.write("\033[H\033[J" + render(state, names, args, width))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        s.close()

    if not first_report:
        print(f"\nport {port} opened, nothing received.", file=sys.stderr)
        print("The handheld prints only when a Host is on the cable, so check "
              "the cable is seated and the board is powered. If it is, the "
              "firmware on it is the one without the Report stream: rebuild "
              "with tools/build_handheld.sh.", file=sys.stderr)
        return 1

    # With --csv, stdout is rows only: the blank line and the summary must not
    # land in the file analyze_walk.py reads.
    if not ANSI and not args.csv:
        print()
    summary(state, names, file=sys.stderr if args.csv else sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
