"""The hardest proven red reply (241 hours: red 3V:29,40,50, blue's winning answer 4A:6,7,8,19) as a close,
natural-looking game for chapter 10: red's move 5 grows its own snake (no new snake far away), chosen as the
one among the engine's favourite growing moves that loses by the fewest cells; after that both sides play
perfectly for the score (margin_line.py).

    .venv/Scripts/python.exe hard_close.py       # log in data/hard_close.log, result in data/hard_close.json
"""
import json
import os

import margin_line as ml
from snekviz import move_cells, neighbours, position_after

ml.LOG = open(os.path.join(ml.HERE, 'data', 'hard_close.log'), 'w', encoding='utf-8')
START = ['1:60', '4B:16,27,28,39', '3V:29,40,50', '4A:6,7,8,19']
GROWING = 24  # how many of the engine's favourite growing moves red may choose from


def main():
    moves = list(START)
    red = position_after(moves, len(moves))[0]
    cands = [m['s'] for m in ml.children(moves)['moves']]
    grow = [c for c in cands if any(n in red for x in move_cells(c) for n in neighbours(x))][:GROWING]
    ml.log('START %s  red growing candidates %d: %s' % (' '.join(moves), len(grow), ' '.join(grow)))
    margin, choice = 1, None
    while choice is None:
        choice = ml.first(moves, grow, margin, 0)  # blue to move, wins by exactly `margin`
        if choice is None:
            margin += 1
    moves.append(choice)
    ml.log('PLY %d red %s  -> blue wins by %d' % (len(moves), choice, margin))
    while True:
        node = ml.children(moves)
        if node['over']:
            break
        cands = [m['s'] for m in node['moves']]
        if node['stm'] == 1:  # blue keeps the margin
            choice = ml.first(moves, cands, margin - 1, -1)
        else:  # red holds it to exactly the margin
            choice = ml.first(moves, cands, margin, 0)
        if choice is None:
            ml.log('NO MOVE KEEPS THE MARGIN?!')
            break
        moves.append(choice)
        ml.log('PLY %d %s %s' % (len(moves), 'blue' if node['stm'] == 1 else 'red', choice))
    ml.log('DONE margin %d: %s' % (margin, ' '.join(moves)))
    json.dump({'moves': moves, 'margin': margin}, open(os.path.join(ml.HERE, 'data', 'hard_close.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
