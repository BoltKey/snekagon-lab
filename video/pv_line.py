"""Perfect continuation of a position with a known exact value: at every ply the first move (engine order)
that keeps the value, checked by the exact solver. The losing side simply plays the engine's favourite.

    python pv_line.py "<moves>" <value for the side to move: 1, 0 or -1> <log name>
"""
import os
import sys
import time

import margin_line as ml

ml.LOG = open(os.path.join(ml.HERE, 'data', sys.argv[3] + '.log'), 'w', encoding='utf-8')


def main():
    moves, value = sys.argv[1].split(), int(sys.argv[2])
    ml.log('START %s  value %d' % (' '.join(moves), value))
    while True:
        node = ml.children(moves)
        if node['over']:
            break
        cands = [m['s'] for m in node['moves']]
        t0 = time.time()
        choice = ml.first(moves, cands, 0, -value)  # a child worth -value for the opponent keeps the value
        if choice is None:
            ml.log('NO MOVE KEEPS THE VALUE?!')
            break
        moves.append(choice)
        value = -value
        ml.log('PLY %d %s  (%.1fs)' % (len(moves), choice, time.time() - t0))
    ml.log('DONE ' + ' '.join(moves))


if __name__ == '__main__':
    main()
