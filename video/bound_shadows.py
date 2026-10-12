"""Shadows for the bounds beat (chapter 9): after each move of the won game, the pieces each player still has,
placed one after another by the rules (the other player not answering), so that the longest snake reaches the
upper bound the bars show. The upper bound is sound, so a shadow that reaches it is a longest possible one.

    .venv/Scripts/python.exe bound_shadows.py     # prints it, writes data/bound_shadows.json
"""
import json
import os

from snekviz import CELLS, PIECES, WIN_GAME, components, longest_snake, move_cells, position_after
from stuck_piece import placements, why

HERE = os.path.dirname(os.path.abspath(__file__))
BEAM = 300


def best_growth(own, other, pieces):
    """(length, moves) of the longest snake the pieces can build onto `own`, `other` not moving."""
    best = (longest_snake(own), [])
    frontier = {frozenset(own): ([], tuple(pieces))}
    while frontier:
        nxt = {}
        for mine, (seq, left) in frontier.items():
            occupied = mine | other
            empty = set(CELLS) - occupied
            for name in set(left):
                rest = list(left)
                rest.remove(name)
                for cells in placements(PIECES[name], empty):
                    if why(cells, mine, occupied) is not None:
                        continue
                    grown = mine | frozenset(cells)
                    if grown not in nxt:
                        nxt[grown] = (seq + ['%s:%s' % (name, ','.join(map(str, cells)))], tuple(rest))
        # keep the most promising: longest snake, then the most cells in one place
        ranked = sorted(nxt.items(), key=lambda kv: (longest_snake(kv[0]), len(kv[0])), reverse=True)[:BEAM]
        frontier = dict(ranked)
        for mine, (seq, _) in ranked:
            if longest_snake(mine) > best[0]:
                best = (longest_snake(mine), seq)
    return best


def replay(own, other, seq, pieces):
    """(length, moves) of the moves of an earlier shadow that are still legal here, in order; None if any is not."""
    mine = set(own)
    kept = [m for m in seq if m.split(':')[0] in pieces]
    for m in kept:
        cells = move_cells(m)
        if set(cells) & (mine | other) or why(cells, mine, mine | other) is not None:
            return None
        mine |= set(cells)
    return longest_snake(mine), kept


def main():
    import chapters2
    out, prev = {}, [[], []]
    for k in range(4, 13):
        placed = position_after(WIN_GAME, k)
        row = []
        for c in (0, 1):
            played = [m.split(':')[0] for j, m in enumerate(WIN_GAME[:k]) if j % 2 == c]
            left = [p for p in PIECES if p not in played and (c == 0 or p != '1')]  # the single hex is red's only
            ub = chapters2.WIN_BOUNDS[k][c][1]
            # the earlier shadow, if it still reaches the bound, so the shadows do not jump around between moves
            again = replay(placed[c], placed[1 - c], prev[c], left) if k > 4 else None
            length, seq = again if again and again[0] == ub else best_growth(frozenset(placed[c]), frozenset(placed[1 - c]), left)
            # only the pieces that end up in the longest snake
            final = set(placed[c]).union(*[set(move_cells(m)) for m in seq])
            snake = max(components(final), key=len)
            seq = [m for m in seq if set(move_cells(m)) <= snake]
            print('after %2d moves, %s: %2d of upper bound %2d %s' % (k, 'red ' if c == 0 else 'blue', length, ub, seq))
            row.append({'length': length, 'moves': seq})
            prev[c] = seq
        out[k] = row
    json.dump(out, open(os.path.join(HERE, 'data', 'bound_shadows.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
