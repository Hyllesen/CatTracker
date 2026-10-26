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

The port is found by USB vendor id, not by filename: with two boards plugged in
the usbmodem suffixes are shuffled between reboots, and the handheld's banner is
printed only when a Host is on the cable, so there is nothing to probe for.

Note this tool reads the human-readable 'v' output, not the RPT stream - the
RPT stream is what tools/monitor.py reads, and it carries more.
"""
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pip install pyserial")

VID_ESPRESSIF = 0x303A   # the ESP32-C3 handheld

# e.g. "[+] pkt #45  RSSI -23.0 dBm  snr 13.3 dB"
import re

PACKET = re.compile(
    r"RSSI\s+(-?[\d.]+)\s*dBm\s+snr\s+(-?[\d.]+)\s*dB"
)


def find_port():
    """The handheld's port, by USB vendor id.

    Probing for the banner is no longer possible: the handheld prints only when
    a Host is on the cable, and the OS enumerates the device at plug-in time,
    so the banner is written to a port with nobody reading it and is dropped.
    The vendor id is deterministic - the collar is an nRF52840 (0x239a), the
    handheld is an Espressif USB JTAG unit (0x303a) - and it does not need the
    board to say anything at all.
    """
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == VID_ESPRESSIF:
            return p.device
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