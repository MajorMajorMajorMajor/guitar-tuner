#!/usr/bin/env python3
"""Write a plucked-string test tone as a 16 kHz mono WAV, for the emulator.

    python3 tools/pluck_wav.py E2 -12 /tmp/e2_flat.wav      # E2, 12 cents flat
    python3 tools/pluck_wav.py E4 0 /tmp/e4.wav --thump 110  # hard pluck: body thump
    python3 tools/pluck_wav.py A2 0 /tmp/soft.wav --peak 0.01 --noise 0.001
    pebble emu-mic --emulator emery --file /tmp/e2_flat.wav

The note is in scientific pitch notation (A4 = 440 Hz), with # or b for
accidentals. Partials decay faster the higher they are, like a real string.

Options:
  --peak X     peak level, 0..1 of full scale (default 0.4)
  --thump HZ   add a guitar-body resonance at HZ ringing for ~0.15 s after
               the pluck, as loud as the string (a hard pluck on an acoustic)
  --noise X    add white noise at RMS X of full scale (a microphone floor)
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


def pluck(hz, seconds, peak):
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
    args = sys.argv[1:]
    opts = {"--peak": 0.4, "--thump": 0.0, "--noise": 0.0}
    positional = []
    i = 0
    while i < len(args):
        if args[i] in opts and i + 1 < len(args):
            opts[args[i]] = float(args[i + 1])
            i += 2
        else:
            positional.append(args[i])
            i += 1
    if len(positional) != 3:
        sys.exit(__doc__)
    note, cents, path = positional[0], float(positional[1]), positional[2]
    hz = note_hz(note) * 2 ** (cents / 1200)
    peak = opts["--peak"]
    samples = pluck(hz, 3.0, peak)
    if opts["--thump"]:
        w = 2 * math.pi * opts["--thump"] / RATE
        for i in range(len(samples)):
            samples[i] += peak * math.exp(-i / (0.15 * RATE)) * math.sin(0.7 + w * i)
    if opts["--noise"]:
        rng = random.Random(2)
        samples = [v + rng.gauss(0, opts["--noise"]) for v in samples]
    top = max(1.0, max(abs(v) for v in samples))
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(b"".join(struct.pack("<h", int(v / top * 32767)) for v in samples))
    print(f"{note} {cents:+g} ct = {hz:.2f} Hz -> {path}")


if __name__ == "__main__":
    main()
