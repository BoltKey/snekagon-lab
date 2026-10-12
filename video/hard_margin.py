"""The hardest proven red reply (241 hours) as a close perfect line, like the cold-open game: red's move 5 is the
closest defence among the engine's 48 favourites, then both sides play for the score (see margin_line.py).
For chapter 10.

    .venv/Scripts/python.exe hard_margin.py       # log in data/hard_margin.log
"""
import os
import sys

import margin_line as ml

ml.LOG = open(os.path.join(ml.HERE, 'data', 'hard_margin.log'), 'w', encoding='utf-8')
sys.argv = [sys.argv[0], '1:60 4B:16,27,28,39 3V:29,40,50 4A:6,7,8,19', '48']
ml.main()
