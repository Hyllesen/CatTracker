#!/usr/bin/env python3
"""Log signal dBm from the handheld over USB serial, live in the terminal.

Written for antenna testing: the phone is in the way when all you want to do
is swap one antenna and watch the number. This shows a rolling readout and
keeps a CSV you can mark up as you change things.

    python3 tools/log_dbm.py                 log until you quit
    python3 tools/log_dbm.py --duration 60   log for 60 seconds
    python3 tools/log_dbm.py --out a.csv     choose the output file

Keys while running:
    m   mark a waypoint - prints the average since the last mark, which is
        the number you want when comparing one antenna against the next
    r   reset the running window without marking
    q   quit

Output is written in the same column layout tools/analyze_walk.py expects, so
an existing session can be analysed with:

    python3 tools/analyze_walk.py antenna-test-*.csv

Unlike the phone-side recorder, this sees the handheld's unsmoothed RSSI off
the serial line, so the analyzer's "raw noise" figure is a real measurement of
the RF noise floor rather than a restatement of the firmware's averaging
window.

Only opens the handheld's receive port, so there is nothing to transmit into a
missing antenna.
"""
import argparse
import glob
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pip install pyserial")

# [+] pkt #45  RSSI -23.0 dBm  smoothed -23.0 dBm  snr 13.3 dB  signal 100%
PACKET = re.compile(
    r"RSSI\s+(-?[\d.]+)\s*dBm\s+smoothed\s+(-?[\d.]+)\s*dBm\s+snr\s+(-?[\d.]+)\s*dB"
)

NO_DATA = -127.0
FLAG_FRESH = 0x01
FLAG_MARKER = 0x02


def find_port():
    """Locate the ESP32-C3 by probing for its banner.

    Two boards are usually plugged in and the usbmodem suffixes get shuffled
    between reboots, so picking by filename is not safe.
    """
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    for port in ports:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.2
        s.dtr = False
        s.rts = False
        try:
            s.open()
            time.sleep(0.3)
            data = b""
            for _ in range(20):
                data += s.read(512)
                if b"Handheld" in data:
                    break
            s.close()
            if b"Handheld" in data:
                return port
        except Exception:
            try:
                s.close()
            except Exception:
                pass
    return None


class RawMode:
    """Single keypresses without Enter, on POSIX terminals only.

    Degrades to a no-op so the script still runs when stdin is a pipe, where
    the m/r/q keys simply will not arrive.
    """

    def __enter__(self):
        self.saved = None
        try:
            import termios
            import tty

            self.fd = sys.stdin.fileno()
            self.saved = termios.tcgetattr(self.fd)
            tty.setcbreak(self.fd)
        except Exception:
            self.saved = None
        return self

    def __exit__(self, *exc):
        if self.saved is not None:
            try:
                import termios

                termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)
            except Exception:
                pass


def stats(values):
    if not values:
        return None
    return {
        "n": len(values),
        "avg": sum(values) / len(values),
        "min": min(values),
        "max": max(values),
    }


def fmt(s, label=""):
    if not s:
        return f"{label} (no samples)"
    return (
        f"{label} avg {s['avg']:6.1f}  min {s['min']:6.1f}  "
        f"max {s['max']:6.1f}  n={s['n']}"
    )


def bar(percent, width=20):
    filled = int(round((percent / 100.0) * width))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="output CSV path")
    ap.add_argument("--duration", type=float, help="stop after N seconds")
    ap.add_argument("--port", help="serial port (skips auto-detection)")
    args = ap.parse_args()

    port = args.port or find_port()
    if port is None:
        sys.exit("Could not find the handheld. Is it plugged in?")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_path = args.out or f"antenna-test-{stamp}.csv"

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    # Deassert DTR/RTS: asserting either resets the C3 and throws away the
    # smoothed-RSSI history you are trying to compare against.
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(2.5)  # let the boot banner go by

    session = []
    window = []
    marker_no = 0
    start = time.time()
    last_t = 0  # keeps timestamps strictly increasing when several packets
    last_rssi = last_snr = None

    def elapsed_ms():
        """Milliseconds since start, never repeating.

        Packets are often parsed several at a time out of one serial read, so
        two rows would otherwise land on the same millisecond and distort the
        time deltas analyze_walk.py derives duration and rate from.
        """
        nonlocal last_t
        last_t = max(last_t + 1, int((time.time() - start) * 1000))
        return last_t

    with open(out_path, "w") as f:
        f.write(
            "# Antenna test - handheld dBm over serial\n"
            "# rssi_dbm is the handheld's smoothed value; raw carries its\n"
            "#   unsmoothed reading, so analyze_walk.py's noise figure is a\n"
            "#   real measurement of the RF floor.\n"
            "t_ms,rssi_dbm,snr_db,flags,raw\n"
        )

        print(f"port: {port}\nlogging to: {out_path}\n")
        print("  m mark   r reset window   q quit\n")

        with RawMode():
            while True:
                if args.duration and time.time() - start > args.duration:
                    break

                try:
                    key = sys.stdin.read(1)
                except Exception:
                    key = ""

                if key in ("q", "Q", "\x03"):
                    break
                if key in ("m", "M"):
                    marker_no += 1
                    s_now = stats(window)
                    print("\n" + fmt(s_now, f"marker {marker_no}:"))
                    # Pressing m before the first packet lands is easy to do -
                    # the keys line is printed as soon as the port opens.
                    # `s_now and s_now['avg']` short-circuits to None here and
                    # then crashes on the format spec.
                    avg_text = f"{s_now['avg']:.1f}" if s_now else "no-samples"
                    f.write(f"# MARKER {marker_no} {avg_text}\n")
                    f.write(
                        f"{elapsed_ms()},{NO_DATA:.1f},"
                        f"{NO_DATA:.1f},{FLAG_MARKER | FLAG_FRESH},{NO_DATA:.1f}\n"
                    )
                    f.flush()
                    window = []
                elif key in ("r", "R"):
                    window = []
                    print("\nwindow reset")

                chunk = s.read(4096)
                if not chunk:
                    continue
                text = chunk.decode("utf-8", errors="replace")

                for line in text.splitlines():
                    m = PACKET.search(line)
                    if not m:
                        continue
                    raw = float(m.group(1))
                    smoothed = float(m.group(2))
                    snr = float(m.group(3))
                    last_rssi, last_snr = smoothed, snr
                    last_sig = None
                    session.append(smoothed)
                    window.append(smoothed)
                    f.write(
                        f"{elapsed_ms()},{smoothed:.1f},"
                        f"{snr:.1f},{FLAG_FRESH},{raw:.1f}\n"
                    )
                    f.flush()

                    pct = 0 if smoothed <= -75 else min(100, int((smoothed + 75) / 35 * 100))
                    win = stats(window)
                    line_out = (
                        f"  {smoothed:6.1f} dBm  snr {snr:5.1f}  {bar(pct)}  "
                        f"mark#{marker_no} "
                        + (f"n={win['n']:<4d} avg {win['avg']:6.1f}" if win else "")
                    )
                    # Erase to end of line before redrawing. A bare \r leaves
                    # the tail of a longer previous line visible, which smears
                    # successive readings together.
                    sys.stdout.write("\r\033[K" + line_out)
                    sys.stdout.flush()

    s.close()

    print("\n\n" + fmt(stats(session), "session:"))
    print(f"wrote {out_path}")
    print(f"analyse with: python3 tools/analyze_walk.py {out_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()