#!/usr/bin/env python3
"""Generate the discrete tracking tones bundled with the Android app.

One short WAV per signal step (1..10). Pitch rises as the signal gets
stronger, Geiger-counter style, so "higher means closer" is learnable in a
single walk and then readable while your eyes stay on the cat.

Uses only the standard library, so the tones are reproducible from source
rather than being opaque binary blobs checked into the repo.

Usage:
    python3 tools/make_tones.py

Output:
    CatTrackApp/android/app/src/main/res/raw/tone_1.wav ... tone_10.wav

Re-run this whenever the pitch scheme changes, then rebuild the app.
"""

from __future__ import annotations

import math
import os
import struct
import wave

# A major pentatonic (A B C# D E F# G A B C#) rather than a chromatic run.
# Pentatonic steps are chosen so no two adjacent tones form a harsh interval,
# which matters here because a weak signal flickers between neighbouring steps
# and the ear is more forgiving of that movement in this scale.
STEP_HZ = [
    440.00,   # 1  A4 - faintest
    493.88,   # 2  B4
    554.37,   # 3  C#5
    659.25,   # 4  E5   (D5 skipped: sits too close to C#5 to read at a glance)
    739.99,   # 5  F#5
    880.00,   # 6  A5
    987.77,   # 7  B5
    1108.73,  # 8  C#6
    1318.51,  # 9  E6
    1760.00,  # 10 A6 - strongest, doubled octave for a clear "on top of it"
]

SAMPLE_RATE = 22050  # plenty for tones under 2 kHz, and halves the file size
DURATION_S = 0.22     # short enough to read as a blip, long enough to locate
# Peak amplitude. At 0.35 (-9 dBFS) the tones played correctly but were easy
# to miss - a quarter-second quiet blip every 5 s on a phone speaker does not
# reliably reach the ear outdoors. 0.8 leaves 2 dB of headroom, so repeated
# blips cannot clip.
PEAK = 0.8

# Envelope, as fractions of the total tone length. A raw sine starting and
# ending at full scale clicks audibly on a phone speaker; short ramps at both
# ends remove that.
ATTACK = 0.08
RELEASE = 0.30


def envelope(t: float, total: float) -> float:
    """Amplitude multiplier for time `t` within a tone of length `total`."""
    if t < ATTACK * total:
        return t / (ATTACK * total)
    tail = total - t
    if tail < RELEASE * total:
        return max(0.0, tail / (RELEASE * total))
    return 1.0


def render(freq: float) -> bytes:
    frames = int(SAMPLE_RATE * DURATION_S)
    samples = bytearray()
    for i in range(frames):
        t = i / SAMPLE_RATE
        value = math.sin(2.0 * math.pi * freq * t)
        value *= envelope(t, DURATION_S)
        # Slight taper across the body keeps repeated blips from feeling harsh.
        value *= PEAK
        samples += struct.pack("<h", int(max(-1.0, min(1.0, value)) * 32767))
    return bytes(samples)


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(
        root, "CatTrackApp", "android", "app", "src", "main", "res", "raw"
    )
    os.makedirs(out_dir, exist_ok=True)

    for index, freq in enumerate(STEP_HZ, start=1):
        path = os.path.join(out_dir, f"tone_{index}.wav")
        with wave.open(path, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(SAMPLE_RATE)
            wav.writeframes(render(freq))
        print(f"tone_{index}.wav  {freq:7.2f} Hz")


if __name__ == "__main__":
    main()