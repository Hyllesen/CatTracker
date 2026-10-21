#!/usr/bin/env python3
"""
Antenna trimming assistant for the cat collar (RA-02 / SX1278).

Reads the handheld's live RSSI while you trim the collar's radiator, and prints
mean RSSI per antenna length so the optimum is found by measurement instead of
by guesswork.

Why measure RSSI rather than range
-----------------------------------
In free space path loss grows 20 dB per decade of distance, so RSSI at a fixed
distance is proportional to range: the length that reads best at 3 m is the
length that reads best at 300 m. Comparing actual range is slow, noisy, and
confounded by foliage and orientation.

The handheld prints one RSSI reading per received packet. The collar pings
every PING_INTERVAL_MS (5 s), so that is one reading every 5 s - just wait,
there is no button on the collar to ping it early.

Setup before you trust the numbers
----------------------------------
  1. Stand the collar on a NON-METALLIC prop (cardboard, plastic, a shelf).
     A hand near the radiator detunes it by more than most of the changes
     you are trying to measure.
  2. Measure and mark one fixed distance and never move it. Both antennas
     keep the same orientation for every reading.
  3. Pick a distance where the starting RSSI is NOT pinned near -35 dBm.
     Saturated readings hide differences. Aim for roughly -70 to -95 dBm and
     if it reads better than -55 dBm, walk further away.
  4. Re-solder the same way each time, and keep the joint small and tidy. The
     solder blob on the ANT pad is part of the antenna - a joint that grows
     each trim changes what you are measuring. A small crocodile-clip or
     temporary contact for the test pass, then one final cut and solder once
     you know the length, is the most accurate approach.

Cutting is one-way
-------------------
Wire cannot be added back to a soldered joint, so a trim you regret is a trim
you have to undo with a whole new piece of wire. Two things to do about it:

  * KEEP EVERY OFFCUT. They are free, and re-fitting one is faster than
    starting again.
  * Start smaller than you think and work in fine steps near the end. Once two
    consecutive steps stop paying, this script says so - take the hint and go
    to 1-2 mm rather than pressing on.

Change ONE thing at a time
--------------------------
Trim the radiator only. Hold the counterpoise at a fixed length and a fixed 90
degree angle for the whole sweep. If you trim both together you lose the
ability to attribute the result, and the radiator's optimum length is itself a
function of the counterpoise length - so a later change to the counterpoise
invalidates the sweep.

Usage
-----
    python3 tools/tune_antenna.py
    python3 tools/tune_antenna.py --port /dev/cu.usbmodem14101 --distance 5
    python3 tools/tune_antenna.py --min-packets 25 --warmup 3

Commands (at the s> prompt)
    s 121.0     radiator is now 121.0 mm - closes the current run, opens a new one
    n note      attach a note to the current run (e.g. "counterpoise to 90 deg")
    p           print the table so far
    q / Ctrl-C  finish and print
"""

import argparse
import glob
import re
import statistics
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pip install pyserial")

# The handheld prints RSSI and this tool has to read it.
#
#   handheld_ble/handheld_ble.ino   [+] pkt #1 id=0  RSSI -44.0 dBm  smoothed -43.7
#                                    dBm  snr 11.0 dB  batt 78%
#
# The line can wrap mid-line depending on firmware and terminal width, so
# match on structure rather than on lines.
RSSI_RE = re.compile(rb"RSSI\s+(?:raw\s+)?(-?\d+(?:\.\d+)?)\s*dBm", re.S)
SNR_RE = re.compile(rb"[Ss][Nn][Rr]\s*(-?\d+(?:\.\d+)?)\s*dB", re.S)
PKT_RE = re.compile(rb"pkt\s*#(\d+)")
WALK_RE = re.compile(rb"WALK-TEST MODE")
# The handheld is silent at boot on purpose, so a packet line has to count as
# "ready" too.
READY_RE = re.compile(rb"Listening for collar pings")

# There is no BOOT button on the collar - the firmware poll that used to read
# one was removed. A ping happens every PING_INTERVAL_MS, so waiting is the
# only way; remind the user how long that is rather than telling them to press
# something that does not exist.
SILENCE_S = 12.0   # no packet for this long -> remind that pings are every 5s


