#!/usr/bin/env python3
"""
NanoVNA driver for picking the collar's antenna.

Why a VNA rather than the RSSI method in tune_antenna.py
--------------------------------------------------------
tune_antenna.py compares RSSI between two builds of the antenna, which only
tells you which of two is *better*. It cannot tell you the input impedance, so
it cannot tell you whether the problem is a wrong length, a missing matching
network, or a lossy joint - and a wrong guess costs you a re-solder, because
wire cannot be added back.

The NanoVNA measures the radiator's actual impedance. That lets us see the
resonance, see where it sits relative to 433 MHz, and compute the matching
network by hand rather than by trial.

Why the calibration is done here and not on the device
------------------------------------------------------
The NanoVNA-H has an SMA connector directly on the port - no cables, no
attenuators, no splitters. The reference plane is therefore the connector
itself and is stable, which is the easy case. We still calibrate, because the
connector and the port's internal matching network are not ideal, and because
an uncalibrated S11 is meaningless for a low-impedance antenna.

We take the three standards as raw sweeps and apply the error terms in
software instead of pressing the device's own on-screen SOL. Doing it here
means the correction is saved as a file, is reproducible, and can be redone
from the raw captures without touching the hardware again.

Antenna reality check
---------------------
The collar radiator is a quarter-wave monopole at 433 MHz with a 17.3 cm
counterpoise (collar/README.md). A monopole is only half an antenna: without
the counterpoise and whatever the counterpoise is lying on, the measurement
is not the antenna you will ship. Put the jig on the cat, or on a slab with
roughly the same conductivity and size, before you trust a single number.
"""

from __future__ import annotations

import argparse
import glob
import math
import os
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass, field

Z0 = 50.0  # system impedance, ohms

# Handy bands. 433 MHz LoRa is narrow enough that a 60 MHz window around it
# shows the resonance with room to spare either side.
BANDS = {
    "wide": (10e6, 1500e6),
    "vhf": (30e6, 300e6),
    "433": (400e6, 470e6),
    "868": (800e6, 920e6),
    "915": (880e6, 960e6),
}


# --------------------------------------------------------------------------
# device link
# --------------------------------------------------------------------------
class LinkError(RuntimeError):
    """The device did not answer. Almost always a physical/firmware problem."""


class Link:
    """Serial link to a NanoVNA, speaking the firmware's ASCII line protocol.

    The protocol is not the old binary framing: commands are ASCII terminated
    with CR, the device echoes the command, it emits one response line per
    point, and the response ends with a prompt line starting 'ch>'.
    """

    def __init__(self, port: str, timeout: float = 4.0):
        import serial  # imported lazily so --simulate needs no hardware

        self.ser = serial.Serial(port, 115200, timeout=timeout, write_timeout=timeout)
        self.port = port
        # The device can be left mid-sweep from a previous program. Dropping
        # DTR and sending a bare CR resynchronises it.
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self) -> "Link":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def command(self, cmd: str, wait: float = 0.05, max_wait: float = 30.0) -> list[str]:
        """Send one command and collect its response lines.

        Echo suppression matters: the device echoes the command back before
        answering, so an unfiltered read feeds the command itself into the
        parser.
        """
        self.ser.reset_input_buffer()
        self.ser.write(f"{cmd}\r".encode("ascii"))
        deadline = time.time() + max_wait
        lines: list[str] = []
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                time.sleep(wait)
                continue
            line = raw.decode("ascii", "replace").strip()
            if not line:
                continue
            if line.startswith("ch>"):
                return lines
            if line == cmd:
                continue  # echo
            lines.append(line)
        raise LinkError(f"no 'ch>' prompt after {cmd!r} within {max_wait}s")

    # -- convenience readers -------------------------------------------------
    def version(self) -> str:
        return " ".join(self.command("version"))

    def info(self) -> list[str]:
        return self.command("info")

    def help(self) -> list[str]:
        return self.command("help")

    def set_sweep(self, start: float, stop: float, points: int) -> None:
        self.command(f"sweep {int(start)} {int(stop)} {points}")
        self.command("resume")

    @contextmanager
    def raw_mode(self):
        """Run with the device's own error correction switched off.

        This matters more than it looks. The device ships with a factory
        calibration that is genuinely good - it reads an open port as 0.987.
        The same port measured raw reads |S11| = 0.61, so the two differ a
        long way. Every number this tool produces is corrected by the error
        terms we build here, so if the device also applied its stored
        calibration the data would be corrected twice and every impedance
        would be wrong.

        'cal off' is used rather than 'cal reset' on purpose: reset throws
        away the factory calibration, whereas off only suspends it and
        'cal on' puts it back.
        """
        reply = self.command("cal off", max_wait=10)
        if reply and reply[0].startswith("usage:"):
            raise LinkError(
                "this firmware rejected 'cal off'. It reports its valid "
                f"calibration commands as: {reply[0]}"
            )
        try:
            yield
        finally:
            try:
                self.command("cal on", max_wait=10)
            except LinkError:
                pass  # never mask the real error; device keeps its factory cal

    def frequencies(self) -> list[int]:
        return [int(line) for line in self.command("frequencies")]

    def raw_s11(self) -> list[complex]:
        """Raw (device-side, uncalibrated) S11, one value per sweep point."""
        lines = self.command("data 0", max_wait=60)
        out = []
        for line in lines:
            parts = line.split()
            if len(parts) >= 2:
                out.append(complex(float(parts[0]), float(parts[1])))
        if not out:
            # Firmware >= 0.7.1 defaults to the scan-with-mask mode, where
            # 'data 0' returns nothing. Fall back to the masked scan, which
            # returns re0 im0 re1 im1 per point.
            for line in self.command("scan 0 0 0 0b110", max_wait=60):
                parts = line.split()
                if len(parts) >= 4:
                    out.append(complex(float(parts[0]), float(parts[1])))
        if not out:
            raise LinkError("device returned no S11 data")
        return out


