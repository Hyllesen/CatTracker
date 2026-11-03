#!/usr/bin/env python3
"""Log both POC boards for a fixed window and print the summary.

    python3 tools/poc_watch.py 1800        # 30 minutes
    python3 tools/poc_watch.py 300

Opens the two ESP32-C3 ports with **DTR low** (asserting DTR resets the board),
prints `[RX]` lines as they arrive, then reports counts, missed seq, and the RSSI
range. Acceptance for the bench hop: one report per 8 s, **missed 0**.

Ports are picked by USB vendor id 0x303a (Espressif native USB), because the
`usbmodem` suffix is shuffled between reboots. The talker is the port whose console
says `role TALKER`; everything else is the listener.
"""
import re
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pip install pyserial")
    sys.exit(1)

VID_ESPRESSIF = 0x303A
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 1800


def ports():
    # pyserial reports the USB VID/PID of the device behind each port, so the port
    # is chosen by vendor id 0x303a (Espressif) and not by the usbmodem suffix.
    return sorted(p.device for p in serial.tools.list_ports.comports()
                  if p.vid == VID_ESPRESSIF)


def open_port(p):
    s = serial.Serial()
    s.port = p
    s.baudrate = 115200
    s.dsrdtr = False
    s.rtscts = False
    s.dtr = False          # set BEFORE open: asserting DTR on the C3 resets the board
    s.rts = False
    s.open()
    s.reset_input_buffer()
    return s


def classify(s, secs=10):
    """Role by behaviour, not by label: 'p' prints the module readback, and only
    the listener reports continuous RX."""
    s.write(b"p\n")
    t0 = time.time()
    buf = ""
    while time.time() - t0 < secs:
        n = s.in_waiting
        if n:
            buf += s.read(n).decode("utf8", "replace")
            if "P2P_RX_ON" in buf:
                return "LISTENER"
            if "precv" in buf:
                return "TALKER"
        time.sleep(0.05)
    return "LISTENER"


def main():
    ps = ports()
    if len(ps) < 2:
        print(f"need two boards, found {len(ps)}: {ps}")
        sys.exit(1)
    conns = []
    for p in ps:
        s = open_port(p)
        conns.append((classify(s), s))
    log = open("/tmp/poc_hop.log", "w")
    print(f"logging {SECS} s to /tmp/poc_hop.log: {conns}")
    t0 = time.time()
    rx = []
    while time.time() - t0 < SECS:
        for role, s in conns:
            try:
                n = s.in_waiting
                if n:
                    text = s.read(n).decode("utf8", "replace")
                    log.write(text)
                    log.flush()
                    for m in re.finditer(r"\[RX\] node (\d+) seq (\d+)\s+rssi (-?\d+) dBm\s+snr (-?\d+)", text):
                        rx.append((int(m.group(2)), int(m.group(3)), int(m.group(4))))
                        print(f"{int(time.time()-t0):5d} s  seq {m.group(2)}  rssi {m.group(3)}  snr {m.group(4)}")
            except Exception as e:
                print(f"[{role}] read err {e}")
        time.sleep(0.05)
    log.close()
    if not rx:
        print("no frames received")
        return
    seqs = [s for s, _, _ in rx]
    rssi = [r for _, r, _ in rx]
    snr = [n for _, _, n in rx]
    missed = max(seqs) + 1 - len(seqs)
    print(f"\nframes {len(seqs)}  seq {min(seqs)}..{max(seqs)}  missed {missed}")
    print(f"rssi {min(rssi)}..{max(rssi)} dBm   snr {min(snr)}..{max(snr)} dB")
    print("ACCEPT" if missed == 0 else "MISSED PACKETS - check the log")


main()