# Finding the handheld's port on macOS needs care. /dev/cu.* contains the real
# USB serial nodes alongside things which are not serial devices at all -
# Bluetooth SPP nodes, the platform debug-console - so globbing it reports
# several "boards" that are not boards. Two other traps:
#
#   * macOS gives every port two nodes, /dev/cu.X (the callout, the one you
#     open) and /dev/tty.X (its BSD tty alias). They are one device.
#   * IOTTYBaseName looks like a USB marker but is set on every
#     IOSerialBSDClient, Bluetooth included. It is not one.
#
# The reliable test is ancestry: a USB serial node is reachable from an IOUSB
# interface node. Bluetooth nodes sit under IOBluetoothHCIController and the
# debug-console under the platform (AppleARMPE). ioreg prints the tree with an
# indent whose '|' and '+' count gives each node's depth, so the ancestor chain
# can be reconstructed from the output alone.
NODE_RE = re.compile(r'^(?P<pre>.*?)(?:[|+]*-?)o\s+(?P<name>\S+)\s+<class (?P<cls>\w+)')
CALLOUT_RE = re.compile(r'"IOCalloutDevice" = "(?P<dev>/dev/cu\.[^"]+)"')
BRANCH_RE = re.compile(r'[|+]')


def find_ports():
    """Callout nodes for attached USB serial devices, one per device."""
    try:
        out = subprocess.run(["ioreg", "-c", "IOSerialBSDClient", "-w", "0"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        out = ""

    ports = []
    stack = []      # classes of the node currently being described
    on_usb = False  # does that node sit under a USB interface?

    for line in out.splitlines():
        m = NODE_RE.match(line)
        if m:
            depth = len(BRANCH_RE.findall(m.group("pre")))
            stack = stack[:depth]
            stack.append(m.group("cls"))
            on_usb = any("USB" in c for c in stack)
            continue
        # Properties belong to the node whose header came just above.
        c = CALLOUT_RE.search(line)
        if c and on_usb and stack and stack[-1] == "IOSerialBSDClient":
            ports.append(c.group("dev"))

    if ports:
        return sorted(set(ports))

    # Fallback if ioreg is unavailable: take callout nodes and drop the
    # well-known virtual ones, accepting that odd devices may slip through.
    skip = ("Bluetooth", "debug-console", "wlan", "irda")
    return sorted(p for p in glob.glob("/dev/cu.*")
                  if not any(s in p for s in skip))


class Run:
    """One antenna length's worth of RSSI samples."""

    def __init__(self, length, note=""):
        self.length = length
        self.note = note
        # Every packet seen during this run, including the warmup ones that
        # are not kept. The warmup decision must be made against this, not
        # against len(rssi) - see the reader.
        self.nseen = 0
        self.rssi = []
        self.snr_last = None

    def mean(self):
        return statistics.fmean(self.rssi) if self.rssi else None

    def sd(self):
        return statistics.pstdev(self.rssi) if len(self.rssi) > 1 else 0.0


class Meter:
    def __init__(self, ser, min_packets, warmup, lock):
        self.ser = ser
        self.min_packets = min_packets
        self.warmup = warmup
        self.lock = lock
        self.runs = []
        self.cur = None
        self.pkt = 0
        self.last_rssi = None
        self.last_snr = None
        self.last_rx = time.monotonic()
        self.stop = False
        self._silenced = False

    # -- reader thread -------------------------------------------------
    def _reader(self):
        while not self.stop:
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
            except serial.SerialException:
                self.stop = True
                return
            if not chunk:
                time.sleep(0.02)
                continue
            self.last_rx = time.monotonic()
            self._silenced = False

            rssis = RSSI_RE.findall(chunk)
            snrs = SNR_RE.findall(chunk)
            pkts = PKT_RE.findall(chunk)
            if pkts:
                self.pkt = int(pkts[-1])

            with self.lock:
                if snrs:
                    self.last_snr = float(snrs[-1])
                if rssis:
                    self.last_rssi = float(rssis[-1])
                    if self.cur is not None:
                        # Discard the first packets after a trim: they straddle
                        # two antenna lengths and mean nothing.
                        #
                        # Count packets SEEN, not samples stored. Testing
                        # len(self.cur.rssi) >= self.warmup here can never be
                        # true, because rssi only grows once the test passes -
                        # so with the default warmup of 3 the run collected
                        # nothing, ever, while the display still showed the
                        # latest RSSI and looked alive.
                        self.cur.nseen += 1
                        if self.cur.nseen > self.warmup:
                            self.cur.rssi.append(float(rssis[-1]))
                            if snrs:
                                self.cur.snr_last = float(snrs[-1])
                    self._print_sample()

    def _print_sample(self):
        """Print one row per received reading and let it stay on screen.

        The previous display rewrote a single line in place with \\r, so only
        the newest reading was ever visible and it looked frozen between
        packets. Antenna trimming wants the spread: individual readings side
        by side show the multipath wobble a mean hides, which is part of what
        you are judging when you move a radiator around.

        Warmup packets are printed too, tagged, rather than silently dropped -
        otherwise the run appears stalled for the first few seconds and there
        is no way to tell that from a dead collar.
        """
        run = self.cur
        r = self.last_rssi
        if r is None:
            return
        snr = self.last_snr
        snr_txt = "snr %+5.1f" % snr if snr is not None else "snr      -"

        if run is None:
            sys.stdout.write("  pkt %3d  %7.1f dBm  %s   (no run open - "
                             "type 's <mm>')\n" % (self.pkt, r, snr_txt))
            sys.stdout.flush()
            return

        have = len(run.rssi)          # post-warmup samples kept so far
        need = self.min_packets
        bar_w = 16
        filled = int(bar_w * min(have, need) / max(need, 1))
        bar = "#" * filled + "." * (bar_w - filled)

        if run.nseen <= self.warmup:
            mean_txt = "   warmup, not counted  "
        else:
            mean_txt = "mean %7.1f dBm" % run.mean()

        sys.stdout.write("  %3d  %7.1f dBm  %s  %s  [%s %2d/%-2d]\n" % (
            run.nseen, r, snr_txt, mean_txt, bar, have, need))
        sys.stdout.flush()

    def _silence_watch(self):
        while not self.stop:
            time.sleep(0.5)
            if (time.monotonic() - self.last_rx > SILENCE_S
                    and not self._silenced):
                with self.lock:
                    self._silenced = True
                    # No \r here: readings scroll now, so this is just another
                    # line in the log rather than an overwrite.
                    sys.stdout.write(
                        "  -- no packet for %ds - the collar pings every 5s, "
                        "so check it is powered and within range\n"
                        % SILENCE_S)
                    sys.stdout.flush()

    def start(self):
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._silence_watch, daemon=True).start()

    def close_current(self, length=None, note=None):
        with self.lock:
            sys.stdout.write("\r\033[K")
            sys.stdout.flush()
            if self.cur is None:
                return None
            if length is not None:
                self.cur.length = length
            if note is not None:
                self.cur.note = note
            closed = self.cur
            if not closed.rssi:
                closed = None
                self.cur = None
                return None
            self.runs.append(closed)
            self.cur = Run(closed.length, closed.note)
            return closed


def fmt_len(x):
    return "%.1f" % x if x is not None else "-"


def print_table(runs):
    scored = [r for r in runs if r.rssi]
    if not scored:
        print("  (no runs with enough packets yet)")
        return
    base = scored[0].mean()
    best = max(r.mean() for r in scored)
    width = max(6, max(len(fmt_len(r.length)) for r in scored))
    hdr = "  %*s  %3s  %8s  %5s  %8s  %7s  %s" % (
        width, "len_mm", "n", "mean_dBm", "sd", "best_dBm", "gain", "note")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for r in scored:
        m = r.mean()
        star = "*" if m >= best else " "
        print("  %*s%s %3d  %8.1f  %5.1f  %8.1f  %+7.1f  %s" % (
            width, fmt_len(r.length), star, len(r.rssi), m, r.sd(),
            max(r.rssi), m - base, r.note))
    print()
    print("  gain is dB above the first run tested.")
    w = max(scored, key=lambda r: r.mean())
    print("  best so far: %s mm (%+.1f dB) -%s"
          % (fmt_len(w.length), w.mean() - base, w.note or "no note"))


def suggest_next(runs):
    """Step the last delta again if it helped, otherwise go back.

    Returns (length, text, needs_more_wire) or None. needs_more_wire is the
    point that matters in practice: you cannot add wire back to a soldered
    joint, so a suggestion to go longer means fitting a new piece first.
    """
    scored = [r for r in runs if r.rssi]
    if len(scored) < 2:
        return None
    a, b = scored[-2], scored[-1]
    step = b.length - a.length
    if step == 0:
        return None
    if b.mean() > a.mean():
        return b.length + step, "that helped - keep going this way", False
    return a.length, "that was worse - go back to the previous length", True


def step_hint(runs):
    """Guidance on step size, from the dB/mm the last step actually paid.

    A reversal is suggest_next's job. What this catches is diminishing returns:
    steps that still improve the reading, but by so little that you are
    grinding through wire far from the peak. That is the moment to go finer.
    """
    scored = [r for r in runs if r.rssi]
    if len(scored) < 3:
        return None
    a, b, c = scored[-3], scored[-2], scored[-1]
    d = c.length - b.length
    if d == 0:
        return None
    gain = (c.mean() - b.mean()) / abs(d)      # dB per mm
    if 0 < gain < 0.3:
        return ("last step paid only %.2f dB/mm - switch to 1-2 mm steps, "
                "the peak is close" % gain)
    return None


def ready(buf):
    """A packet line counts as ready as well as the console banner.

    handheld_ble prints nothing at boot until 'v' is sent, so waiting only for
    'Listening for collar pings' would time out forever against that build.
    """
    return bool(READY_RE.search(buf) or PKT_RE.search(buf))


def wait_for_console(ser, timeout=15.0):
    """Wait for the handheld to start reporting. Drops it out of walk-test mode if armed.

    'v' is sent once, up front. handheld_ble.ino prints nothing at boot until
    verbose logging is switched on; handheld.ino has no 'v' handler and simply
    ignores the byte, so it is safe for both.

    Exactly once, deliberately: 'v' is a TOGGLE, so resending it on a retry
    would switch logging straight back off and the handheld would then look
    dead. Equally, it cannot be sent in response to seeing output - the BLE
    build produces none until 'v' arrives, so that would never fire.
    """
    time.sleep(1.0)
    ser.write(b"v")

    deadline = time.monotonic() + timeout
    buf = b""
    sent_x = False
    glitches = 0
    while time.monotonic() < deadline:
        try:
            chunk = ser.read(ser.in_waiting or 1)
        except serial.SerialException:
            # The USB node can report "ready to read" and then hand back
            # nothing - seen just after opening, and when another process has
            # the port. Tolerate a few rather than dying with a traceback.
            glitches += 1
            if glitches > 200:
                sys.stdout.write(
                    "\n  the serial port keeps failing - another program "
                    "probably has it open.\n")
                sys.stdout.flush()
                return False
            time.sleep(0.05)
            continue
        if chunk:
            buf += chunk
            if ready(buf):
                return True
            if not sent_x and WALK_RE.search(buf):
                sys.stdout.write(
                    "  handheld was armed in walk-test mode - sending 'x' "
                    "to return to console\n")
                sys.stdout.flush()
                ser.write(b"x")
                sent_x = True
        else:
            time.sleep(0.05)
    return ready(buf)


def main():
    ap = argparse.ArgumentParser(
        description="Trim the collar antenna against the handheld's live RSSI.")
    ap.add_argument("--port", help="serial port (default: auto-detect)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--distance", type=float, default=None,
                    help="note only - your fixed measurement distance, in metres")
    ap.add_argument("--min-packets", type=int, default=15,
                    help="packets needed before a run counts (default 15)")
    ap.add_argument("--warmup", type=int, default=3,
                    help="packets discarded after each trim (default 3)")
    args = ap.parse_args()

    port = args.port
    if not port:
        ports = find_ports()
        if not ports:
            sys.exit("No serial ports found. Plug the handheld in, or pass "
                     "--port /dev/cu.usbmodemXXXX")
        if len(ports) > 1:
            print("Several boards attached:")
            for p in ports:
                print("   " + p)
            print("Re-run with --port <one of the above>.")
            print("(USB ports swap between the two boards - identify by what "
                  "it prints, not by name. The handheld prints "
                  "'Cat Tracker - Handheld'.)")
            sys.exit(1)
        port = ports[0]

    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = args.baud
        ser.timeout = 0.2
        # Do not let opening the port reset the board (DTR/RTS toggling).
        ser.dtr = False
        ser.rts = False
        ser.open()
    except serial.SerialException as e:
        sys.exit("Could not open %s: %s" % (port, e))

    print("Connected to %s. Waiting for the handheld..." % port)
    if not wait_for_console(ser):
        print("No packets seen. The handheld may not be this board, it may be "
              "still booting, or the BLE build may need 'v' typed at it "
              "first. Re-plug it and retry.")
        ser.close()
        sys.exit(1)

    dist_txt = ("  Distance: %.1f m\n" % args.distance if args.distance
                else "")
    print("\n" + "=" * 62)
    print(" Collar antenna trim - RSSI vs radiator length")
    print("=" * 62)
    print(dist_txt.rstrip("\n") if dist_txt else "")
    print("  Commands:  s <mm>   set current length, start a new run")
    print("              n <note> attach a note to the current run")
    print("              p        print the table")
    print("              q        finish")
    print("  The collar pings every 5 s on its own; readings arrive that often.")
    print("  Keep your hands and the wire well clear while a run is counting.")
    print()

    lock = threading.Lock()
    meter = Meter(ser, args.min_packets, args.warmup, lock)
    meter.start()

    try:
        while True:
            try:
                line = input("s> ").strip()
            except EOFError:
                break
            if not line:
                continue
            cmd, _, rest = line.partition(" ")
            cmd = cmd.lower()
            rest = rest.strip()

            if cmd == "q":
                break
            if cmd == "p":
                with lock:
                    sys.stdout.write("\r\033[K")
                print_table(meter.runs)
                continue
            if cmd == "s":
                try:
                    length = float(rest)
                except ValueError:
                    print("  usage: s <length in mm>, e.g. s 121.5")
                    continue
                closed = meter.close_current(length=length)
                if closed is None and meter.cur is None:
                    meter.cur = Run(length)
                elif closed is not None:
                    print("  recorded %s mm -> %.1f dBm mean (%d pkts, sd %.1f)"
                          % (fmt_len(closed.length), closed.mean(),
                             len(closed.rssi), closed.sd()))
                    nxt = suggest_next(meter.runs)
                    if nxt:
                        print("  -> try %s mm (%s)"
                              % (fmt_len(nxt[0]), nxt[1]))
                        if nxt[2]:
                            print("     !! that is LONGER than what is "
                                  "currently fitted.")
                            print("     !! Wire cannot be added back to a "
                                  "soldered joint. Either")
                            print("     !! re-fit a longer piece, or accept "
                                  "the previous length")
                            print("     !! as close to the peak.")
                    hint = step_hint(meter.runs)
                    if hint:
                        print("  -> %s" % hint)
                print("  Now measuring %s mm ..." % fmt_len(length))
                sys.stdout.write(
                    "     #  rssi      snr      running mean      "
                    "progress\n")
                sys.stdout.flush()
                continue
            if cmd == "n":
                if meter.cur is None:
                    print("  open a run first with 's <mm>'")
                    continue
                meter.cur.note = rest
                print("  noted on %s mm" % fmt_len(meter.cur.length))
                continue
            print("  unknown command %r (s / n / p / q)" % cmd)

    except KeyboardInterrupt:
        pass
    finally:
        with lock:
            sys.stdout.write("\r\033[K")
        meter.stop = True
        meter.close_current()
        # A run still in flight counts if it has any usable packets.
        if meter.cur is not None and meter.cur.rssi:
            meter.runs.append(meter.cur)
        ser.close()

    print()
    print_table(meter.runs)
    print("\n  Now cut to the best length in one clean step and solder it once.")
    print("  Re-check with the nanoVNA when it arrives - a VNA settles this in")
    print("  a minute, whereas this sweep takes an evening.")
    print()


if __name__ == "__main__":
    main()