# --------------------------------------------------------------------------
# data model + maths
# --------------------------------------------------------------------------
@dataclass
class Sweep:
    freqs: list[int]
    s11: list[complex]

    @property
    def z(self) -> list[complex]:
        """Impedance from reflection coefficient, de-embedded to Z0."""
        return [to_z(s) for s in self.s11]

    def index_at(self, freq: float) -> int:
        return min(range(len(self.freqs)), key=lambda i: abs(self.freqs[i] - freq))

    def subset(self, lo: float, hi: float) -> "Sweep":
        pairs = [(f, s) for f, s in zip(self.freqs, self.s11) if lo <= f <= hi]
        return Sweep([p[0] for p in pairs], [p[1] for p in pairs])


def to_z(gamma: complex) -> complex:
    return Z0 * (1 + gamma) / (1 - gamma)


def to_swr(gamma: complex) -> float:
    m = abs(gamma)
    if m >= 1.0:
        return float("inf")
    return (1 + m) / (1 - m)


@dataclass
class SolCalibration:
    """One-port short-open-load error terms, per frequency.

    Follows the same bilinear model as NanoVNA-Saver's Calibration.py so the
    numbers agree with that tool.
    """

    freqs: list[int]
    e00: list[complex] = field(default_factory=list)
    e11: list[complex] = field(default_factory=list)
    delta_e: list[complex] = field(default_factory=list)

    @classmethod
    def from_standards(
        cls, freqs: list[int], open_raw: list[complex], short_raw: list[complex], load_raw: list[complex]
    ) -> "SolCalibration":
        """Build error terms from the three measured standards.

        True values are ideal: open = +1, short = -1, load = 0.
        """
        if not (len(open_raw) == len(short_raw) == len(load_raw) == len(freqs)):
            raise ValueError("standards must all match the frequency list")

        g1, g2, g3 = complex(-1, 0), complex(1, 0), complex(0, 0)  # ideal standards
        cal = cls(freqs=freqs)
        for gm1, gm2, gm3 in zip(short_raw, open_raw, load_raw):
            denominator = (
                g1 * (g2 - g3) * gm1
                + g2 * g3 * gm2
                - g2 * g3 * gm3
                - (g2 * gm2 - g3 * gm3) * g1
            )
            if abs(denominator) < 1e-12:
                raise ValueError("degenerate calibration - standards are not distinguishable")
            e00 = -(
                (g2 * gm3 - g3 * gm3) * g1 * gm2
                - (g2 * g3 * gm2 - g2 * g3 * gm3 - (g3 * gm2 - g2 * gm3) * g1) * gm1
            ) / denominator
            e11 = ((g2 - g3) * gm1 - g1 * (gm2 - gm3) + g3 * gm2 - g2 * gm3) / denominator
            delta_e = -(
                (g1 * (gm2 - gm3) - g2 * gm2 + g3 * gm3) * gm1
                + (g2 * gm3 - g3 * gm3) * gm2
            ) / denominator
            cal.e00.append(e00)
            cal.e11.append(e11)
            cal.delta_e.append(delta_e)
        return cal

    def apply(self, sw: Sweep) -> Sweep:
        """Error-correct a raw sweep. Frequencies must match the cal grid."""
        if len(sw.s11) != len(self.freqs):
            raise ValueError(
                f"sweep has {len(sw.s11)} points, calibration has {len(self.freqs)}"
            )
        corrected = []
        for m, e00, e11, de in zip(sw.s11, self.e00, self.e11, self.delta_e):
            den = m * e11 - de
            corrected.append((m - e00) / den if abs(den) > 1e-12 else complex(float("nan"), 0))
        return Sweep(list(sw.freqs), corrected)

    def save(self, path: str) -> None:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("# NanoVNA SOL calibration, generated by tools/vna.py\n")
            fh.write("# freq_hz  e00_re e00_im  e11_re e11_im  delta_e_re delta_e_im\n")
            for f, a, b, c in zip(self.freqs, self.e00, self.e11, self.delta_e):
                fh.write(
                    f"{f} {a.real:.9g} {a.imag:.9g} "
                    f"{b.real:.9g} {b.imag:.9g} {c.real:.9g} {c.imag:.9g}\n"
                )

    @classmethod
    def load(cls, path: str) -> "SolCalibration":
        cal = cls(freqs=[])
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                v = [float(x) for x in line.split()]
                cal.freqs.append(int(v[0]))
                cal.e00.append(complex(v[1], v[2]))
                cal.e11.append(complex(v[3], v[4]))
                cal.delta_e.append(complex(v[5], v[6]))
        if not cal.freqs:
            raise ValueError(f"{path} holds no calibration data")
        return cal


