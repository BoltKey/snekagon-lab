"""YouTube subtitles (.srt) for the joined video, from what the render recorded (data/timeline/<Scene>.json).

    .venv/Scripts/python.exe subtitles.py      # writes snekagon_en.srt

Each beat's text is split into short cues (two lines of at most 42 characters). A cue's time comes from the
recording: the text is spread over the stretches where the voice is actually heard (pauses left out), so a cue
appears when its words are spoken. The scenes follow each other as in build.py's join, each as long as its video.
"""
import json
import os
import re
import wave

import av
import numpy as np

import declick
from chapters import CHAPTERS
from chapters2 import CHAPTERS2

HERE = os.path.dirname(os.path.abspath(__file__))
SCENES = [('chapters', c) for c in CHAPTERS] + [('chapters2', c) for c in CHAPTERS2]
LINE = 42


def video_seconds(path):
    with av.open(path) as c:
        v = c.streams.video[0]
        return float(v.duration * v.time_base)


def voiced(key, duration):
    """The stretches (start, end) in seconds where the take is heard; pauses of 0.25 s or more left out."""
    path = os.path.join(HERE, 'recordings', key + '.wav')
    if not os.path.exists(path):
        return [(0.0, duration)]
    w = wave.open(path)
    sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float) / 32768
    if w.getnchannels() > 1:
        x = x.reshape(-1, w.getnchannels()).mean(1)
    spans = []
    for a, b in declick.segments(x, sr):
        if spans and a / sr - spans[-1][1] < 0.25:
            spans[-1][1] = b / sr
        else:
            spans.append([a / sr, b / sr])
    return [tuple(s) for s in spans] or [(0.0, duration)]


def at(spans, f):
    """The time at fraction f of the voiced time."""
    total = sum(b - a for a, b in spans)
    left = f * total
    for a, b in spans:
        if left <= b - a:
            return a + left
        left -= b - a
    return spans[-1][1]


def chunks(text):
    """The text in cues of at most two lines: sentences, long ones split at commas and colons, then by words."""
    out = []
    for sentence in re.split(r'(?<=[.?!])\s+', text.strip()):
        parts = [sentence]
        while any(len(p) > 2 * LINE for p in parts):
            p = next(p for p in parts if len(p) > 2 * LINE)
            i = parts.index(p)
            cuts = [m.end() for m in re.finditer(r'[,:;—]\s', p)]
            mid = len(p) / 2
            if cuts and min(abs(c - mid) for c in cuts) < len(p) * 0.3:
                c = min(cuts, key=lambda c: abs(c - mid))
            else:
                spaces = [m.start() for m in re.finditer(r' ', p)]
                c = min(spaces, key=lambda c: abs(c - mid)) + 1
            parts[i:i + 1] = [p[:c].strip(), p[c:].strip()]
        out += parts
    return out


def lines(cue):
    """One line, or two balanced ones."""
    if len(cue) <= LINE:
        return cue
    spaces = [m.start() for m in re.finditer(r' ', cue)]
    c = min(spaces, key=lambda c: max(c, len(cue) - c - 1))
    return cue[:c] + '\n' + cue[c + 1:]


def stamp(t):
    ms = int(round(t * 1000))
    return '%02d:%02d:%02d,%03d' % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)


def main():
    cues, offset = [], 0.0
    for folder, scene in SCENES:
        beats = json.load(open(os.path.join(HERE, 'data', 'timeline', scene + '.json'), encoding='utf-8'))['beats']
        for b in beats:
            spans = voiced(b['key'], b['duration'])
            text = b['text'].strip()
            pos = 0
            for c in chunks(text):
                i = text.index(c, pos)
                j = i + len(c)
                pos = j
                cues.append([offset + b['start'] + at(spans, i / len(text)),
                             offset + b['start'] + at(spans, j / len(text)) + 0.3, lines(c)])
        offset += video_seconds(os.path.join(HERE, 'media', 'videos', folder, '1080p60', scene + '.mp4'))
    for k in range(len(cues) - 1):  # a cue gives way to the next one; and stays up long enough to be read
        cues[k][1] = min(max(cues[k][1], cues[k][0] + 1.0), cues[k + 1][0] - 0.05)
    out = os.path.join(HERE, 'snekagon_en.srt')
    with open(out, 'w', encoding='utf-8') as f:
        for n, (a, b, t) in enumerate(cues, 1):
            f.write('%d\n%s --> %s\n%s\n\n' % (n, stamp(a), stamp(b), t))
    with open(os.path.join(HERE, 'snekagon_en.vtt'), 'w', encoding='utf-8') as f:  # WebVTT, which YouTube also takes
        f.write('WEBVTT\n\n')
        for a, b, t in cues:
            f.write('%s --> %s\n%s\n\n' % (stamp(a).replace(',', '.'), stamp(b).replace(',', '.'), t))
    # plain ASCII (straight quotes) in YouTube's own SubViewer format, for when an upload of the others fails
    plain = str.maketrans({'’': "'", '‘': "'", '“': '"', '”': '"', '—': '-', '–': '-'})
    with open(os.path.join(HERE, 'snekagon_en.sbv'), 'w', encoding='ascii', errors='replace') as f:
        for a, b, t in cues:
            sbv = lambda s: stamp(s)[1:].replace(',', '.')  # H:MM:SS.mmm
            f.write('%s,%s\n%s\n\n' % (sbv(a), sbv(b), t.translate(plain)))
    print('wrote', out, ', .vtt and .sbv -', len(cues), 'cues, last ends at', stamp(cues[-1][1]))


if __name__ == '__main__':
    main()
