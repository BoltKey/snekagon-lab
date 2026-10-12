"""Move-by-move story of a game, for writing narration: what each piece does (new snake, grow, join), the
snake lengths, which snake ends die, and which of the opponent's reserved cells get used.

    .venv/Scripts/python.exe game_story.py RULES_GAME
"""
import sys

import snekviz
from snekviz import CELLS, components, longest_snake, move_cells, neighbours, position_after


def shape(own, other):
    body = {c for c in own if sum(1 for n in neighbours(c) if n in own) >= 2}
    ends = {c for c in own if sum(1 for n in neighbours(c) if n in own) <= 1}
    empty = set(CELLS) - own - other
    growable = {c for c in empty if not any(n in body for n in neighbours(c))}
    live = {e for e in ends if any(n in growable for n in neighbours(e))}
    reserved_for_other = {n for c in body for n in neighbours(c) if n in empty}  # dead for owner, fine for the other
    return ends, live, reserved_for_other


def main(name):
    game = getattr(snekviz, name)
    for k, m in enumerate(game):
        player = k % 2
        who = 'red ' if player == 0 else 'blue'
        before = position_after(game, k)
        after = position_after(game, k + 1)
        if m == 'pass':
            print('%2d %s pass' % (k + 1, who))
            continue
        cells = set(move_cells(m))
        own_before = before[player]
        touched = [c for c in components(own_before) if any(n in c for x in cells for n in neighbours(x))]
        kind = 'new snake' if not touched else 'grow' if len(touched) == 1 else 'JOIN %d snakes' % len(touched)
        _, live_opp_before, _ = shape(before[1 - player], before[player])
        _, live_opp_after, _ = shape(after[1 - player], after[player])
        killed = sorted(live_opp_before - live_opp_after)
        _, _, reserved_by_opp = shape(before[1 - player], before[player])
        used_reserved = sorted(cells & reserved_by_opp)
        print('%2d %s %-18s %-14s red %2d blue %2d  snakes %d/%d%s%s' % (
            k + 1, who, m, kind, longest_snake(after[0]), longest_snake(after[1]), len(components(after[0])), len(components(after[1])),
            '  KILLS opponent end(s) %s' % killed if killed else '', '  uses reserved %s' % used_reserved if used_reserved else ''))


if __name__ == '__main__':
    main(sys.argv[1])