# --------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------
@dataclass
class Summary:
    target_freq: int
    z_at_target: complex
    swr_at_target: float
    rl_at_target_db: float
    swr_freq: int
    swr: float
    z_at_swr: complex
    x_zero_freq: int | None


def summarise(sw: Sweep, target: float) -> Summary:
    """Locate the lowest-SWR point, the reactance zero, and the target itself.

    The number that matters is the one at the target frequency. The lowest
    SWR anywhere in the sweep is reported too, because for a resonant
    radiator the two land on the same frequency - and when they do not, the
    gap between them is the whole story: the antenna is tuned somewhere else.
    """
    mags = [abs(s) for s in sw.s11]
    i_swr = min(range(len(sw.freqs)), key=lambda i: mags[i])
    i_tgt = sw.index_at(target)
    m_tgt = mags[i_tgt]

    # reactance zero = sign change in imag(Z); interpolated for sub-point accuracy
    xs = [z.imag for z in sw.z]
    x_zero = None
    for i in range(len(xs) - 1):
        if xs[i] == 0:
            x_zero = sw.freqs[i]
            break
        if (xs[i] < 0) != (xs[i + 1] < 0):
            t = -xs[i] / (xs[i + 1] - xs[i]) if xs[i + 1] != xs[i] else 0.0
            x_zero = int(sw.freqs[i] + t * (sw.freqs[i + 1] - sw.freqs[i]))
            break

    return Summary(
        target_freq=sw.freqs[i_tgt],
        z_at_target=sw.z[i_tgt],
        swr_at_target=to_swr(sw.s11[i_tgt]),
        rl_at_target_db=(-20 * math.log10(m_tgt)) if m_tgt > 0 else float("inf"),
        swr_freq=sw.freqs[i_swr],
        swr=to_swr(sw.s11[i_swr]),
        z_at_swr=sw.z[i_swr],
        x_zero_freq=x_zero,
    )


def _series_el(x: float, f0: float) -> dict:
    """Turn a series reactance into a real, buildable component."""
    if abs(x) < 1e-12:
        return {"kind": "none", "value": 0.0}
    if x > 0:
        return {"kind": "inductor", "value": x / (2 * math.pi * f0)}
    return {"kind": "capacitor", "value": -1.0 / (x * 2 * math.pi * f0)}


def _shunt_el(b: float, f0: float) -> dict:
    """Turn a shunt susceptance into a real, buildable component."""
    if abs(b) < 1e-12:
        return {"kind": "none", "value": 0.0}
    if b > 0:
        return {"kind": "capacitor", "value": b / (2 * math.pi * f0)}
    return {"kind": "inductor", "value": -1.0 / (b * 2 * math.pi * f0)}


