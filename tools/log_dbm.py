#!/usr/bin/env python3
"""Print the handheld's signal level to the terminal as packets arrive.

    python3 tools/log_dbm.py                 live readout
    python3 tools/log_dbm.py baseline        same, tagged with a config name

The optional label is printed at startup and in the closing summary, so a
series of antenna configurations can be compared without keeping notes.

Ctrl-C to stop. Handy for antenna testing: swap an antenna and watch the
number move. Keep both boards the same distance apart and orientation
between runs, and check the spread - if it climbs past a couple of dB the
setup is drifting and the comparison means nothing.

The port is found by probing for the handheld's banner rather than by
filename, because with two boards plugged in the usbmodem suffixes get
shuffled between reboots.
"""
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pip install pyserial")

# e.g. "[+] pkt #45  RSSI -23.0 dBm  smoothed -23.0 dBm  snr 13.3 dB"
import re

PACKET = re.compile(
    r"smoothed\s+(-?[\d.]+)\s*dBm\s+snr\s+(-?[\d.]+)\s*dB"
)


def find_port():
    """Return the port that actually yields signal data.

    Tries each candidate and keeps the one that produces a packet line, which
    works whether or not the board is freshly booted. Matching on the boot
    banner alone is not enough: DTR is deasserted so opening the port does not
    reset the board, so a running handheld never prints its banner and would
    go undetected.
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
            deadline = time.time() + 2.0
            while time.time() < deadline:
                data += s.read(512)
                if b"Handheld" in data or b"pkt #" in data:
                    break
            s.close()
            if b"Handheld" in data or b"pkt #" in data:
                return port
        except Exception:
            try:
                s.close()
            except Exception:
                pass
    return None


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else None

    port = find_port()
    if port is None:
        sys.exit("Could not find the handheld. Is it plugged in?")

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(2.5)  # let the boot banner go by

    print(f"port {port} - Ctrl-C to stop")
    if label:
        print(f"config: {label}")
    print()

    seen = []
    try:
        while True:
            chunk = s.read(4096)
            if not chunk:
                continue
            for line in chunk.decode("utf-8", errors="replace").splitlines():
                m = PACKET.search(line)
                if not m:
                    continue
                rssi = float(m.group(1))
                snr = float(m.group(2))
                seen.append(rssi)
                print(
                    f"{rssi:6.1f} dBm   snr {snr:5.1f}   "
                    f"avg {sum(seen)/len(seen):6.1f}   "
                    f"spread {max(seen) - min(seen):4.1f}"
                )
    except KeyboardInterrupt:
        pass
    finally:
        s.close()

    if seen:
        print(f"\n{len(seen)} packets   avg {sum(seen)/len(seen):.1f}   "
              f"min {min(seen):.1f}   max {max(seen):.1f}   "
              f"spread {max(seen) - min(seen):.1f}")
        if label:
            print(f"config: {label}")


if __name__ == "__main__":
    main()