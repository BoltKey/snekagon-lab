"""Among the perfect-for-the-score continuations of a close line, find one where blue does not end with all
24 cells in one snake (that looks as if red never opposed it). Every move keeps the exact margin: red holds
blue to it, blue keeps it; within that, the search tries a few alternatives per move, depth first.

    python margin_dfs.py "<moves, blue to move>" <margin> [alternatives per move]   # log: data/margin_dfs.log
"""
import os
import sys

import margin_line as ml
from snekviz import longest_snake, position_after  # noqa: E402

ml.LOG = open(os.path.join(ml.HERE, 'data', 'margin_dfs.log'), 'a', encoding='utf-8')


def keepers(moves, cands, bonus, wanted, limit):
    """Up to `limit` candidates (engine order) whose child has the wanted value under the bonus."""
    found = []
    for start in range(0, len(cands), ml.THREADS):
        batch = cands[start:start + ml.THREADS]
        vals = ml.solve([moves + [c] for c in batch], bonus)
        found += [c for c, v in zip(batch, vals) if v == wanted]
        if len(found) >= limit:
            break
    return found[:limit]


def dfs(moves, margin, width):
    node = ml.children(moves)
    if node['over']:
        placed = position_after(moves, len(moves))
        red, blue = longest_snake(placed[0]), longest_snake(placed[1])
        ml.log('  end %d : %d  %s' % (blue, red, ' '.join(moves[4:])))
        return moves if blue < 24 else None
    cands = [m['s'] for m in node['moves']]
    if node['stm'] == 1:
        options = keepers(moves, cands, margin - 1, -1, width)  # blue keeps the margin
    else:
        options = keepers(moves, cands, margin, 0, width)       # red holds blue to the margin
    for m in options:
        found = dfs(moves + [m], margin, width)
        if found:
            return found
    return None


def main():
    moves, margin = sys.argv[1].split(), int(sys.argv[2])
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    ml.log('START %s  margin %d, width %d' % (' '.join(moves), margin, width))
    found = dfs(moves, margin, width)
    ml.log('FOUND %s' % ' '.join(found) if found else 'NONE')


if __name__ == '__main__':
    main()
