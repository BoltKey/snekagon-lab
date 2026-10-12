"""Renders the chapters and joins them into one video.

    .venv/Scripts/python.exe build.py draft                    # 480p15, quick check
    .venv/Scripts/python.exe build.py final                    # 1080p60
    .venv/Scripts/python.exe build.py final Ch05Rules Ch06...  # only some chapters
    .venv/Scripts/python.exe build.py final --join             # ... and then join all that exist into one video
    .venv/Scripts/python.exe build.py final join               # only join the renders that exist

Chapters render in parallel, SNEK_JOBS at a time (default: half the cores, at most 6). Joining re-encodes the
whole video (minutes), and the web player uses the chapters directly, so it only runs when asked for.
"""
import os
import subprocess
import time
import sys
from concurrent.futures import ThreadPoolExecutor

import av
import imageio_ffmpeg

import camera
import music

from chapters import CHAPTERS
from chapters2 import CHAPTERS2

SCENES = [('chapters', c) for c in CHAPTERS] + [('chapters2', c) for c in CHAPTERS2]
FILE = dict((c, f) for f, c in SCENES)

HERE = os.path.dirname(os.path.abspath(__file__))
ARGV = [a for a in sys.argv[1:] if a != '--join']
MODE = ARGV[0] if ARGV else 'draft'
ONLY = [] if ARGV[1:] == ['join'] else (ARGV[1:] or [c for _, c in SCENES])  # 'join': only join existing renders
JOIN = '--join' in sys.argv or ARGV[1:] == ['join']
JOBS = int(os.environ.get('SNEK_JOBS', max(1, min(6, (os.cpu_count() or 2) // 2))))
# --disable_caching: a fully cached manim 0.19 render silently loses its audio track
ARGS = {'draft': ['-ql'], 'final': ['-r', '1920,1080', '--fps', '60']}[MODE] + ['--disable_caching']
FOLDER = {'draft': '480p15', 'final': '1080p60'}[MODE]
env = dict(os.environ, PYTHONIOENCODING='utf-8')
manim = os.path.join(HERE, '.venv', 'Scripts', 'manim.exe')



def render(ch):
    # each process gets its own text cache: two processes writing the same svg at once leave a broken file
    t0 = time.time()
    print('rendering', ch, flush=True)
    r = subprocess.run([manim, *ARGS, FILE[ch] + '.py', ch], cwd=HERE, env=dict(env, SNEK_TEXT_SUBDIR=ch),
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        print(r.stdout[-3000:].encode('ascii', 'replace').decode(), r.stderr[-3000:].encode('ascii', 'replace').decode(), flush=True)
    else:
        print('done %s in %.0f s' % (ch, time.time() - t0), flush=True)
    return ch, r.returncode


with ThreadPoolExecutor(JOBS) as pool:
    failed = [ch for ch, code in pool.map(render, ONLY) if code != 0]
# parallel renders starting together can trip over each other creating the same media folders; once more, alone
failed = [ch for ch in failed if render(ch)[1] != 0]
if failed:
    sys.exit('render of %s failed' % ', '.join(failed))
if not JOIN:
    sys.exit(0)

parts = [os.path.join(HERE, 'media', 'videos', f, FOLDER, ch + '.mp4') for f, ch in SCENES]
parts = [p for p in parts if os.path.exists(p)]


def video_seconds(path):
    with av.open(path) as c:
        v = c.streams.video[0]
        return float(v.duration * v.time_base)


# Manim's audio track ends with the last spoken word, the picture goes on (fade-outs, title cards).
# Every chapter's audio is therefore padded with silence to exactly its video length before joining;
# otherwise each chapter's sound would start early and the drift would add up.
for p in parts:
    with av.open(p) as c:
        if not c.streams.audio:
            sys.exit('%s has no audio track - render it again' % os.path.basename(p))
inputs, chains, streams, clips = [], [], [], []
for k, p in enumerate(parts):
    inputs += ['-i', p]
    chains.append('[%d:a]apad=whole_dur=%.3f[a%d]' % (k, video_seconds(p), k))
    # Adam's camera footage over the beats he speaks to the camera (camera.py); its inputs come after the music
    over = camera.overlays(os.path.splitext(os.path.basename(p))[0])
    if over:
        chains.append(camera.overlay_filter('%d:v' % k, len(parts) + 1 + len(clips), over, 'pv%d' % k))
        clips += [c[0] for c in over]
    streams.append('[%s][a%d]' % ('pv%d' % k if over else '%d:v' % k, k))
graph = ';'.join(chains) + ';' + ''.join(streams) + 'concat=n=%d:v=1:a=1[v][narration]' % len(parts)
# the background music (music.py) under the whole video, ducking under the narration
total = sum(video_seconds(p) for p in parts)
music_wav = os.path.join(HERE, '_music_full.wav')
music.write(0.0, total, music_wav, fade_out=5.0)
inputs += ['-i', music_wav]
for c in clips:
    inputs += ['-i', c]
graph += ';' + music.mix_filter('narration', '%d:a' % len(parts), total, -6.0)
out = os.path.join(HERE, 'snekagon_%s_%s.mp4' % (MODE, time.strftime('%m%d-%H%M')))  # new name: an open player locks the old file
subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-y', '-loglevel', 'error', *inputs, '-filter_complex', graph,
                '-map', '[v]', '-map', '[a]', '-c:v', 'libx264', '-preset', 'medium', '-crf', '20',
                '-c:a', 'aac', '-b:a', '160k', out], check=True)
os.remove(music_wav)
print('wrote', out)
