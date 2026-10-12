"""Finds moments where something touches or crosses the edge of the frame: samples every chapter video at a
few frames per second and reports frames with content in the outermost pixels, grouped into time spans.

    .venv/Scripts/python.exe edge_check.py [chNN ...]      # default: all chapters in share/
"""
import glob
import os
import subprocess
import sys

import imageio_ffmpeg
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FPS, W, H = 4, 480, 270  # sampled frames, downscaled
BAND = 2                 # outermost pixels checked (of the downscaled frame)
BG = np.array([20, 23, 28])


def frames(path):
    cmd = [imageio_ffmpeg.get_ffmpeg_exe(), '-loglevel', 'error', '-i', path, '-vf', 'fps=%d,scale=%d:%d' % (FPS, W, H),
           '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    raw = subprocess.run(cmd, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, H, W, 3).astype(int)


def edges(frame):
    """Which sides have something other than the background in their outermost pixels."""
    off = np.abs(frame - BG).sum(2) > 40
    sides = {'top': off[:BAND].any(), 'bottom': off[-BAND:].any(), 'left': off[:, :BAND].any(), 'right': off[:, -BAND:].any()}
    return [s for s, hit in sides.items() if hit]


def main(names):
    for path in sorted(glob.glob(os.path.join(HERE, 'share', 'ch*.mp4'))):
        name = os.path.basename(path)[:4]
        if names and name not in names:
            continue
        spans = []
        for k, f in enumerate(frames(path)):
            hit = edges(f)
            if hit:
                t = k / FPS
                if spans and t - spans[-1][1] <= 1.0 / FPS + 1e-6 and spans[-1][2] == hit:
                    spans[-1][1] = t
                else:
                    spans.append([t, t, hit])
        print(name, 'clean' if not spans else '')
        for a, b, hit in spans:
            print('   %5.1f-%5.1f s  %s' % (a, b, ', '.join(hit)))


if __name__ == '__main__':
    main(sys.argv[1:])
