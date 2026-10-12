"""Sound effects, synthesised (nothing downloaded): a wooden click for placed pieces, key taps for typed
text and a short chime for chapter openings. Run this file to (re)generate sfx/*.wav."""
import math
import os
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'sfx')
RATE = 44100


def _write(name, x, peak=0.8):
    x = np.asarray(x, dtype=np.float64)
    x = x / (np.max(np.abs(x)) + 1e-9) * peak
    fade = min(len(x), int(0.004 * RATE))
    x[-fade:] *= np.linspace(1, 0, fade)
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((x * 32767).astype(np.int16).tobytes())


def _t(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def _lowpass(x, cutoff):
    a = math.exp(-2 * math.pi * cutoff / RATE)
    y = np.zeros_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1 - a) * v + a * acc
        y[i] = acc
    return y


def click(rng, pitch=1.0):
    """A piece snapping onto the board: a short noise transient plus damped wooden resonances."""
    t = _t(0.11)
    noise = rng.standard_normal(len(t)) * np.exp(-t / 0.004)
    noise = noise - _lowpass(noise, 600)
    body = (np.sin(2 * np.pi * 950 * pitch * t) * np.exp(-t / 0.018) * 0.9 +
            np.sin(2 * np.pi * 2300 * pitch * t) * np.exp(-t / 0.008) * 0.5 +
            np.sin(2 * np.pi * 190 * pitch * t) * np.exp(-t / 0.03) * 0.6)
    return noise * 0.6 + body


def key(rng, pitch=1.0):
    """One key tap."""
    t = _t(0.06)
    noise = rng.standard_normal(len(t)) * np.exp(-t / 0.003)
    noise = noise - _lowpass(noise, 1500)
    tone = np.sin(2 * np.pi * 3200 * pitch * t) * np.exp(-t / 0.005) * 0.4 + np.sin(2 * np.pi * 420 * pitch * t) * np.exp(-t / 0.012) * 0.3
    return noise * 0.7 + tone


def chime():
    """Chapter opening: a soft marimba-like arpeggio over a quiet pad, with a little echo."""
    total = 3.0
    t = _t(total)
    x = np.zeros_like(t)
    notes = [(0.00, 523.25), (0.16, 659.25), (0.32, 783.99), (0.55, 1046.50)]
    for start, f in notes:
        k = int(start * RATE)
        tt = t[: len(t) - k]
        tone = (np.sin(2 * np.pi * f * tt) * np.exp(-tt / 0.55) +
                0.25 * np.sin(2 * np.pi * 4 * f * tt) * np.exp(-tt / 0.06) +
                0.12 * np.sin(2 * np.pi * 10 * f * tt) * np.exp(-tt / 0.02))
        x[k:] += tone * (1.2 if start > 0.5 else 1.0)
    pad = sum(np.sin(2 * np.pi * f * t) for f in (261.63, 329.63, 392.00)) / 3
    env = np.minimum(1, t / 0.6) * np.exp(-np.maximum(0, t - 0.8) / 0.8)
    x += 0.35 * pad * env
    out = x.copy()
    for delay, g in ((0.13, 0.32), (0.26, 0.14), (0.39, 0.06)):
        d = int(delay * RATE)
        out[d:] += g * x[:-d]
    return out


def generate():
    os.makedirs(OUT, exist_ok=True)
    rng = np.random.default_rng(7)
    for k, p in enumerate((1.0, 0.94, 1.06, 0.97)):
        _write('click%d' % k, click(rng, p), 0.8)
    for k, p in enumerate((1.0, 0.92, 1.08, 0.96, 1.04)):
        _write('key%d' % k, key(rng, p), 0.6)
    _write('chime', chime(), 0.7)


def path(name):
    return os.path.join(OUT, name + '.wav')


if __name__ == '__main__':
    generate()
    print(sorted(os.listdir(OUT)))
