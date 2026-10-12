"""Why red cannot place its last piece (the 4C 'U') at the end of the story game: every placement on empty
cells with the reason it breaks the rules (readable/rules.hpp: a new snake must not touch any piece; a
grown snake keeps at most two own neighbours per cell and no three own cells touching each other).

    .venv/Scripts/python.exe stuck_piece.py [game piece plies out]   # default: DRAW_GAME 4C 14 stuck_piece
"""
import sys
import json
import os

import snekviz
from snekviz import CELLS, PIECES, apply_symmetry, idx, neighbours, position_after, qr

HERE = os.path.dirname(os.path.abspath(__file__))


def placements(shape, empty):
    seen = set()
    for t in range(12):
        offs = [apply_symmetry(t, q, r) for q, r in shape]
        for anchor in empty:
            aq, ar = qr(anchor)
            cells = []
            for q, r in offs:
                c = idx(aq + q, ar + r) if abs(aq + q) <= 5 and abs(ar + r) <= 5 else None
                if c is None or c not in empty:
                    break
                cells.append(c)
            else:
                key = tuple(sorted(cells))
                if key not in seen:
                    seen.add(key)
                    yield list(key)


def why(cells, own, occupied):
    """The rule a red piece on these cells breaks, and the cells that show it (None if it is legal)."""
    cells_set = set(cells)
    border = {n for c in cells for n in neighbours(c)} - cells_set
    if not (border & own):
        touched = sorted(border & occupied)
        return ('a new snake may not touch other pieces', touched) if touched else None
    after = own | cells_set
    for c in cells:
        around = [n for n in neighbours(c) if n in after]
        if len(around) > 2:
            return ('a cell would get a third red neighbour', [c] + around)
        if len(around) == 2 and around[1] in neighbours(around[0]):
            return ('sharp turn: three red cells touching', [c] + around)
    for o in sorted(border & own):
        around = [n for n in neighbours(o) if n in after]
        if len(around) > 2:
            return ('this red cell would get a third neighbour', [o])
    return None


def main(game='DRAW_GAME', piece='4C', plies=14, out='stuck_piece'):
    placed = position_after(getattr(snekviz, game), int(plies))
    own, occupied = placed[0], placed[0] | placed[1]
    empty = set(CELLS) - occupied
    result = []
    for cells in placements(PIECES[piece], empty):
        reason = why(cells, own, occupied)
        assert reason is not None, ('legal placement?!', cells)
        result.append({'cells': cells, 'reason': reason[0], 'show': reason[1]})
    counts = {}
    for r in result:
        counts[r['reason']] = counts.get(r['reason'], 0) + 1
    print(len(result), 'placements, all illegal:', counts)
    json.dump(result, open(os.path.join(HERE, 'data', out + '.json'), 'w'), indent=0)


if __name__ == '__main__':
    main(*sys.argv[1:])
