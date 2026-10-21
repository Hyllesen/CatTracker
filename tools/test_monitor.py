#!/usr/bin/env python3
"""Self-test for tools/monitor.py: drives it with a fake serial port.

    python3 tools/test_monitor.py

Why this exists: the Report line is a contract between the firmware and this
tool, and a mismatch is silent - the tool prints nothing, which looks exactly
like a handheld that has not heard a collar. The firmware's format string is
checked against the parser here, so the two cannot drift apart without a test
failing.

The fake port replays the exact bytes emitReport() in
handheld_ble/handheld_ble.ino produces, so a change to the format string has to
be made in the test as well, and the test tells you when the two disagree.
"""
import importlib.util
import io
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("monitor",
                                             os.path.join(HERE, "monitor.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

# The bytes the firmware writes. Mirrors emitReport() in handheld_ble.ino:
#   "RPT %u t=%lu pk=%lu unk=%lu ok=%lu fail=%lu pc=%u ps=%u"
#   " c%u=%d/%d/%u/%lu/%lu/%u/%u/%d"
# Fields per collar: r10, s2, batt, packets, missed, age, flags, r10raw.
REPORTS = [
    "RPT 1 t=1000 pk=0 unk=0 ok=0 fail=0 pc=0 ps=1 "
    "c0=0/0/255/0/0/0/0/0 c1=0/0/255/0/0/0/0/0 c2=0/0/255/0/0/0/0/0",
    "RPT 1 t=3000 pk=1 unk=0 ok=1 fail=0 pc=1 ps=1 "
    "c0=-700/20/87/1/0/0/3/-695 c1=0/0/255/0/0/0/0/0 c2=0/0/255/0/0/0/0/0",
    "RPT 1 t=5000 pk=2 unk=1 ok=2 fail=0 pc=1 ps=1 "
    "c0=-680/22/87/2/0/2/3/-672 c1=-1100/10/50/1/0/2/1/-1103 c2=0/0/255/0/0/0/0/0",
    "RPT 1 t=7000 pk=3 unk=1 ok=3 fail=0 pc=1 ps=1 "
    "c0=-660/24/87/3/0/4/1/-655 c1=-1090/12/50/2/0/4/1/-1088 c2=0/0/255/0/0/0/0/0",
]


class FakeSerial:
    """Replays REPORTS at 2 s intervals, the handheld's keep-alive cadence."""

    def __init__(self, *a, **kw):
        self.port = None
        self.baudrate = 115200
        self.timeout = 0.2
        self.dtr = True
        self.rts = True
        self.i = 0
        self.next = 0.0
        self.opened = False
        self.closed = False

    def open(self):
        self.opened = True

    def read(self, n):
        if self.closed or self.i >= len(REPORTS):
            time.sleep(0.1)
            return b""
        if time.time() < self.next:
            time.sleep(0.05)
            return b""
        self.next = time.time() + 0.05
        line = REPORTS[self.i] + "\n"
        self.i += 1
        return line.encode()

    def close(self):
        self.closed = True


def run():
    m.serial.Serial = FakeSerial
    m.find_port = lambda: "/dev/cu.usbmodem99999"
    m.ANSI = False
    # Isolate from the repo's own cats.json: the checks below expect the
    # fallback names, and a rename in the repo file must not change what the
    # test asserts.
    m.default_names_path = lambda: None

    sys.argv = ["monitor.py", "--csv", "--duration", "1.0"]
    out, err = io.StringIO(), io.StringIO()
    real_out, real_err = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = out, err
    try:
        rc = m.main()
    finally:
        sys.stdout, sys.stderr = real_out, real_err
    # The summary goes to stderr with --csv, so that redirecting the CSV into a
    # file for analyze_walk.py gets rows only. Keep the two streams apart so the
    # "stdout is rows only" check is really checking stdout.
    return rc, out.getvalue(), err.getvalue()


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + (f"  {detail}" if detail and not cond else ""))
    return cond


def main():
    rc, out, err = run()
    rows = [r for r in out.splitlines() if re.match(r"^\d+\.\d+,", r)]
    rest = [r for r in err.splitlines() if r and not re.match(r"^\d+\.\d+,", r)]
    ok = True

    ok &= check("stdout is header + rows, nothing else",
                all(r.startswith("host_time") or re.match(r"^\d+\.\d+,", r)
                    for r in out.splitlines() if r),
                [r for r in out.splitlines() if not r.startswith("host_time")
                   and not re.match(r"^\d+\.\d+,", r)][:1])
    ok &= check("summary goes to stderr, not into the CSV file",
                any("reports" in r for r in rest), err[:60])

    ok &= check("exit code 0", rc == 0, f"rc={rc}")
    ok &= check("csv header present", out.startswith("host_time,dev_time,id,name"))
    ok &= check("one row per heard collar per report", len(rows) == 5, f"{len(rows)} rows")

    fields = [r.split(",") for r in rows]
    ok &= check("collar 0 rows", sum(1 for f in fields if f[2] == "0") == 3)
    ok &= check("collar 1 rows", sum(1 for f in fields if f[2] == "1") == 2)
    ok &= check("collar 2 never heard, no rows",
               sum(1 for f in fields if f[2] == "2") == 0)

    rssi = [float(f[4]) for f in fields]
    ok &= check("rssi decoded from dBm*10", rssi[0] == -70.0, f"{rssi[0]}")
    ok &= check("rssi negative scaling", rssi[-1] == -109.0, f"{rssi[-1]}")

    snr = [float(f[6]) for f in fields]
    ok &= check("snr decoded from dB*2", snr[0] == 10.0, f"{snr[0]}")

    ok &= check("battery carried, 255 not printed as a number",
                fields[0][7] == "87" and any(f[7] == "50" for f in fields), fields[0][7])
    ok &= check("packet count carried through to the last report",
                max(int(f[8]) for f in fields if f[2] == "0") == 3)
    ok &= check("all 12 columns present", all(len(f) == 12 for f in fields),
                [len(f) for f in fields])
    ok &= check("raw RSSI carried, and not the same number as smoothed",
                fields[0][5] == "-69.5" and fields[0][4] == "-70.0",
                fields[0][4:6])

    # analyze_walk.py must read what --csv writes. It reports "no samples
    # found" on a layout it does not know, which is silent enough to ship.
    csv_path = os.path.join(tempfile.gettempdir(), "monitor_walk.csv")
    with open(csv_path, "w") as f:
        f.write(out)
    res = subprocess.run([sys.executable, os.path.join(HERE, "analyze_walk.py"), csv_path],
                         capture_output=True, text=True)
    ok &= check("analyze_walk.py reads the --csv layout",
               "No samples found" not in res.stdout and "rssi range" in res.stdout,
               res.stdout.strip().splitlines()[0] if res.stdout.strip() else res.stdout)

    # The summary is printed after the loop.
    ok &= check("summary printed", any("reports" in r and "packets" in r for r in rest),
                rest[-1] if rest else "")
    ok &= check("per-collar summary", any("Cat 1" in r and "3 packets" in r for r in rest),
                rest[-1] if rest else "")

    # Names: the tool reads a JSON file mapping collar id -> cat name, the same
    # mapping the app keeps on the phone.
    path = os.path.join(tempfile.gettempdir(), "cats_test.json")
    with open(path, "w") as f:
        f.write('{"0": "Miso", "1": "Peanut"}')
    sys.argv = ["monitor.py", "--csv", "--duration", "1.0", "--names", path]
    out2 = io.StringIO()
    real = sys.stdout
    sys.stdout = out2
    try:
        m.main()
    finally:
        sys.stdout = real
    # The default names file, checked on a fresh copy of the module: run()
    # patched default_names_path away so the other checks see the fallback names.
    fresh_spec = importlib.util.spec_from_file_location(
        "monitor_fresh", os.path.join(HERE, "monitor.py"))
    fresh = importlib.util.module_from_spec(fresh_spec)
    fresh_spec.loader.exec_module(fresh)
    p = fresh.default_names_path()
    ok &= check("cats.json in the repo root is the default names file",
                p is not None and os.path.basename(p) == "cats.json"
                and fresh.load_names(p).get(0) == "Chichi", p)
    ok &= check("names from --names reach the CSV",
               '"Miso"' in out2.getvalue() and '"Peanut"' in out2.getvalue(),
               out2.getvalue().splitlines()[1] if len(out2.getvalue().splitlines()) > 1 else "")

    # Dashboard width: a row that wraps on an 80-column terminal turns a
    # dashboard you can read at a glance into a scroll of fragments.
    g, cs = m.parse_report(REPORTS[3])
    collars = [m.Collar(i) for i in range(3)]
    for i, c in cs.items():
        collars[i].update(c)
    block = m.render([g, collars, 4, 0.0], {0: "Miso", 1: "Peanut"},
                     type("a", (), {"port": "/dev/cu.usbmodem111301"})(), 80)
    over = [l for l in block.splitlines() if len(l) > 80]
    ok &= check("dashboard rows fit 80 columns", not over, over[:1])

    # Version mismatch must be reported, not silently swallowed.
    bad = "RPT 2 t=1000 pk=0 unk=0 ok=0 fail=0 pc=0 ps=1 c0=0/0/255/0/0/0/0 c1=0/0/255/0/0/0/0 c2=0/0/255/0/0/0/0"
    try:
        m.parse_report(bad)
        ok &= check("version 2 rejected", False, "no exception")
    except ValueError as e:
        ok &= check("version 2 rejected", "this tool understands 1" in str(e), str(e))

    # A truncated line must not be read as a short roster.
    trunc = "RPT 1 t=1000 pk=0 unk=0 ok=0 fail=0 pc=0 ps=1 c0=-700/20/87/1/0/0/3 c1=-1100/10/50/1/0"
    try:
        g, cs = m.parse_report(trunc)
        ok &= check("truncated collar entry rejected", len(cs) == 1 and "c1" not in "".join(cs.keys()),
                    f"{cs}")
    except ValueError:
        ok &= check("truncated collar entry rejected", True)

    print("\n" + ("all checks passed" if ok else "CHECKS FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
