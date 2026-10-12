"""Re-encode rendered scenes for the shared web player (share/chNN.mp4) with the background music mixed in
(music.py: each scene gets its own stretch of one timeline, so the music runs on across scenes), and update
their lengths in share/snekagon_video.html.

    .venv/Scripts/python.exe share_encode.py 1 2 3      # chapter numbers
"""
import glob
import os
import re
import subprocess
import sys

import imageio_ffmpeg

import camera
import music

HERE = os.path.dirname(os.path.abspath(__file__))
FF = imageio_ffmpeg.get_ffmpeg_exe()
HTML = os.path.join(HERE, 'share', 'snekagon_video.html')
MUSIC_DB = -6.0  # measured: about 15 dB under the narration in pauses, a little lower under speech


def duration(path):
    out = subprocess.run([FF, '-hide_banner', '-i', path], capture_output=True, text=True).stderr
    h, m, s = re.search(r'Duration: (\d+):(\d+):([\d.]+)', out).groups()
    return int(h) * 3600 + int(m) * 60 + float(s)


def source(n):
    hits = glob.glob(os.path.join(HERE, 'media', 'videos', '*', '1080p60', 'Ch%02d*.mp4' % n))
    assert len(hits) == 1, hits
    return hits[0]


def main(numbers):
    html = open(HTML, encoding='utf-8').read()
    table = re.search(r'const SCENES = \[(.*?)\];', html, re.S)
    entries = re.findall(r"\['([^']*)', ([\d.]+)\]", table.group(1))
    for n in numbers:
        src = source(n)
        dur = round(duration(src), 1)
        dst = os.path.join(HERE, 'share', 'ch%02d.mp4' % n)
        entries[n - 1] = (entries[n - 1][0], '%.1f' % dur)
        start = sum(float(e[1]) for e in entries[:n - 1])  # where this scene sits on the music's timeline
        tmp = os.path.join(HERE, 'share', '_music_%02d.wav' % n)
        music.write(start, start + dur, tmp, fade_out=5.0 if n == len(entries) else 0.0)
        # Adam's camera footage over the beats he speaks to the camera (camera.py)
        clips = camera.overlays(os.path.splitext(os.path.basename(src))[0])
        graph = music.mix_filter('0:a', '1:a', dur, MUSIC_DB)
        if clips:
            graph += ';' + camera.overlay_filter('0:v', 2, clips, 'v')
        subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-y', '-i', src, '-i', tmp, *[x for c in clips for x in ('-i', c[0])],
                        '-filter_complex', graph, '-map', '[v]' if clips else '0:v', '-map', '[a]',
                        '-r', '30',  # the draft player stays at 30 fps (smaller uploads); the final join keeps 60
                        '-c:v', 'libx264', '-crf', '23', '-preset', 'slow', '-pix_fmt', 'yuv420p',
                        '-c:a', 'aac', '-b:a', '128k', '-t', '%.3f' % dur, '-movflags', '+faststart', dst], check=True)
        os.remove(tmp)
        print('ch%02d  %.1f s  %.1f MB' % (n, dur, os.path.getsize(dst) / 1e6))
    rows = ', '.join("['%s', %s]" % e for e in entries)
    html = html[:table.start(1)] + rows + html[table.end(1):]
    total = sum(float(e[1]) for e in entries)
    html = re.sub(r'Draft video · \d+ min', 'Draft video · %d min' % round(total / 60), html)
    open(HTML, 'w', encoding='utf-8').write(html)
    print('total %d:%02d' % (total // 60, total % 60))


if __name__ == '__main__':
    main([int(a) for a in sys.argv[1:]])
