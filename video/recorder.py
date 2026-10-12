"""Narration recorder: a small local web page that shows every beat of the video (data/beats.json, made by
beats.py), records it from the microphone and saves it as recordings/<key>.wav, cleaned up (a gentle high-pass
and noise reduction, silence trimmed at both ends, loudness normalised). tts.narrate uses a recording wherever
one exists, so the next render follows Adam's own timing.

Small text edits made in the recorder go straight into script_snapshot.md (what the video is rendered from) and
into data/script_edits.jsonl, from where Claude copies them into the script doc (the recorder cannot reach it).
An edit that removes a phrase the animation is synced to (self.until / split_at in chapters*.py) asks first.

    .venv/Scripts/python.exe recorder.py        # then open http://localhost:8765 in Chrome or Edge
"""
import ast
import datetime
import http.server
import json
import os
import re
import subprocess
import urllib.parse
import wave

import imageio_ffmpeg

import declick
from script import on_camera

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 8765
REC = os.path.join(HERE, 'recordings')
RAW = os.path.join(REC, 'raw')  # the browser's original takes, kept in case the cleaning ever needs redoing
FF = imageio_ffmpeg.get_ffmpeg_exe()
CLEAN = ('highpass=f=70,afftdn=nr=8:nf=-45,'
         'silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.15,areverse,'
         'silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.3,areverse,'
         'loudnorm=I=-18:TP=-1.5:LRA=11')
KEY = re.compile(r'^[0-9a-f]{16}$')
BEATS = os.path.join(HERE, 'data', 'beats.json')
SNAPSHOT = os.path.join(HERE, 'script_snapshot.md')
EDITS = os.path.join(HERE, 'data', 'script_edits.jsonl')
STRING = r"'(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\""  # a Python string literal in single or double quotes


def sync_phrases():
    """The phrases the animation waits for (self.until) or splits paragraphs at (split_at)."""
    out = set()
    for name in ('chapters.py', 'chapters2.py'):
        src = open(os.path.join(HERE, name), encoding='utf-8').read()
        for m in re.finditer(r'\buntil\((' + STRING + r')\)', src):  # self.until, or a local alias of it
            out.add(ast.literal_eval(m.group(1)))
        for m in re.finditer(r'split_at\([^\[]*\[([^\]]*)\]\)', src):
            for q in re.finditer(STRING, m.group(1)):
                out.add(ast.literal_eval(q.group(0)))
    return out


def pending_edits():
    return sum(1 for _ in open(EDITS, encoding='utf-8')) if os.path.exists(EDITS) else 0


def duration(path):
    with wave.open(path, 'rb') as w:
        return round(w.getnframes() / w.getframerate(), 2)


def beats():
    out = json.load(open(BEATS, encoding='utf-8'))
    snapshot = open(SNAPSHOT, encoding='utf-8').read()
    for b in out:
        # quotes and the countdown are written in the video's code, not in the script: no edits here
        b['editable'] = snapshot.count(b['text']) == 1
        rec = os.path.join(REC, b['key'] + '.wav')
        b['recorded'] = duration(rec) if os.path.exists(rec) else None
        b['ttsDuration'] = duration(os.path.join(HERE, b['tts']))
        b['camera'] = on_camera(b['text'])  # spoken to the camera, recorded as video instead
    return out


