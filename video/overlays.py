"""Overlays laid over Adam's camera footage: Manim scenes rendered with a transparent background, one per narrated
beat. Each result goes to assets/overlays/<key>.mov, where camera.overlays() finds it for share_encode.py and build.py
(it is laid over the frame, above the footage, while that beat is spoken).

    .venv/Scripts/python.exe overlays.py
"""
import glob
import os
import shutil
import subprocess

from script import para
from tts import recording_key

HERE = os.path.dirname(os.path.abspath(__file__))
FOLDER = os.path.join(HERE, 'assets', 'overlays')
manim = os.path.join(HERE, '.venv', 'Scripts', 'manim.exe')
OVERLAYS = {'Ch07YearsOverlay': ('chapters', lambda: para(7, 0))}  # scene: (file, the beat it goes over)


def main():
    os.makedirs(FOLDER, exist_ok=True)
    for scene, (module, text) in OVERLAYS.items():
        env = dict(os.environ, SNEK_TEXT_SUBDIR=scene, PYTHONIOENCODING='utf-8')
        r = subprocess.run([manim, '-t', '-r', '1920,1080', '--fps', '60', '--disable_caching', module + '.py', scene],
                           cwd=HERE, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode != 0:
            print(scene, 'FAILED', r.stdout[-1500:], r.stderr[-1500:])
            continue
        out = max(glob.glob(os.path.join(HERE, 'media', 'videos', module, '1080p60', scene + '.*')), key=os.path.getmtime)
        dst = os.path.join(FOLDER, recording_key(text().strip()) + os.path.splitext(out)[1])
        shutil.copy(out, dst)
        print(scene, '->', os.path.relpath(dst, HERE))


if __name__ == '__main__':
    main()
