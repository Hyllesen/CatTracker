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
                    # Two listener lines, same fields. The node report carries the collar
                    # it heard ("cat 1 rssi -86"), a plain talker frame carries no cat.
                    for m in re.finditer(
                            r"\[RX\] node (\d+) seq (\d+)\s+(?:cat (-?\d+) rssi (-?\d+)|no collar)\s*\|\s*915 rssi (-?\d+) dBm snr (-?\d+)",
                            text):
                        # "no collar" leaves groups 3 and 4 unset, so they must not be
                        # fed to int() unconditionally.
                        seq, r, snr = int(m.group(2)), int(m.group(5)), int(m.group(6))
                        cat = int(m.group(3)) if m.group(3) is not None else None
                        catr = m.group(4)
                        rx.append((seq, r, snr, cat, catr))
                        tag = f"cat {cat} rssi {catr}" if cat is not None else "no collar"
                        print(f"{int(time.time()-t0):5d} s  seq {seq}  {tag}  915 rssi {r} snr {snr}")
            except Exception as e:
                print(f"[{role}] read err {e}")
        time.sleep(0.05)
    log.close()
    if not rx:
        print("no frames received")
        return
    seqs = [s for s, *_ in rx]
    rssi = [r for _, r, *_ in rx]
    snr = [n for _, _, n, *_ in rx]
    cats = [c for *_, c, _ in rx if c is not None]
    # missed counts gaps *inside* the received range: seqs received after the listener
    # was armed. The first frames sent before arming are not losses, and counting them
    # as one made a healthy 10-frame run read as "MISSED PACKETS".
    missed = (max(seqs) - min(seqs) + 1) - len(set(seqs))
    print(f"\nframes {len(seqs)}  seq {min(seqs)}..{max(seqs)}  missed {missed}")
    print(f"915 hop rssi {min(rssi)}..{max(rssi)} dBm   snr {min(snr)}..{max(snr)} dB")
    print(f"collars seen at the house: {sorted(set(cats))}  ({len(cats)} reports carried a cat)")
    print("ACCEPT" if missed == 0 else "MISSED PACKETS - check the log")


main()
