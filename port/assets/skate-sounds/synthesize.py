#!/usr/bin/env python3
"""Makes the built-in skate sounds that are synthesized rather than recorded
(see CREDITS.txt for which): filtered noise and decaying tones, from a fixed
seed, so the same files come out each time.

    python port/assets/skate-sounds/synthesize.py [name ...]

writes each named sound as a 16-bit mono 44.1 kHz WAV file beside this
script: by default the two the built-in set ships (powerslide, roll_rough);
the others (roll, grind, slide, pop, land, board_impact, bail) are stand-ins
for the recorded files, kept to try. Standard library only.
"""

import math
import random
import struct
import sys
import wave
from pathlib import Path

RATE = 44100
HERE = Path(__file__).resolve().parent


def write(name, samples, peak=0.7):
    """samples normalised to peak, as name.wav"""
    top = max(1e-9, max(abs(s) for s in samples))
    data = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s / top * peak)) * 32767)) for s in samples)
    with wave.open(str(HERE / f"{name}.wav"), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(data)


def noise(count, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(count)]


def lowpass(samples, cutoff):
    a = 1.0 - math.exp(-2.0 * math.pi * cutoff / RATE)
    out, y = [], 0.0
    for s in samples:
        y += a * (s - y)
        out.append(y)
    return out


def highpass(samples, cutoff):
    low = lowpass(samples, cutoff)
    return [s - l for s, l in zip(samples, low)]


def bandpass(samples, centre, q):
    """a two-pole resonator (RBJ band pass, constant peak gain)"""
    w = 2.0 * math.pi * centre / RATE
    alpha = math.sin(w) / (2.0 * q)
    b0, b2 = alpha, -alpha
    a0, a1, a2 = 1.0 + alpha, -2.0 * math.cos(w), 1.0 - alpha
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for x in samples:
        y = (b0 * x + b2 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1, y2, y1 = x1, x, y1, y
        out.append(y)
    return out


def tone(frequency, seconds, decay, phase=0.0):
    count = int(seconds * RATE)
    return [math.sin(phase + 2.0 * math.pi * frequency * i / RATE) * math.exp(-i / (decay * RATE))
            for i in range(count)]


def envelope(samples, attack, decay):
    out = []
    for i, s in enumerate(samples):
        t = i / RATE
        out.append(s * min(1.0, t / attack if attack else 1.0) * math.exp(-t / decay))
    return out


def mix(*parts):
    length = max(len(p) for p in parts)
    return [sum(p[i] for p in parts if i < len(p)) for i in range(length)]


def scale(samples, gain):
    return [s * gain for s in samples]


def loopable(samples, fade):
    """the last `fade` seconds crossfaded into the start, and cut off, so the
    end runs straight into the start"""
    n = int(fade * RATE)
    head, body, tail = samples[:n], samples[n:-n], samples[-n:]
    joined = [tail[i] * (1.0 - i / n) + head[i] * (i / n) for i in range(n)]
    return joined + body


def roll(rng):
    """the wheels on concrete: a low rumble of filtered noise, the grain of
    the ground, and the faint thud of the wheels over seams"""
    seconds = 3.0
    count = int(seconds * RATE)
    rumble = lowpass(lowpass(noise(count, rng), 220), 220)
    grain = scale(bandpass(noise(count, rng), 1800, 0.7), 0.25)
    flutter = [1.0 + 0.15 * math.sin(2.0 * math.pi * 7.0 * i / RATE) for i in range(count)]
    body = [(r * 6.0 + g) * f for r, g, f in zip(rumble, grain, flutter)]
    return loopable(highpass(body, 40), 0.25)


def roll_rough(rng):
    """the wheels on dirt or grass: grittier, crackling"""
    seconds = 3.0
    count = int(seconds * RATE)
    rumble = lowpass(lowpass(noise(count, rng), 160), 160)
    crackle = [rng.uniform(-1, 1) if rng.random() < 0.004 else 0.0 for _ in range(count)]
    crackle = bandpass(crackle, 2500, 1.2)
    body = [r * 5.0 + c * 3.0 for r, c in zip(rumble, crackle)]
    return loopable(highpass(body, 40), 0.25)


def grind(rng):
    """metal trucks on a rail: bright scraping noise with ringing partials"""
    seconds = 2.5
    count = int(seconds * RATE)
    scrape = bandpass(noise(count, rng), 3200, 1.5)
    ring = mix(*[scale(bandpass(noise(count, rng), f, 40.0), 0.6) for f in (1850, 2710, 4130)])
    wobble = [1.0 + 0.3 * math.sin(2.0 * math.pi * 11.0 * i / RATE) for i in range(count)]
    return loopable([(s + r) * w for s, r, w in zip(scrape, ring, wobble)], 0.2)


def slide(rng):
    """the deck's wood sliding on a ledge or rail: a duller, lower hiss"""
    seconds = 2.5
    count = int(seconds * RATE)
    hiss = bandpass(noise(count, rng), 1100, 0.9)
    low = scale(lowpass(noise(count, rng), 300), 2.0)
    return loopable([h + l for h, l in zip(hiss, low)], 0.2)


def powerslide(rng):
    """the wheels sliding sideways: a squealing scrub"""
    seconds = 2.5
    count = int(seconds * RATE)
    scrub = bandpass(noise(count, rng), 900, 2.0)
    squeal = scale(bandpass(noise(count, rng), 2300, 25.0), 0.8)
    return loopable([a + b for a, b in zip(scrub, squeal)], 0.2)


def pop(rng):
    """the tail snapping on the ground: a sharp crack and a woody knock"""
    crack = envelope(highpass(noise(int(0.03 * RATE), rng), 1500), 0.0005, 0.006)
    knock = mix(tone(820, 0.25, 0.035), scale(tone(1460, 0.25, 0.02), 0.5), scale(tone(180, 0.25, 0.05), 0.8))
    return mix(scale(crack, 1.5), knock)


def land(rng):
    """the board landing: a deep thump, the deck's slap and the wheels'
    rattle"""
    thump = mix(tone(70, 0.5, 0.09), scale(tone(110, 0.5, 0.06), 0.7))
    slap = envelope(bandpass(noise(int(0.2 * RATE), rng), 1200, 0.8), 0.001, 0.025)
    rattle = envelope(bandpass(noise(int(0.3 * RATE), rng), 3500, 3.0), 0.005, 0.06)
    return mix(thump, scale(slap, 1.4), scale(rattle, 0.5))


def board_impact(rng):
    """the board on its own hitting the ground: a hollow wooden clack"""
    clack = mix(tone(640, 0.35, 0.05), scale(tone(1290, 0.35, 0.03), 0.6), scale(tone(2380, 0.35, 0.015), 0.3))
    hit = envelope(highpass(noise(int(0.05 * RATE), rng), 800), 0.0005, 0.008)
    return mix(clack, hit)


def bail(rng):
    """a body hitting the ground: a dull, heavy thud"""
    thud = mix(tone(55, 0.6, 0.12), scale(tone(85, 0.6, 0.08), 0.8))
    cloth = envelope(lowpass(noise(int(0.4 * RATE), rng), 900), 0.002, 0.08)
    return mix(thud, scale(cloth, 1.2))


SOUNDS = {
    "roll": roll,
    "roll_rough": roll_rough,
    "grind": grind,
    "slide": slide,
    "powerslide": powerslide,
    "pop": pop,
    "land": land,
    "board_impact": board_impact,
    "bail": bail,
}


# the synthesized sounds the built-in set ships (the rest are recordings)
SHIPPED = ("powerslide", "roll_rough")


def main(names):
    for name in names or SHIPPED:
        # (each from its own seed, so that one sound changed leaves the rest)
        rng = random.Random(f"opence-skate-{name}")
        write(name, SOUNDS[name](rng))
        print(f"{name}.wav")


if __name__ == "__main__":
    main(sys.argv[1:])
