"""Cuts off the space-bar taps the recorder left at the start (and end) of takes.

    .venv/Scripts/python.exe declick.py          # list what it would silence
    .venv/Scripts/python.exe declick.py --apply  # do it (originals go to recordings/_before_declick/)

A tap is a short (<= 250 ms) bright burst away from the speech. Taps before the first word (or
after the last one) are cut off, keeping the recorder's margins, as if the take had started without them.
"""
import glob
import json
import os
import shutil
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def segments(x, sr):
    f = int(sr * 0.01)
    rms = np.sqrt(np.convolve(x ** 2, np.ones(f) / f, 'same'))[::f]
    on = 20 * np.log10(rms + 1e-9) > -42
    segs, i = [], 0
    while i < len(on):
        if on[i]:
            j = i
            while j < len(on) and on[j]:
                j += 1
            segs.append([i * f, j * f])
            i = j
        else:
            i += 1
    merged = []
    for a, b in segs:  # one sound with dips under 60 ms
        if merged and a - merged[-1][1] < 0.06 * sr:
            merged[-1][1] = b
        else:
            merged.append([a, b])
    return merged


def is_tap(x, sr, a, b):
    seg = x[a:b]
    if b - a > 0.25 * sr or len(seg) < 16:  # a press and its release can merge into one sound
        return False
    sp = np.abs(np.fft.rfft(seg))
    fr = np.fft.rfftfreq(len(seg), 1 / sr)
    bright = sp[fr > 3000].sum() / (sp.sum() + 1e-9)
    return bright > 0.6 and 20 * np.log10(np.abs(seg).max() + 1e-9) > -34


def keep(x, sr):
    """(from, to) samples to keep: without the taps before the first word and after the last one, with the
    recorder's margins (0.15 s before the speech, 0.3 s after it); None when there is no tap.
    The short sounds before the first word (a key press and its release, often two) go if one of them is a tap and
    the word comes at least 0.12 s later; after the last word the gap must be 0.3 s, so a final "t" stays."""
    segs = segments(x, sr)
    speech = [i for i, (a, b) in enumerate(segs) if b - a > 0.15 * sr and not is_tap(x, sr, a, b)]
    if not speech:
        return None
    first, last = speech[0], speech[-1]
    lead, trail = segs[:first], segs[last + 1:]
    cut_lead = lead and any(is_tap(x, sr, *t) for t in lead) and segs[first][0] - lead[-1][1] >= 0.12 * sr
    cut_trail = trail and any(is_tap(x, sr, *t) for t in trail) and trail[0][0] - segs[last][1] >= 0.3 * sr
    if not (cut_lead or cut_trail):
        return None
    # the margins stop short of the taps themselves
    a = max(lead[-1][1] + int(0.03 * sr), segs[first][0] - int(0.15 * sr)) if cut_lead else 0
    b = min(trail[0][0] - int(0.03 * sr), segs[last][1] + int(0.3 * sr)) if cut_trail else len(x)
    return a, b


def trim_taps(p, apply=True, backup=True):
    """Cuts the taps off one 16-bit mono wav, as if the take had started and ended without them (the original
    goes to recordings/_before_declick/ first, unless backup is False); returns the kept part in seconds of the original, or None."""
    w = wave.open(p)
    params, raw = w.getparams(), w.readframes(w.getnframes())
    w.close()
    if params.sampwidth != 2 or params.nchannels != 1:
        return None
    x = np.frombuffer(raw, np.int16).astype(float) / 32768
    sr = params.framerate
    k = keep(x, sr)
    if k and apply:
        folder = os.path.join(HERE, 'recordings', '_before_declick')
        if backup and not os.path.exists(os.path.join(folder, os.path.basename(p))):
            os.makedirs(folder, exist_ok=True)
            shutil.copy(p, folder)
        y = x[k[0]:k[1]].copy()
        n = min(int(0.01 * sr), len(y))
        y[:n] *= np.linspace(0, 1, n)  # no click from the cut itself
        y[len(y) - n:] *= np.linspace(1, 0, n)
        w = wave.open(p, 'wb')
        w.setparams(params)
        w.writeframes((np.clip(y, -1, 1) * 32767).astype(np.int16).tobytes())
        w.close()
    return k and (k[0] / sr, k[1] / sr)


def main(apply):
    beats = {b['key']: b for b in json.load(open(os.path.join(HERE, 'data', 'beats.json'), encoding='utf-8'))}
    cam = {os.path.basename(p)[:-4] for p in glob.glob(os.path.join(HERE, 'assets', 'camera', '*.mp4'))}
    scenes = set()
    for p in sorted(glob.glob(os.path.join(HERE, 'recordings', '*.wav'))):
        key = os.path.basename(p)[:-4]
        if key not in beats or key in cam:  # old takes; the camera clips carry their own sound
            continue
        k = trim_taps(p, apply)
        if not k:
            continue
        scenes.add(beats[key]['scene'])
        print('%s %-20s keeps %.2f-%.2f s | %s' % (key, beats[key]['scene'], k[0], k[1], beats[key]['text'][:45]))
    print('scenes:', ' '.join(sorted(scenes)))


if __name__ == '__main__':
    main('--apply' in sys.argv)