def match_network(z_load: complex, z_sys: float = Z0, f0: float = 1.0) -> list[dict]:
    """Two-element L-network solutions that transform z_load to z_sys at f0.

    A passive two-element network only exists in one of two topologies, and
    which one is fixed by the sign of Re(Z_load) - Re(Z_sys):

      * Re(Z_load) < Re(Z_sys): series element first, then shunt. A shunt
        element can only cancel susceptance, so the series element must
        already land the load on the constant-conductance circle g = 1/Z_sys,
        which needs x1^2 = R_load*(Z_sys - R_load) > 0.

      * Re(Z_load) > Re(Z_sys): shunt element first, then series. A series
        element cannot change the real part, so the shunt alone must move the
        load onto the constant-resistance circle Re = Z_sys, which needs the
        load's conductance g_l to already be below 1/Z_sys.

    Each topology yields two solutions, one per sign of the square root. Both
    are returned: they are equivalent at f0 but differ in bandwidth and in how
    hard they are to fit into a few square millimetres.

    Elements come back as concrete component values (henries or farads), never
    as signed reactances, so nothing downstream has to interpret a sign.
    """
    r_l, x_l = z_load.real, z_load.imag
    none_el = {"kind": "none", "value": 0.0}
    out: list[dict] = []

    # Degenerate-but-common case first. When the resistance already sits on
    # 50 ohm the only thing missing is the reactance, and the textbook
    # two-element answer degenerates into a silly high-Q network. Say the
    # useful thing instead.
    r_tol = 0.5
    if abs(r_l - z_sys) <= r_tol:
        if abs(x_l) <= r_tol:
            out.append({"topology": "already-matched", "series": none_el, "shunt": none_el})
        else:
            out.append({"topology": "series-only", "series": _series_el(-x_l, f0), "shunt": none_el})
        return out

    mag2 = r_l * r_l + x_l * x_l
    g_l = r_l / mag2 if mag2 else 0.0
    b_l = -x_l / mag2 if mag2 else 0.0
    g_2 = 1.0 / z_sys

    if 0 < r_l < z_sys:
        x1_sq = r_l * (z_sys - r_l)
        for sign in (+1, -1):
            x1 = sign * math.sqrt(x1_sq)
            out.append(
                {
                    "topology": "series-then-shunt",
                    "series": _series_el(x1 - x_l, f0),
                    "shunt": _shunt_el(x1 / (r_l * r_l + x1 * x1), f0),
                }
            )

    if r_l > z_sys and g_l < g_2:
        b_sq = g_l * (g_2 - g_l)
        for sign in (+1, -1):
            b = sign * math.sqrt(b_sq)
            out.append(
                {
                    "topology": "shunt-then-series",
                    # Z2 already contains the load's own reactance - it is the
                    # parallel combination of Z_load and the shunt - so the
                    # series element only has to cancel Im(Z2), which is
                    # -b*Z_sys/g_l. Subtracting x_l here would double-count it.
                    "series": _series_el(b * z_sys / g_l, f0),
                    "shunt": _shunt_el(b - b_l, f0),
                }
            )

    if not out:
        out.append(
            {
                "topology": "none",
                "note": f"Re(Z) = {r_l:.1f} ohm: no passive two-element L-network "
                        f"reaches {z_sys:.0f} ohm from here",
                "series": none_el,
                "shunt": none_el,
            }
        )
    return out


_ORDER = {
    "series-then-shunt": ["series", "shunt"],
    "shunt-then-series": ["shunt", "series"],
    "series-only": ["series"],
    "already-matched": ["series"],
}


def apply_network(z_load: complex, net: dict, f0: float) -> complex:
    """Impedance seen at the system port after applying net to z_load.

    Used to verify a synthesis rather than trust it.
    """
    z = z_load
    for part in _ORDER[net["topology"]]:
        el = net[part]
        if el["kind"] == "inductor":
            x = 2 * math.pi * f0 * el["value"]
        elif el["kind"] == "capacitor":
            x = -1.0 / (2 * math.pi * f0 * el["value"])
        else:
            x = 0.0
        if part == "series":
            z = z + complex(0, x)
        else:
            y = 1.0 / complex(0, x) if x else complex(0, 0)
            z = 1.0 / (1.0 / z + y)
    return z


def fmt_el(el: dict) -> str:
    kind, val = el["kind"], el["value"]
    if kind == "none":
        return "none"
    if kind == "inductor":
        return f"L = {val * 1e9:.2f} nH"
    return f"C = {val * 1e12:.2f} pF"


