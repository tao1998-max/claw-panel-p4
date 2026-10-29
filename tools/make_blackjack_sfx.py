#!/usr/bin/env python3
"""Generate the Blackjack UI sound effects as 16 kHz mono OGG/Opus files."""
import math
import os
import random
import shutil
import subprocess
import sys
import wave

SR = 16000
OUT = os.path.join(os.path.dirname(__file__), "..", "apps", "blackjack", "assets", "sfx")
TMP = os.path.join(os.path.dirname(__file__), "..", "build", "sfx_wav")

random.seed(21)


def env(n, attack=0.006, release=0.04):
    a = max(1, int(SR * attack))
    r = max(1, int(SR * release))
    out = [1.0] * n
    for i in range(min(a, n)):
        out[i] = i / a
    for i in range(min(r, n)):
        out[n - 1 - i] = min(out[n - 1 - i], i / r)
    return out


def add_tone(buf, start, dur, freq, amp, harmonics=((1, 1.0), (2, 0.16), (3, 0.05)),
             attack=0.006, release=0.04, vibrato=0.0):
    n = int(dur * SR)
    e = env(n, attack, release)
    for i in range(n):
        t = i / SR
        phase = 2.0 * math.pi * freq * t
        if vibrato:
            phase += vibrato * math.sin(2.0 * math.pi * 5.0 * t)
        s = 0.0
        for h, ha in harmonics:
            s += ha * math.sin(phase * h)
        idx = start + i
        if idx < len(buf):
            buf[idx] += amp * s * e[i]


def normalize(buf, peak=0.9):
    m = max(1e-9, max(abs(x) for x in buf))
    if m > peak:
        scale = peak / m
        for i in range(len(buf)):
            buf[i] *= scale
    return buf


def write_wav(name, buf):
    os.makedirs(TMP, exist_ok=True)
    path = os.path.join(TMP, name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(
            int(max(-32768, min(32767, x * 32767))).to_bytes(2, "little", signed=True)
            for x in buf))
    return path


def encode(name, buf):
    wav = write_wav(name, normalize(buf))
    ogg = os.path.join(OUT, name + ".ogg")
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error", "-i", wav,
        "-c:a", "libopus", "-b:a", "24k", "-ar", str(SR), "-ac", "1",
        "-application", "audio", "-frame_duration", "60", ogg,
    ], check=True)
    print(f"{name}.ogg {os.path.getsize(ogg)} bytes")


def deal():
    n = int(0.19 * SR)
    buf = [0.0] * n
    lp = 0.0
    for i in range(n):
        t = i / SR
        x = random.uniform(-1.0, 1.0)
        lp += 0.16 * (x - lp)
        swell = min(1.0, t / 0.02) * math.exp(-t * 14.0)
        buf[i] = lp * 0.85 * swell
        buf[i] += 0.18 * math.sin(2 * math.pi * 145 * t) * math.exp(-t * 30)
        buf[i] += 0.10 * x * math.exp(-t * 70)
    return buf


def flip():
    n = int(0.09 * SR)
    buf = [0.0] * n
    lp = 0.0
    for i in range(n):
        t = i / SR
        x = random.uniform(-1.0, 1.0)
        lp += 0.5 * (x - lp)
        buf[i] = (x - lp) * 0.7 * math.exp(-t * 40)
        buf[i] += 0.25 * math.sin(2 * math.pi * 1900 * t) * math.exp(-t * 55)
    return buf


def click():
    n = int(0.045 * SR)
    buf = [0.0] * n
    for i in range(n):
        t = i / SR
        buf[i] = 0.55 * math.sin(2 * math.pi * 1350 * t) * math.exp(-t * 75)
        buf[i] += 0.15 * random.uniform(-1, 1) * math.exp(-t * 120)
    return buf


def win():
    buf = [0.0] * int(0.62 * SR)
    notes = [(523.25, 0.10), (659.25, 0.10), (783.99, 0.10), (1046.5, 0.26)]
    start = 0
    for f, d in notes:
        add_tone(buf, start, d, f, 0.28, release=0.03)
        start += int(d * SR)
    add_tone(buf, start - int(0.26 * SR), 0.26, 783.99, 0.10,
             harmonics=((1, 1.0), (2, 0.12)), release=0.08)
    return buf


def blackjack():
    buf = [0.0] * int(0.78 * SR)
    notes = [(523.25, 0.075), (659.25, 0.075), (783.99, 0.075),
             (1046.5, 0.075), (1318.5, 0.075), (1568.0, 0.30)]
    start = 0
    for f, d in notes:
        add_tone(buf, start, d, f, 0.25, release=0.025)
        start += int(d * SR)
    chord = start - int(0.30 * SR)
    for f in (1046.5, 1318.5, 1568.0):
        add_tone(buf, chord, 0.30, f, 0.11, release=0.10)
    return buf


def lose():
    buf = [0.0] * int(0.64 * SR)
    notes = [(392.0, 0.16), (329.63, 0.16), (261.63, 0.30)]
    start = 0
    for f, d in notes:
        add_tone(buf, start, d, f, 0.25, release=0.06, vibrato=0.015)
        start += int(d * SR)
    add_tone(buf, 0, 0.55, 130.81, 0.08, harmonics=((1, 1.0), (2, 0.2)),
             release=0.2)
    return buf


def push():
    buf = [0.0] * int(0.38 * SR)
    add_tone(buf, 0, 0.14, 659.25, 0.24, release=0.03)
    add_tone(buf, int(0.14 * SR), 0.22, 523.25, 0.24, release=0.08)
    return buf


def bust():
    n = int(0.36 * SR)
    buf = [0.0] * n
    phase = 0.0
    for i in range(n):
        t = i / SR
        freq = 140.0 - 70.0 * (t / 0.36)
        phase += freq / SR
        saw = 2.0 * (phase % 1.0) - 1.0
        trem = 0.65 + 0.35 * math.sin(2 * math.pi * 24 * t)
        e = min(1.0, t / 0.006) * math.exp(-t * 5.0)
        buf[i] = (0.20 * saw + 0.06 * random.uniform(-1, 1)) * trem * e
    return buf


def main():
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found")
    os.makedirs(OUT, exist_ok=True)
    encode("deal", deal())
    encode("flip", flip())
    encode("click", click())
    encode("win", win())
    encode("blackjack", blackjack())
    encode("lose", lose())
    encode("push", push())
    encode("bust", bust())


if __name__ == "__main__":
    main()
