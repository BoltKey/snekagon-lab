"""A peaceful game for chapter 13: both players only grow their own single snake and place every piece, so
red ends with 25 cells and blue with 24. Snakes may touch, but nobody shuts the other in: after every move
the engine's sound upper bound must still allow both full lengths. Depth-first search, own growth first.

    .venv/Scripts/python.exe peaceful_game.py      # prints the moves, writes data/peaceful_game.json
"""
import json
import os
import subprocess

from snekviz import longest_snake, neighbours

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'build', 'snek.exe')
FULL = (25, 24)
WIDTH = 5  # candidates tried per move


class Engine:
    def __init__(self):
        self.p = subprocess.Popen([ENGINE, 'serve', '1'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def state(self, moves):
        self.p.stdin.write('set %s\n' % ' '.join(moves))
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())


def search(engine, moves):
    st = engine.state(moves)
    if st['over']:
        own = st['longest']
        return moves if tuple(own) == FULL else None
    player = st['stm']
    mine, theirs = set(st['own'][player]), set(st['own'][1 - player])
    cands = []
    for m in st['legal']:
        cells = set(m['c'])
        ring = {n for c in cells for n in neighbours(c)} - cells
        if mine and not ring & mine:
            continue  # only growing, never a second snake
        new = mine | cells
        if longest_snake(new) != len(new):
            continue  # one single snake
        ends = [c for c in new if sum(1 for n in neighbours(c) if n in new) <= 1]
        room = sum(1 for e in ends for n in neighbours(e) if n not in new and n not in theirs)
        cands.append((bool(ring & theirs), -room, m['s']))
    cands.sort()
    tried = 0
    for _, _, s in cands:
        after = engine.state(moves + [s])
        if any(after['bounds'][p][1] < FULL[p] for p in (0, 1)):
            continue  # someone could no longer reach a full snake: shut in
        found = search(engine, moves + [s])
        if found:
            return found
        tried += 1
        if tried >= WIDTH:
            break
    return None


def main():
    engine = Engine()
    game = search(engine, ['1:60'])
    if not game:
        print('no peaceful game found')
        return
    st = engine.state(game)
    print(game, 'final', st['longest'])
    json.dump({'moves': game}, open(os.path.join(HERE, 'data', 'peaceful_game.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
