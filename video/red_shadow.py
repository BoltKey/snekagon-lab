"""The snake red threatens to build along the bottom of the board in the story game, before blue's move 8:
red's remaining pieces placed one after another (legal by the rules, blue not answering), each growing red's
snake, through the bottom cells where all of blue's drawing moves lie. Picks the longest such snake.
Also every bottom cell (rows 2 to 5) red could grow into with its remaining pieces, before its move 6.

    .venv/Scripts/python.exe red_shadow.py      # prints it, writes data/red_shadow.json
"""
import itertools
import json
import os

from snekviz import CELLS, DRAW_GAME, PIECES, longest_snake, neighbours, position_after, qr
from stuck_piece import placements, why

HERE = os.path.dirname(os.path.abspath(__file__))
ZONE = set(json.load(open(os.path.join(HERE, 'data', 'move8_draws.json')))['cells'])


def growths(own, blue, name):
    """Every legal placement of piece `name` that grows one of red's snakes."""
    empty = set(CELLS) - own - blue
    for cells in placements(PIECES[name], empty):
        if any(n in own for x in cells for n in neighbours(x)) and why(cells, own, own | blue) is None:
            yield cells


def wanted_cells():
    """Bottom cells red could grow into with four of its five remaining pieces, before its move 6."""
    placed = position_after(DRAW_GAME, 6)
    red, blue = frozenset(placed[0]), frozenset(placed[1])
    reached = set()
    for names in itertools.permutations(['2', '3I', '4A', '4B', '4C'], 4):
        frontier = {red}
        for name in names:
            frontier = {own | frozenset(cells) for own in frontier for cells in growths(own, blue, name)}
        for own in frontier:
            reached |= own - red
    return sorted(c for c in reached if qr(c)[1] >= 2)


def main():
    placed = position_after(DRAW_GAME, 7)
    red, blue = set(placed[0]), set(placed[1])
    best = None
    for names in itertools.permutations(['2', '3I', '4A', '4C'], 3):
        frontier = [(red, [])]
        for name in names:
            nxt = [(own | set(cells), seq + ['%s:%s' % (name, ','.join(map(str, cells)))])
                   for own, seq in frontier for cells in growths(own, blue, name)]
            # keep the most promising few: longest snake, most cells in the bottom zone
            nxt.sort(key=lambda t: (longest_snake(t[0]), len(t[0] & ZONE)), reverse=True)
            frontier = nxt[:60]
        for own, seq in frontier:
            score = (longest_snake(own), len((own - red) & ZONE))
            if best is None or score > best[0]:
                best = (score, seq)
    (length, in_zone), seq = best
    print('red could grow from', longest_snake(red), 'to', length, 'with', seq, '| cells in blue\'s zone:', in_zone)
    wants = wanted_cells()
    print('bottom cells red could grow into:', wants)
    json.dump({'moves': seq, 'from': longest_snake(red), 'to': length, 'wants': wants},
              open(os.path.join(HERE, 'data', 'red_shadow.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
