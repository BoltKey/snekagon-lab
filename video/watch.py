"""Waits for new takes from the recorder and renders the scenes they belong to.

    .venv/Scripts/python.exe watch.py

Watches recordings/*.wav. Once something has changed and nothing more has for QUIET seconds (a recording session
is over), it renders the scenes of the changed beats (build.py final) and encodes them for the player
(share_encode.py), then exits, so whoever started it hears that it is done. If the recorder queued text edits
(data/script_edits.jsonl), it exits before rendering: those go into the script doc first, and a changed sync phrase
would break the render.
"""
import glob
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QUIET = 180
PY = sys.executable


def takes():
    return {os.path.basename(p)[:-4]: os.path.getmtime(p) for p in glob.glob(os.path.join(HERE, 'recordings', '*.wav'))}


def scenes_of(keys):
    beats = json.load(open(os.path.join(HERE, 'data', 'beats.json'), encoding='utf-8'))
    found = {b['scene'] for b in beats if b['key'] in keys}
    for path in glob.glob(os.path.join(HERE, 'data', 'timeline', '*.json')):  # keys of beats whose text was edited since
        if any(b['key'] in keys for b in json.load(open(path, encoding='utf-8'))['beats']):
            found.add(os.path.basename(path)[:-5])
    return sorted(found)


def main():
    before, changed, last = takes(), set(), None
    print('watching recordings/ ...', flush=True)
    while True:
        time.sleep(10)
        now = takes()
        diff = {k for k in set(before) | set(now) if before.get(k) != now.get(k)}
        if diff:
            print(time.strftime('%H:%M'), 'changed:', ' '.join(sorted(diff)), flush=True)
            changed |= diff
            before, last = now, time.time()
        if changed and time.time() - last >= QUIET:
            break
    edits = os.path.join(HERE, 'data', 'script_edits.jsonl')
    if os.path.exists(edits) and open(edits, encoding='utf-8').read().strip():
        print('text edits are waiting to go into the script doc; not rendering:', ' '.join(scenes_of(changed)), flush=True)
        return
    import declick  # the space-bar taps, in case the recorder that saved the takes is an older one
    for k in sorted(changed):
        path = os.path.join(HERE, 'recordings', k + '.wav')
        if os.path.exists(path) and declick.trim_taps(path):
            print('trimmed the taps off', k, flush=True)
    scenes = scenes_of(changed)
    if not scenes:
        print('the changed takes belong to no scene', flush=True)
        return
    print(time.strftime('%H:%M'), 'rendering', ' '.join(scenes), flush=True)
    at_start = takes()
    r = subprocess.run([PY, 'build.py', 'final', *scenes], cwd=HERE, env={**os.environ, 'SNEK_JOBS': '3'},
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    tail = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith('done ') or 'failed' in l or 'Error' in l]
    print('\n'.join(tail[-20:]), flush=True)
    numbers = [str(int(s[2:4])) for s in scenes]
    subprocess.run([PY, 'share_encode.py', *numbers], cwd=HERE)
    print(time.strftime('%H:%M'), 'done:', ' '.join(scenes), flush=True)
    late = takes()
    late = {k for k in set(at_start) | set(late) if at_start.get(k) != late.get(k)}
    if late:  # recorded while the render ran: not in it
        print('changed during the render, not in it:', ' '.join(sorted(late)), '->', ' '.join(scenes_of(late)), flush=True)


if __name__ == '__main__':
    main()