# --------------------------------------------------------------------------
# ASCII Smith chart
# --------------------------------------------------------------------------
_RGRID = (0.2, 0.5, 1.0, 2.0, 5.0)
_XGRID = (0.2, 0.5, 1.0, 2.0, 5.0)


def smith_chart(sw: Sweep, target: float, width: int = 71, height: int = 33) -> str:
    """Impedance Smith chart as text.

    The outer circle is |S11|=1; everything is clipped to it, so the chart
    only ever shows impedance space. Points outside the circle (SWR infinite,
    a short or an open) are clamped onto the boundary, because for a plain
    whip radiator the interesting bit really can sit out there.
    """
    grid = [[" "] * width for _ in range(height)]

    def px(u: float) -> int:
        return int(round((u + 1.0) / 2.0 * (width - 1)))

    def py(v: float) -> int:
        return int(round((1.0 - v) / 2.0 * (height - 1)))

    def inside(u: float, v: float) -> bool:
        return u * u + v * v <= 1.0

    def put(i: int, j: int, ch: str) -> None:
        if 0 <= i < height and 0 <= j < width:
            grid[i][j] = ch

    def stamp(cu: float, cv: float, rad: float, ch: str) -> None:
        for a in range(0, 3600):
            t = math.radians(a / 10)
            u, v = cu + rad * math.cos(t), cv + rad * math.sin(t)
            if inside(u, v):
                put(py(v), px(u), ch)

    stamp(0.0, 0.0, 1.0, "#")                                    # unit circle
    for r in _RGRID:                                             # constant Re
        stamp(r / (r + 1), 0.0, 1.0 / (r + 1), "o")
    for x in _XGRID:                                             # constant Im
        for sign in (+1, -1):
            xv = sign * x
            stamp(1.0, 1.0 / xv, 1.0 / abs(xv), "x")
    for j in range(width):                                        # real axis
        put(py(0.0), j, "-")

    clamped = 0
    prev = None
    for g in sw.s11:
        u, v = g.real, g.imag
        r = math.hypot(u, v)
        if r > 1.0:
            clamped += 1
            u, v = u / r * 0.99, v / r * 0.99
        cur = (py(v), px(u))
        if prev is not None:
            (r0, c0), (r1, c1) = prev, cur
            steps = max(abs(r1 - r0), abs(c1 - c0), 1)
            for k in range(steps + 1):
                put(r0 + (r1 - r0) * k // steps, c0 + (c1 - c0) * k // steps, "*")
        prev = cur

    idx = sw.index_at(target)
    out = ["".join(row) for row in grid]
    out.append(
        "  '#'=SWR infinite  'o'=const resistance  'x'=const reactance  '*'=sweep"
    )
    if clamped:
        out.append(f"  {clamped} point(s) beyond the circle, clamped to the edge")
    out.append(
        f"  {sw.freqs[idx]/1e6:.3f} MHz -> Z = {sw.z[idx].real:+.1f} {sw.z[idx].imag:+.1f}j ohm"
    )
    return "\n".join(out)


# --------------------------------------------------------------------------
# simulation (lets the whole pipeline be exercised without hardware)
# --------------------------------------------------------------------------
def simulate_antenna(start: float, stop: float, points: int, f0: float, r: float, q: float) -> Sweep:
    """Synthetic quarter-wave radiator over a small ground plane.

    Modelled as a series RLC: a lossless resonator with finite Q. R is the
    feed resistance a small ground plane produces, well under the 36.5 ohm a
    monopole over a perfect plane would show.
    """
    freqs = [int(start + (stop - start) * i / (points - 1)) for i in range(points)]
    out = []
    for f in freqs:
        # Series RLC: X = Q*R*(f/f0 - f0/f). At f0 the reactance is zero and
        # the feed shows its resistive value alone.
        z = complex(r, q * r * (f / f0 - f0 / f))
        out.append((z - Z0) / (z + Z0))
    return Sweep(freqs, out)


def sim_open(sw: Sweep, a: complex = complex(0.3, 0.9), l: complex = complex(-0.2, 0.6)) -> list[complex]:
    return [s * a + l for s in sw.s11]


# --------------------------------------------------------------------------
# io helpers
# --------------------------------------------------------------------------
def write_csv(path: str, sw: Sweep) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# freq_hz,s11_re,s11_im,s11_mag,s11_db,z_re,z_im,swr\n")
        for f, g, z in zip(sw.freqs, sw.s11, sw.z):
            mag = abs(g)
            db = -20 * math.log10(mag) if mag > 0 else float("inf")
            fh.write(
                f"{f},{g.real:.6g},{g.imag:.6g},{mag:.6g},"
                f"{db:.4f},{z.real:.4f},{z.imag:.4f},{to_swr(g):.4f}\n"
            )


def read_csv(path: str) -> Sweep:
    freqs, s11 = [], []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            v = line.split(",")
            freqs.append(int(float(v[0])))
            s11.append(complex(float(v[1]), float(v[2])))
    return Sweep(freqs, s11)


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    env = os.environ.get("VNA_PORT")
    if env:
        return env
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(cands) == 1:
        return cands[0]
    if not cands:
        raise LinkError("no /dev/cu.usbmodem* found - is the NanoVNA plugged in?")
    raise LinkError(
        "several serial devices found: " + ", ".join(cands) + " - pass --port"
    )


# --------------------------------------------------------------------------
# cli
# --------------------------------------------------------------------------
def parse_band(spec: str) -> tuple[float, float]:
    if spec in BANDS:
        return BANDS[spec]
    if "-" in spec:
        lo, hi = spec.split("-", 1)
        return float(lo) * 1e6, float(hi) * 1e6
    return float(spec) * 1e6, float(spec) * 1e6


def print_summary(sw: Sweep, target: float, label: str) -> Summary:
    s = summarise(sw, target)
    print(f"\n=== {label}")
    print(f"  sweep             : {len(sw.freqs)} points, "
          f"{sw.freqs[0]/1e6:.3f}-{sw.freqs[-1]/1e6:.3f} MHz")
    print(f"  at {s.target_freq/1e6:.3f} MHz   : "
          f"Z = {s.z_at_target.real:+.1f} {s.z_at_target.imag:+.1f}j ohm   "
          f"SWR = {s.swr_at_target:.2f}   RL = {s.rl_at_target_db:.1f} dB")
    print(f"  lowest SWR        : {s.swr:.2f} @ {s.swr_freq/1e6:.3f} MHz  "
          f"(Z = {s.z_at_swr.real:+.1f} {s.z_at_swr.imag:+.1f}j)")
    if s.x_zero_freq:
        off = (s.x_zero_freq - target) / 1e3
        print(f"  resonance         : {s.x_zero_freq/1e6:.3f} MHz  "
              f"({off:+.0f} kHz from target)")
    else:
        print("  resonance         : no reactance zero crossed in this sweep "
              "- widen the band or the antenna is badly off")

    over = sum(1 for g in sw.s11 if abs(g) >= 1.0)
    if over:
        print(f"  WARNING           : {over} point(s) at |S11|>=1 (past the Smith chart edge)")
    return s


def cmd_info(args) -> int:
    port = find_port(args.port)
    print(f"port: {port}")
    with Link(port) as link:
        print(f"\nversion:\n  {link.version()}")
        print("\ninfo:")
        for line in link.info():
            print(f"  {line}")
        print("\ncal:")
        for line in link.command("cal"):
            print(f"  {line}")
    return 0


def cmd_calibrate(args) -> int:
    port = find_port(args.port)
    start, stop = parse_band(args.band)
    points = args.points

    print(f"port {port}")
    with Link(port) as link:
        print(f"device: {link.version()}\n")

        link.set_sweep(start, stop, points)
        freqs = link.frequencies()
        print(f"calibrating over {len(freqs)} points "
              f"({freqs[0]/1e6:.1f}-{freqs[-1]/1e6:.1f} MHz)\n")

        # Standards must be captured raw. run_full captures each standard
        # across the whole grid with the sweep resumed, which is what keeps
        # the device from being left paused between steps.
        with link.raw_mode():
            print(">>> 1/3 OPEN: leave nothing on the connector, then press Enter")
            input()
            open_raw = run_full(link)

            print(">>> 2/3 SHORT: fit the short standard squarely, then press Enter")
            input()
            short_raw = run_full(link)

            print(">>> 3/3 LOAD: fit the 50 ohm load squarely, then press Enter")
            input()
            load_raw = run_full(link)

    cal = SolCalibration.from_standards(freqs, open_raw, short_raw, load_raw)
    cal.save(args.calfile)
    print(f"\nwrote {args.calfile}")

    # Quality gate. A calibration that only looks fine in the middle of the
    # band is worse than one you know is bad, because you trust it.
    print("\nstandard residuals (|S11| should be small at every frequency):")
    for name, raw in (("open", open_raw), ("short", short_raw), ("load", load_raw)):
        worst = max(abs(x) for x in raw)
        at = freqs[max(range(len(raw)), key=lambda i: abs(raw[i]))]
        flag = "" if worst < 0.5 else "   <- suspicious"
        print(f"  {name:<6} worst {worst:6.3f} @ {at/1e6:7.2f} MHz{flag}")
    return 0


def run_full(link: "Link") -> list[complex]:
    """Capture a full sweep with the sweep explicitly resumed first.

    If the sweep is left paused from a previous capture, 'data' blocks and the
    command never sees its prompt. Resuming before every read costs nothing
    and removes a whole class of hangs.
    """
    link.command("resume")
    return link.raw_s11()


def cmd_sweep(args) -> int:
    start, stop = parse_band(args.band)
    target = args.target
    label = args.label or "antenna"

    if args.simulate:
        sw = simulate_antenna(start, stop, args.points, args.sim_f0, args.sim_r, args.sim_q)
        print(f"[simulated] quarter-wave radiator: f0={args.sim_f0/1e6:.1f} MHz, "
              f"R={args.sim_r} ohm, Q={args.sim_q}")
    else:
        port = find_port(args.port)
        with Link(port) as link:
            link.set_sweep(start, stop, args.points)
            # Raw capture, or the stored factory cal would be applied here
            # and then again by our own error terms.
            with link.raw_mode():
                freqs = link.frequencies()
                sw = Sweep(freqs, run_full(link))
        calpath = args.calfile
        if os.path.exists(calpath) and not args.nocal:
            cal = SolCalibration.load(calpath)
            if len(cal.freqs) != len(sw.freqs) or cal.freqs != sw.freqs:
                # Re-run the calibration on the same sweep settings; error
                # terms are per-frequency and cannot be interpolated safely
                # across a different grid.
                raise SystemExit(
                    f"calibration grid does not match this sweep "
                    f"({len(cal.freqs)} vs {len(sw.freqs)} points). "
                    f"Re-run 'vna.py calibrate --band {args.band} --points {args.points}'."
                )
            sw = cal.apply(sw)
            print(f"applied calibration {calpath}")
        else:
            print("WARNING: raw, uncalibrated data - treat magnitudes as untrustworthy")

    os.makedirs(args.outdir, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(args.outdir, f"vna-{label}-{stamp}.csv")
    write_csv(path, sw)
    print(f"wrote {path}")

    summ = print_summary(sw, target, label)
    print()
    print(smith_chart(sw, target))

    sols = match_network(summ.z_at_swr, Z0, summ.swr_freq)
    print(f"\nL-network for the feed point, {summ.swr_freq/1e6:.3f} MHz, "
          f"Z = {summ.z_at_swr.real:.1f} {summ.z_at_swr.imag:+.1f}j ohm:")
    for s in sols:
        if s["topology"] == "none":
            print(f"  {s['note']}")
            continue
        print(f"  {s['topology']}")
        print(f"    series : {fmt_el(s['series'])}")
        print(f"    shunt  : {fmt_el(s['shunt'])}")
    return 0


def cmd_selftest(args) -> int:
    """Exercise the maths with no hardware.

    Two properties are checked.

    SOL: the device's forward error model is m = (delta_e*G - e00)/(e11*G - 1),
    which is the inverse of correct11. Synthesise the three standards through
    that model, build error terms from them, then push a known antenna through
    the same model and require the correction to recover the antenna exactly.
    If the bilinear algebra is wrong this cannot pass.

    L-network: every network the tool proposes must transform its load to
    50 ohm when fed back through apply_network, for loads above, below and on
    the system impedance, so both topologies get exercised.
    """
    print("--- SOL error correction -------------------------------------------")
    truth = simulate_antenna(400e6, 470e6, 41, 433e6, 18.0, 20.0)
    n = len(truth.freqs)

    # Frequency-dependent error terms, with signs the algebra must handle.
    e00 = [complex(0.15, 0.05) * (1 + 0.3 * i / n) for i in range(n)]
    e11 = [complex(-0.10, 0.02) * (1 - 0.4 * i / n) for i in range(n)]
    dlt = [complex(0.03, -0.04) * (1 + 0.2 * i / n) for i in range(n)]

    def forward(g: complex, i: int) -> complex:
        return (dlt[i] * g - e00[i]) / (e11[i] * g - 1.0)

    cal = SolCalibration.from_standards(
        truth.freqs,
        [forward(complex(1, 0), i) for i in range(n)],    # open
        [forward(complex(-1, 0), i) for i in range(n)],   # short
        [forward(complex(0, 0), i) for i in range(n)],    # load
    )
    err_e00 = max(abs(a - b) for a, b in zip(cal.e00, e00))
    err_e11 = max(abs(a - b) for a, b in zip(cal.e11, e11))
    err_dlt = max(abs(a - b) for a, b in zip(cal.delta_e, dlt))
    print(f"  error-term recovery: e00 {err_e00:.2e}  e11 {err_e11:.2e}  delta_e {err_dlt:.2e}")

    measured = Sweep(truth.freqs, [forward(g, i) for i, g in enumerate(truth.s11)])
    recovered = cal.apply(measured)
    worst = max(abs(a - b) for a, b in zip(recovered.s11, truth.s11))
    print(f"  worst |corrected - true| over {n} points: {worst:.2e}")
    cal_ok = max(err_e00, err_e11, err_dlt, worst) < 1e-12

    print("\n--- L-network synthesis -------------------------------------------")
    cases = [
        ("small radiator (R<Z0)", complex(12.0, 0.0)),
        ("detuned radiator", complex(12.0, -2.2)),
        ("already 50 ohm", complex(50.0, 0.0)),
        ("high-impedance (R>Z0)", complex(180.0, 25.0)),
        ("near-short", complex(3.0, 1.0)),
        ("pure reactive", complex(50.0, 40.0)),
    ]
    f0 = 433e6
    nets_ok = True
    for name, z in cases:
        print(f"  {name:<26} Z = {z.real:6.1f} {z.imag:+6.1f}j")
        for net in match_network(z, Z0, f0):
            if net["topology"] == "none":
                print(f"      {net['note']}")
                continue
            z_out = apply_network(z, net, f0)
            err = abs(z_out - Z0)
            verdict = "ok" if err < 1e-6 else "MISMATCH"
            if err >= 1e-6:
                nets_ok = False
            print(f"      {net['topology']:<20} series {fmt_el(net['series']):>14}"
                  f"   shunt {fmt_el(net['shunt']):>14}"
                  f"   -> {z_out.real:6.2f}{z_out.imag:+5.2f}j  {verdict}")

    print()
    if cal_ok and nets_ok:
        print("selftest ok")
        return 0
    print("SELFTEST FAILED", "" if cal_ok else "(SOL)", "" if nets_ok else "(L-network)")
    return 1


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="NanoVNA driver for the cat collar antenna",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--port", help="serial device (default: autodetect /dev/cu.usbmodem*)")
        sp.add_argument("--band", default="433", help="named band, 'lo-hi' in MHz, or a single MHz value")
        sp.add_argument("--points", type=int, default=101, help="sweep points")
        sp.add_argument("--target", type=float, default=433e6, help="frequency of interest, Hz")
        sp.add_argument("--calfile", default=".vna/nano.sol", help="calibration file")
        return sp

    s = sub.add_parser("info", help="ask the device who it is")
    s.add_argument("--port")
    s.set_defaults(func=cmd_info)

    s = common(sub.add_parser("calibrate", help="guided short-open-load calibration"))
    s.set_defaults(func=cmd_calibrate)

    s = common(sub.add_parser("sweep", help="measure and analyse"))
    s.add_argument("--label", help="name for the output file")
    s.add_argument("--outdir", default="antenna-vna", help="where to write the CSV")
    s.add_argument("--nocal", action="store_true", help="skip the calibration file")
    s.add_argument("--simulate", action="store_true", help="use synthetic data instead of hardware")
    s.add_argument("--sim-f0", type=float, default=440e6, dest="sim_f0")
    s.add_argument("--sim-r", type=float, default=15.0, dest="sim_r", help="feed resistance, ohm")
    s.add_argument("--sim-q", type=float, default=25.0, dest="sim_q")
    s.set_defaults(func=cmd_sweep)

    s = sub.add_parser("selftest", help="verify the maths offline, no hardware")
    s.set_defaults(func=cmd_selftest)

    args = p.parse_args(argv)
    try:
        return args.func(args)
    except LinkError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        print(
            "\nIf the device is plugged in and identified but returns nothing, the\n"
            "problem is at the device, not the software: power-cycle it, check it\n"
            "is showing the main sweep screen, and try another USB port/cable.",
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    sys.exit(main())