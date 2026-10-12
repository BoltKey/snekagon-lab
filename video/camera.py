"""Footage of Adam speaking to the camera, for the passages in script.ON_CAMERA.

    .venv/Scripts/python.exe camera.py <video> "<start of the passage>"          # prepare one clip
    .venv/Scripts/python.exe camera.py <video> "<start of the passage>" 16.4     # ... with the speech ending at 16.4 s
    .venv/Scripts/python.exe camera.py <video> "<start of the passage>" --after 60   # the last take, from 60 s on

Finds the speech in the video (the longest stretch of sound, pauses under 1.2 s included) and cuts picture and
sound at the same points (0.15 s before, 0.3 s after, like the recorder). The sound is cleaned like the recorder's
takes and saved as recordings/<key>.wav, so the scene is timed by it; the picture goes to assets/camera/<key>.mp4.
share_encode.py and build.py lay that picture over the whole frame while the beat is spoken (the render writes when
each beat starts to data/timeline/<Scene>.json); the scene itself leaves the frame empty there (snekviz.camera_clip).
"""
import glob
import json
import os
import re
import subprocess
import sys

import imageio_ffmpeg

HERE = os.path.dirname(os.path.abspath(__file__))
FF = imageio_ffmpeg.get_ffmpeg_exe()
FOLDER = os.path.join(HERE, 'assets', 'camera')
CLEAN = 'highpass=f=70,afftdn=nr=8:nf=-45,loudnorm=I=-18:TP=-1.5:LRA=11'  # the recorder's cleaning, without trimming
FADE = 0.3  # the footage fades in and out over this many seconds


def speech_span(video, after=0.0):
    """(start, end) of the speech: the longest stretch of sound, pauses shorter than 1.2 s included."""
    log = subprocess.run([FF, '-hide_banner', '-i', video, '-vn', '-af', 'silencedetect=noise=-38dB:d=0.4', '-f', 'null', '-'],
                         capture_output=True, text=True).stderr
    h, m, s = re.search(r'Duration: (\d+):(\d+):([\d.]+)', log).groups()
    total = int(h) * 3600 + int(m) * 60 + float(s)
    starts = [float(x) for x in re.findall(r'silence_start: ([\d.]+)', log)]
    ends = [float(x) for x in re.findall(r'silence_end: ([\d.]+)', log)]
    silences = list(zip(starts, ends + [total] * (len(starts) - len(ends))))
    sounds, t = [], 0.0
    for a, b in silences:
        if a > t:
            sounds.append([t, a])
        t = b
    if t < total:
        sounds.append([t, total])
    sounds = [r for r in sounds if r[1] - r[0] >= 0.2 and r[0] >= after]  # clicks are shorter than a word; earlier takes before `after`
    merged = []
    for a, b in sounds:
        if merged and a - merged[-1][1] < 1.2:
            merged[-1][1] = b
        else:
            merged.append([a, b])
    return tuple(max(merged, key=lambda r: r[1] - r[0]))


def prepare(video, prefix, end_at=None, after=0.0):
    beats = [b for b in json.load(open(os.path.join(HERE, 'data', 'beats.json'), encoding='utf-8')) if b['text'].startswith(prefix)]
    if len(beats) != 1:
        sys.exit('%d beats start with "%s"' % (len(beats), prefix))
    key = beats[0]['key']
    start, end = speech_span(video, after)
    if end_at is not None:  # noise after the last word that the detection kept
        end = min(end, end_at)
    a, z = max(0.0, start - 0.15), end + 0.3
    os.makedirs(FOLDER, exist_ok=True)
    os.makedirs(os.path.join(HERE, 'recordings'), exist_ok=True)
    cut = ['-ss', '%.3f' % a, '-to', '%.3f' % z, '-i', video]
    subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-y', *cut, '-vn', '-af', CLEAN, '-ar', '44100', '-ac', '1',
                    os.path.join(HERE, 'recordings', key + '.wav')], check=True)
    subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-y', *cut, '-an',
                    '-vf', 'scale=1920:1080:force_original_aspect_ratio=increase,crop=1920:1080,fps=30,format=yuv420p',
                    '-c:v', 'libx264', '-crf', '18', '-preset', 'medium', os.path.join(FOLDER, key + '.mp4')], check=True)
    print('%s: speech %.2f-%.2f s, clip %.2f s -> recordings/%s.wav, assets/camera/%s.mp4' % (
        beats[0]['scene'], start, end, z - a, key, key))


def overlays(scene):
    """(clip, start, duration) for every beat of a rendered scene that has camera footage or an overlay."""
    path = os.path.join(HERE, 'data', 'timeline', scene + '.json')
    if not os.path.exists(path):
        return []
    beats = json.load(open(path, encoding='utf-8'))['beats']
    out = []
    for b in beats:
        # the footage first, then any overlay drawn over it (overlays.py: a transparent Manim render)
        for clip in [os.path.join(FOLDER, b['key'] + '.mp4')] + glob.glob(os.path.join(HERE, 'assets', 'overlays', b['key'] + '.*')):
            if os.path.exists(clip):
                out.append((clip, b['start'], b['duration']))
    return out


def overlay_filter(video, first_input, items, out):
    """An ffmpeg filter chain laying each clip (inputs first_input, first_input + 1, ...) over [video] at its start,
    fading in and out; the result is labelled [out]. Its inner labels carry `out`, so several chains can share a graph."""
    chain, current = [], video
    for j, (_, start, duration) in enumerate(items):
        chain.append('[%d:v]format=yuva420p,fade=t=in:st=0:d=%.2f:alpha=1,fade=t=out:st=%.3f:d=%.2f:alpha=1,'
                     'setpts=PTS-STARTPTS+%.3f/TB[%s_cam%d]' % (first_input + j, FADE, max(0.0, duration - FADE), FADE, start, out, j))
        label = out if j == len(items) - 1 else '%s_over%d' % (out, j)
        chain.append('[%s][%s_cam%d]overlay=eof_action=pass[%s]' % (current, out, j, label))
        current = label
    return ';'.join(chain)


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('prefix', help='the start of the passage')
    ap.add_argument('end', nargs='?', type=float, help='the speech ends here (noise after the last word)')
    ap.add_argument('--after', type=float, default=0.0, help='look for the speech only from here on (the last take)')
    a = ap.parse_args()
    prepare(a.video, a.prefix, a.end, a.after)
