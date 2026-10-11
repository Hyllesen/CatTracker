#!/usr/bin/env python3
"""Send a command to the handheld and show the reply.

    python3 tools/serial_cmd.py c          clear the log
    python3 tools/serial_cmd.py w          arm walk-test mode
    python3 tools/serial_cmd.py d          dump the log as CSV
    python3 tools/serial_cmd.py x          leave walk-test mode
    python3 tools/serial_cmd.py r          print modem registers
    python3 tools/serial_cmd.py            just watch the output

Commands must NOT contain newlines - the firmware reads one keypress at a time.
DTR/RTS are deasserted so opening the port does not reset the board.
"""
import glob
import sys
import time

try:
    import serial
except ImportError:
    print("pip install pyserial")
    sys.exit(1)


def find_port():
    """Find the ESP32-C3 by probing for its banner.

    There are usually two boards plugged in, so guessing by filename is not
    safe. Each candidate is opened and checked for the handheld's banner.
    """
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    for p in ports:
        s = serial.Serial()
        s.port = p
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
                return p
        except Exception:
            try:
                s.close()
            except Exception:
                pass
    # Fall back to the first port so the user sees the banner either way.
    return ports[0] if ports else None


def main():
    port = sys.argv[2] if len(sys.argv) > 2 else find_port()
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if port is None:
        print("No serial port found. Is the handheld plugged in?")
        return 1

    print(f"port: {port}")
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(2.5)  # let the boot banner go by

    if cmd:
        print(f"sending: {cmd!r}")
        s.write(cmd.encode())
        s.flush()

    start = time.time()
    while time.time() - start < 10:
        try:
            chunk = s.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()
        except serial.SerialTimeoutException:
            pass
        except KeyboardInterrupt:
            break

    s.close()
    print("\n--- done ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())