"""Lists every narrated beat of the video, in order, for the recorder: runs each scene as a dry run (no frames,
Piper cached) and collects what it narrates into data/beats.json.

    .venv/Scripts/python.exe beats.py                    # every scene
    .venv/Scripts/python.exe beats.py Ch12WhyHard        # only some scenes (after a text edit); the rest stays
"""
import json
import os
import subprocess
import sys

from chapters import CHAPTERS
from chapters2 import CHAPTERS2

SCENES = [('chapters', c) for c in CHAPTERS] + [('chapters2', c) for c in CHAPTERS2]  # as in build.py
FILE = dict((c, f) for f, c in SCENES)

HERE = os.path.dirname(os.path.abspath(__file__))
LINES = os.path.join(HERE, 'data', 'beats.jsonl')
manim = os.path.join(HERE, '.venv', 'Scripts', 'manim.exe')


def main():
    only = sys.argv[1:]
    if os.path.exists(LINES):
        os.remove(LINES)
    for _, scene in SCENES:
        if only and scene not in only:
            continue
        env = dict(os.environ, SNEK_BEATS=LINES, SNEK_SCENE=scene, SNEK_TEXT_SUBDIR=scene, PYTHONIOENCODING='utf-8')
        r = subprocess.run([manim, '--dry_run', '-ql', FILE[scene] + '.py', scene], cwd=HERE, env=env,
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        print(scene, 'ok' if r.returncode == 0 else 'FAILED ' + r.stderr[-1500:], flush=True)
    fresh, seen = {}, set()
    for line in open(LINES, encoding='utf-8'):
        b = json.loads(line)
        if b['key'] not in seen:  # a beat narrated twice (it never is, but be safe) is recorded once
            seen.add(b['key'])
            fresh.setdefault(b['scene'], []).append(b)
    os.remove(LINES)
    path = os.path.join(HERE, 'data', 'beats.json')
    old = json.load(open(path, encoding='utf-8')) if only and os.path.exists(path) else []
    beats = []
    for _, scene in SCENES:  # keep the scene order; scenes not run again keep their old beats
        beats += fresh.get(scene) or [b for b in old if b['scene'] == scene]
    json.dump(beats, open(path, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print(len(beats), 'beats')


if __name__ == '__main__':
    main()