class Handler(http.server.BaseHTTPRequestHandler):
    def send(self, code, body, kind):
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value, code=200):
        self.send(code, json.dumps(value, ensure_ascii=False).encode('utf-8'), 'application/json; charset=utf-8')

    def send_file(self, path, kind):
        if path and os.path.exists(path):
            self.send(200, open(path, 'rb').read(), kind)
        else:
            self.send_json({'error': 'not found'}, 404)

    def key(self, query):
        k = query.get('key', [''])[0]
        return k if KEY.match(k) else None

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path == '/':
            self.send_file(os.path.join(HERE, 'recorder.html'), 'text/html; charset=utf-8')
        elif url.path == '/beats':
            self.send_json({'beats': beats(), 'pendingEdits': pending_edits()})
        elif url.path == '/audio' and self.key(query):
            self.send_file(os.path.join(REC, self.key(query) + '.wav'), 'audio/wav')
        elif url.path == '/tts' and self.key(query):
            tts = next((b['tts'] for b in beats() if b['key'] == self.key(query)), None)
            self.send_file(os.path.join(HERE, tts) if tts else '', 'audio/wav')
        else:
            self.send_json({'error': 'not found'}, 404)

    def edit(self, k, query):
        """Replaces the text of beat k (the new text is the request body) in the snapshot and the beat list."""
        new = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode('utf-8').strip()
        all_beats = json.load(open(BEATS, encoding='utf-8'))
        beat = next((b for b in all_beats if b['key'] == k), None)
        if not beat or not new:
            return self.send_json({'error': 'unknown beat or empty text'}, 400)
        old = beat['text']
        if new == old:
            return self.send_json({'beat': beat, 'pendingEdits': pending_edits(), 'syncPhrasesLost': []})
        lost = sorted(p for p in sync_phrases() if p in old and p not in new)
        if lost and query.get('confirm', [''])[0] != '1':
            return self.send_json({'needsConfirm': lost})
        snapshot = open(SNAPSHOT, encoding='utf-8').read()
        if snapshot.count(old) != 1:
            why = ('it is written in the video’s code (a quote or the countdown), not in the script'
                   if not snapshot.count(old) else 'it appears %d times in the script' % snapshot.count(old))
            return self.send_json({'error': 'this text cannot be edited here: %s. Ask Claude to change it.' % why}, 409)
        with open(SNAPSHOT + '.tmp', 'w', encoding='utf-8') as f:
            f.write(snapshot.replace(old, new))
        os.replace(SNAPSHOT + '.tmp', SNAPSHOT)
        os.environ['SNEK_TTS_ONLY'] = '1'  # the synthetic reference, even if the new text has a recording already
        from tts import narrate, recording_key  # Piper is loaded only when it is needed
        beat['text'], beat['key'] = new, recording_key(new)
        beat['tts'] = os.path.relpath(narrate(new)[0], HERE)
        json.dump(all_beats, open(BEATS, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
        with open(EDITS, 'a', encoding='utf-8') as f:
            record = {'time': datetime.datetime.now().isoformat(timespec='seconds'), 'scene': beat['scene'],
                      'old': old, 'new': new, 'syncPhrasesLost': lost}
            f.write(json.dumps(record, ensure_ascii=False) + '\n')
        rec = os.path.join(REC, beat['key'] + '.wav')
        beat['recorded'] = duration(rec) if os.path.exists(rec) else None
        beat['ttsDuration'] = duration(os.path.join(HERE, beat['tts']))
        self.send_json({'beat': beat, 'pendingEdits': pending_edits(), 'syncPhrasesLost': lost})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        k = self.key(query)
        if not k:
            return self.send_json({'error': 'bad key'}, 400)
        if url.path == '/edit':
            return self.edit(k, query)
        if url.path == '/save':
            os.makedirs(RAW, exist_ok=True)
            raw = os.path.join(RAW, k + '.webm')
            raw_tmp = raw + '.tmp.webm'
            with open(raw_tmp, 'wb') as f:
                f.write(self.rfile.read(int(self.headers.get('Content-Length', 0))))
            out = os.path.join(REC, k + '.wav')
            tmp = out + '.tmp.wav'
            r = subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-y', '-i', raw_tmp, '-af', CLEAN,
                                '-ar', '44100', '-ac', '1', tmp], capture_output=True, text=True)
            if r.returncode != 0:
                os.remove(raw_tmp)
                return self.send_json({'error': r.stderr[-500:]}, 500)
            declick.trim_taps(tmp, backup=False)  # the space bar that started the take
            if duration(tmp) < 0.5:  # nothing but silence: the microphone gave no sound; keep the previous take
                os.remove(tmp)
                os.remove(raw_tmp)
                return self.send_json({'error': 'the take is silent, so it was not saved and the previous take is kept. '
                                                'Check the microphone (the browser may be using a different one, or it is muted).'}, 422)
            os.replace(raw_tmp, raw)
            os.replace(tmp, out)
            self.send_json({'key': k, 'recorded': duration(out)})
        elif url.path == '/delete':
            for path in (os.path.join(REC, k + '.wav'), os.path.join(RAW, k + '.webm')):
                if os.path.exists(path):
                    os.remove(path)
            self.send_json({'key': k, 'recorded': None})
        else:
            self.send_json({'error': 'not found'}, 404)

    def log_message(self, *args):
        pass


if __name__ == '__main__':
    os.makedirs(REC, exist_ok=True)
    print('recorder: http://localhost:%d  (Chrome or Edge; recordings go to %s)' % (PORT, REC), flush=True)
    http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
