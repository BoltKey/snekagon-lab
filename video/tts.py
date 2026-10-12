"""Narration: Adam's own recordings where they exist (recordings/<key>.wav, made with recorder.py), otherwise
Piper (offline neural TTS). Piper output is cached: the same text and voice give the same file.

With SNEK_BEATS=<file> every narrated text is also appended to that file (one JSON line per beat, with the
scene from SNEK_SCENE): beats.py uses it to list every beat of the video for the recorder.
"""
import hashlib
import json
import os
import wave

from piper import PiperVoice, SynthesisConfig

HERE = os.path.dirname(os.path.abspath(__file__))
VOICE = os.environ.get('SNEK_VOICE', 'en_US-ryan-high')
SPEED = float(os.environ.get('SNEK_SPEED', '1.12'))  # Piper length scale: > 1 = slower, calmer
# Pitch factor < 1 makes the voice deeper: the audio is synthesised faster and then played at a lower
# sample rate (pitch and formants drop together, tempo stays SPEED).
PITCH = float(os.environ.get('SNEK_PITCH', '0.9'))
CACHE = os.path.join(HERE, 'audio_cache')
RECORDINGS = os.path.join(HERE, 'recordings')

_voices = {}


def _voice(name):
    if name not in _voices:
        _voices[name] = PiperVoice.load(os.path.join(HERE, 'voices', name + '.onnx'))
    return _voices[name]


def recording_key(text):
    """The file name of Adam's recording of this text: it changes when the text changes."""
    return hashlib.sha1(text.strip().encode('utf-8')).hexdigest()[:16]


def _duration(path):
    with wave.open(path, 'rb') as wf:
        return wf.getnframes() / wf.getframerate()


def _piper(text, voice, speed, pitch):
    os.makedirs(CACHE, exist_ok=True)
    key = hashlib.sha1(('%s|%s|%s|%s' % (voice, speed, pitch, text)).encode('utf-8')).hexdigest()[:16]
    path = os.path.join(CACHE, key + '.wav')
    if not os.path.exists(path):
        cfg = SynthesisConfig(length_scale=speed * pitch, noise_scale=0.6, noise_w_scale=0.7)
        with wave.open(path + '.raw', 'wb') as wf:
            _voice(voice).synthesize_wav(text, wf, syn_config=cfg)
        with wave.open(path + '.raw', 'rb') as src:
            params, frames = src.getparams(), src.readframes(src.getnframes())
        with wave.open(path + '.tmp', 'wb') as dst:
            dst.setparams(params)
            dst.setframerate(int(round(params.framerate * pitch)))
            dst.writeframes(frames)
        os.remove(path + '.raw')
        os.replace(path + '.tmp', path)
    return path


def narrate(text, voice=VOICE, speed=SPEED, pitch=PITCH):
    """Returns (wav path, duration in seconds) for the spoken text."""
    tts = _piper(text, voice, speed, pitch)
    rec = os.path.join(RECORDINGS, recording_key(text) + '.wav')
    if os.environ.get('SNEK_BEATS'):
        with open(os.environ['SNEK_BEATS'], 'a', encoding='utf-8') as f:
            f.write(json.dumps({'scene': os.environ.get('SNEK_SCENE', ''), 'key': recording_key(text), 'text': text,
                                'tts': os.path.relpath(tts, HERE)}, ensure_ascii=False) + '\n')
    path = rec if os.path.exists(rec) and not os.environ.get('SNEK_TTS_ONLY') else tts
    return path, _duration(path)


if __name__ == '__main__':
    sample = ("So here is the question that has been bugging me for years. If both players play perfectly, "
              "who wins? It feels like a question a computer should answer over lunch.")
    import shutil
    os.makedirs(os.path.join(HERE, 'samples'), exist_ok=True)
    for v, short in (('en_GB-alan-medium', 'alan'), ('en_US-ryan-high', 'ryan')):
        for pitch, tag in ((1.0, 'normal'), (0.9, 'deeper')):
            p, d = narrate(sample, voice=v, pitch=pitch)
            out = os.path.join(HERE, 'samples', 'voice_%s_%s.wav' % (short, tag))
            shutil.copy(p, out)
            print(out, '%.1f s' % d)
