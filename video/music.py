"""Background music, synthesised (nothing downloaded): a quiet ambient piano, slow chords with a sparse
arpeggio and a long reverb. Every note lives on one global timeline (seconds from the start of the video),
so a slice of it rendered for one scene joins the slices of the scenes around it seamlessly.

    .venv/Scripts/python.exe music.py 0 64 music_test.wav      # render seconds 0-64 of the timeline, to listen
"""
import math
import random
import sys
import wave

import numpy as np

RATE = 44100
BEAT = 1.0                  # 60 bpm
CHORD = 8 * BEAT            # one chord every 8 seconds
NOTE = 6.0                  # how long a note rings
TAIL = 3.0                  # reverb length
PRE = NOTE + TAIL + 1.0     # notes this long before a slice still sound in it
GAIN = 0.2                 # fixed, never per slice: slices must match where they meet
# D major, slow and open: (bass, chord tones for the arpeggio), MIDI numbers
PROGRESSION = [
    (38, [62, 66, 69, 73, 76]),   # Dmaj9
    (35, [62, 66, 69, 71, 74]),   # Bm7
    (31, [59, 62, 66, 67, 71]),   # Gmaj7
    (33, [61, 64, 69, 71, 76]),   # Aadd9
    (42, [62, 66, 69, 74, 78]),   # D/F#
    (31, [62, 67, 71, 74, 79]),   # G
    (40, [59, 64, 67, 71, 74]),   # Em7
    (33, [61, 64, 67, 69, 73]),   # A7
]


def _note(midi, velocity):
    """A soft piano-like tone: slightly inharmonic partials, the higher ones dying faster and quieter on soft notes."""
    f = 440.0 * 2 ** ((midi - 69) / 12)
    t = np.arange(int(NOTE * RATE)) / RATE
    x = np.zeros_like(t)
    rng = random.Random(midi)
    for k in range(1, 9):
        fk = k * f * math.sqrt(1 + 0.0004 * k * k)
        if fk > 8000:
            break
        amp = k ** -1.4 * velocity ** (0.35 * (k - 1))
        decay = (0.35 + 0.55 * (k - 1)) * (f / 261.6) ** 0.3
        x += amp * np.sin(2 * math.pi * fk * t + rng.random() * 6.283) * np.exp(-decay * t)
    x *= np.minimum(1.0, t / 0.008) * velocity          # soft hammer
    x[-int(0.3 * RATE):] *= np.linspace(1, 0, int(0.3 * RATE))
    return x


def _notes(c):
    """The notes of chord number c: (start time, midi, velocity), all decided by c alone."""
    bass, tones = PROGRESSION[c % len(PROGRESSION)]
    rng = random.Random(7919 + c)
    t0 = c * CHORD
    out = [(t0, bass + 12, 0.55), (t0 + 0.02, tones[0], 0.32), (t0 + 0.05, tones[2], 0.28)]
    last = None
    for b in range(1, 8):
        if rng.random() < 0.55:
            m = rng.choice([x for x in tones if x != last])
            last = m
            out.append((t0 + b * BEAT + rng.uniform(-0.04, 0.04), m + (12 if rng.random() < 0.12 else 0), rng.uniform(0.18, 0.36)))
    return out


_CACHE = {}
_IR = []


def _impulse():
    if not _IR:
        rng = np.random.default_rng(3)
        t = np.arange(int(TAIL * RATE)) / RATE
        ir = rng.standard_normal(len(t)) * np.exp(-t / 0.55)
        ir = np.convolve(ir, np.ones(12) / 12, mode='same')   # darker
        ir[0] = 0
        _IR.append(ir / np.sqrt(np.sum(ir ** 2)) * 0.5)
    return _IR[0]


def render(t0, t1):
    """The music from t0 to t1 seconds on the global timeline, as float samples."""
    start = t0 - PRE
    dry = np.zeros(int((t1 - start) * RATE) + 1)
    for c in range(max(0, int(math.floor(start / CHORD))), int(math.ceil(t1 / CHORD)) + 1):
        for s, m, v in _notes(c):
            if s < start or s >= t1:
                continue
            if (m, round(v, 3)) not in _CACHE:
                _CACHE[(m, round(v, 3))] = _note(m, v)
            x = _CACHE[(m, round(v, 3))]
            i = int(round((s - start) * RATE))
            n = min(len(x), len(dry) - i)
            dry[i:i + n] += x[:n]
    ir = _impulse()
    size = 1 << int(math.ceil(math.log2(len(dry) + len(ir))))
    wet = np.fft.irfft(np.fft.rfft(dry, size) * np.fft.rfft(ir, size), size)[:len(dry)]
    out = (0.75 * dry + wet) * GAIN
    out = out[int(PRE * RATE):int(PRE * RATE) + int((t1 - t0) * RATE)]
    # the very start of the video fades in
    t = t0 + np.arange(len(out)) / RATE
    return out * np.clip(t / 4.0, 0, 1)


def write(t0, t1, path, fade_out=0.0):
    """Writes the slice as a mono 16-bit wav; fade_out > 0 fades the last seconds (the end of the video)."""
    x = render(t0, t1)
    if fade_out > 0:
        n = min(len(x), int(fade_out * RATE))
        x[-n:] *= np.linspace(1, 0, n) ** 2
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


def mix_filter(voice, music, duration, music_db=-6.0):
    """An ffmpeg filter graph: the music under the voice, ducking while someone speaks. Output label [a]."""
    return ('[%s]apad=whole_dur=%.3f,aformat=sample_rates=44100:channel_layouts=stereo,asplit=2[voice][sc];'
            '[%s]aformat=sample_rates=44100:channel_layouts=stereo,volume=%.1fdB[m];'
            '[m][sc]sidechaincompress=threshold=0.012:ratio=3:attack=60:release=900[md];'
            '[voice][md]amix=inputs=2:duration=first:normalize=0[a]' % (voice, duration, music, music_db))


if __name__ == '__main__':
    write(float(sys.argv[1]), float(sys.argv[2]), sys.argv[3])
    print('wrote', sys.argv[3])
