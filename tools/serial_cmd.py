#!/usr/bin/env python3
"""Send a command to a board and show the reply.

    python3 tools/serial_cmd.py c          clear the log
    python3 tools/serial_cmd.py w          arm walk-test mode
    python3 tools/serial_cmd.py d          dump the log as CSV
    python3 tools/serial_cmd.py x          leave walk-test mode
    python3 tools/serial_cmd.py r          print modem registers
    python3 tools/serial_cmd.py            just watch the output

Commands must NOT contain newlines - the firmware reads one keypress at a time.

DTR: the two boards need OPPOSITE states, and getting it wrong is silent.

  ESP32-C3 handheld  DTR LOW.  Asserting it resets the board, which is the
                     classic esptool behaviour: the board restarts and the
                     phone's BLE connection dies with it.

  nRF52840 collar   DTR HIGH. Its USB CDC delivers NOTHING in either
                     direction while DTR is low - no output, and no keystrokes
                     get in either. The board looks completely dead: no
                     banner, no 'v' response, not even the periodic ping
                     lines. It is not dead, and it is not the firmware's
                     fault; the host just is not passing the bytes.

So the default is chosen from the board's USB vendor id. Override it with
--dtr on / --dtr off if the guess is wrong.
"""
import glob
import sys
import time

try:
    import serial
except ImportError:
    print("pip install pyserial")
    sys.exit(1)

# USB vendor ids of the two boards.
VID_ESPRESSIF = 0x303A   # ESP32-C3 handheld
VID_ADAFRUIT = 0x239A   # nRF52840 collar (and its bootloader)


def vid_of(port):
    """USB vendor id of a serial port, or None if it cannot be read."""
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            if p.device == port:
                return p.vid
    except Exception:
        pass
    return None


def dtr_for(port, override=None):
    """Should DTR be asserted on this port?

    True for the collar, False for the handheld, unless overridden.
    """
    if override is not None:
        return override
    vid = vid_of(port)
    if vid == VID_ADAFRUIT:
        return True
    if vid == VID_ESPRESSIF:
        return False
    # Unknown board: the handheld is the one that breaks when DTR is high,
    # so default to the safe state for it.
    return False


def open_port(port, dtr):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = dtr
    s.rts = False
    return s


def find_port():
    """Find the ESP32-C3 by probing for its banner.

    There are usually two boards plugged in, so guessing by filename is not
    safe. Each candidate is opened and checked for the handheld's banner.
    """
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    for p in ports:
        s = open_port(p, dtr_for(p))
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
    args = sys.argv[1:]
    override = None
    if "--dtr" in args:
        i = args.index("--dtr")
        value = args[i + 1].lower() if i + 1 < len(args) else ""
        override = value in ("on", "true", "1", "high")
        del args[i:i + 2]
    if "-h" in args or "--help" in args:
        print(__doc__)
        return 0

    cmd = args[0] if args else ""
    port = args[1] if len(args) > 1 else find_port()
    if port is None:
        print("No serial port found. Is the board plugged in?")
        return 1

    dtr = dtr_for(port, override)
    print(f"port: {port}  dtr: {'on' if dtr else 'off'}")
    s = open_port(port, dtr)
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