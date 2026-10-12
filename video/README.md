# Snekagon video (3Blue1Brown style)

Animations with [Manim Community](https://www.manim.community/), narration with [Piper](https://github.com/OHF-Voice/piper1-gpl) (offline neural TTS).
The script lives in the Claude Docs document "Snekagon: the 7 turn game that could not be solved";
`script_snapshot.md` is a local copy of it.

```bash
python -m venv video/.venv
video/.venv/Scripts/python.exe -m pip install manim piper-tts
cd video && .venv/Scripts/python.exe -m piper.download_voices --download-dir voices en_US-ryan-high
PYTHONIOENCODING=utf-8 .venv/Scripts/manim -r 1920,1080 --fps 30 pilot.py Pilot     # final
PYTHONIOENCODING=utf-8 .venv/Scripts/manim -ql pilot.py Pilot                        # quick draft
```

- `tts.py` - narration: voice `SNEK_VOICE` (default en_US-ryan-high), tempo `SNEK_SPEED`, pitch `SNEK_PITCH`
  (0.9 = a bit deeper). Audio is cached in `audio_cache/` by text, so re-rendering is cheap.
- `snekviz.py` - shared visuals: the board (same cell numbering as the engine), pieces, the example
  game, and `Narrated.beat(text, action)`, which plays one narration paragraph and runs the animation
  that belongs to it within the paragraph's duration.
- `pilot.py` - section 1 (cold open), title card, section 5 (rules).
