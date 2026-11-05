#!/usr/bin/env python3
"""Write a plucked-string test tone as a 16 kHz mono WAV, for the emulator.

    python3 tools/pluck_wav.py E2 -12 /tmp/e2_flat.wav   # E2, 12 cents flat
    pebble emu-mic --emulator emery --file /tmp/e2_flat.wav

The note is in scientific pitch notation (A4 = 440 Hz), with # or b for
accidentals. Partials decay faster the higher they are, like a real string.
"""

import math
import random
import struct
import sys
import wave

RATE = 16000
NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def note_hz(note):
    pitch = NAMES[note[0].upper()]
    rest = note[1:]
    if rest[:1] == "#":
        pitch, rest = pitch + 1, rest[1:]
    elif rest[:1] == "b":
        pitch, rest = pitch - 1, rest[1:]
    midi = (int(rest) + 1) * 12 + pitch
    return 440.0 * 2 ** ((midi - 69) / 12)


def pluck(hz, seconds, peak=0.4):
    n = int(seconds * RATE)
    out = [0.0] * n
    rng = random.Random(1)
    for k in range(1, 25):
        fk = k * hz
        if fk > RATE * 0.45:
            break
        amp = (1.4 if k == 2 else 1.0) / k
        decay = 1.2 + 0.6 * k
        phase = rng.uniform(0, 2 * math.pi)
        w = 2 * math.pi * fk / RATE
        for i in range(n):
            out[i] += amp * math.exp(-decay * i / RATE) * math.sin(phase + w * i)
    top = max(abs(v) for v in out)
    return [v * peak / top for v in out]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    note, cents, path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    hz = note_hz(note) * 2 ** (cents / 1200)
    samples = pluck(hz, 3.0)
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(b"".join(struct.pack("<h", int(v * 32767)) for v in samples))
    print(f"{note} {cents:+g} ct = {hz:.2f} Hz -> {path}")


if __name__ == "__main__":
    main()
