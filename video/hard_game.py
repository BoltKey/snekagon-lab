"""The hardest proven red reply (241 hours: red 3V:29,40,50, blue's winning answer 4A:6,7,8,19), played out:
blue, the winning side, plays the first move (engine order) the exact solver proves still wins; red defends
with the engine's favourite every time. For chapter 10.

    .venv/Scripts/python.exe hard_game.py       # log in data/hard_game.log, result in data/hard_game.json
"""
import json
import os
import time

import margin_line as ml

ml.LOG = open(os.path.join(ml.HERE, 'data', 'hard_game.log'), 'w', encoding='utf-8')
START = ['1:60', '4B:16,27,28,39', '3V:29,40,50', '4A:6,7,8,19']


def main():
    moves = list(START)
    ml.log('START %s' % ' '.join(moves))
    while True:
        node = ml.children(moves)
        if node['over']:
            break
        cands = [m['s'] for m in node['moves']]
        t0 = time.time()
        if node['stm'] == 0:
            choice = cands[0]  # red, losing: the engine's favourite defence
        else:
            choice = ml.first(moves, cands, 0, -1)  # blue: a move after which red (to move) is proven lost
            if choice is None:
                ml.log('NO WINNING MOVE?!')
                break
        moves.append(choice)
        ml.log('PLY %d %s  (%.1fs)' % (len(moves), choice, time.time() - t0))
    ml.log('DONE ' + ' '.join(moves))
    json.dump({'moves': moves}, open(os.path.join(ml.HERE, 'data', 'hard_game.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
