"""A close perfect line for the video: from a position blue is proven to win (red to move), both sides
play for the score. Red picks the move that loses by the fewest cells, blue a move that keeps its
margin. The margin comes from the exact solver with SOLVER_RED_BONUS=b (red gets b extra cells), so
"blue still wins" means "blue wins by more than b" (build/snek_margin.exe).

    python margin_line.py "<moves>" [first-red-candidates]     # log: data/margin_line.log
"""
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', 'build', 'snek_margin.exe')
THREADS = int(os.environ.get('THREADS', 12))
BATCH = os.path.join(HERE, 'data', 'margin_batch.txt')
LOG = open(os.path.join(HERE, 'data', 'margin_line.log'), 'a', encoding='utf-8')
BELOW_NORMAL = 0x4000


def log(*a):
    print(*a, file=LOG, flush=True)


def children(moves):
    p = subprocess.run([EXE, 'serve'], input='children %s\nquit\n' % ' '.join(moves), capture_output=True, text=True)
    return json.loads(p.stdout.splitlines()[0])


def solve(positions, bonus):
    """Exact values (for the side to move) of the positions with red getting `bonus` extra cells."""
    with open(BATCH, 'w') as f:
        for ms in positions:
            f.write(' '.join(ms) + '\n')
    env = dict(os.environ, SOLVER_RED_BONUS=str(bonus))
    p = subprocess.run([EXE, 'benchset', BATCH, str(min(THREADS, len(positions)))], capture_output=True, text=True,
                       env=env, creationflags=BELOW_NORMAL)
    return [int(m.group(1)) for m in re.finditer(r'^\s*\d+\s+value\s+(-?\d+)', p.stdout, re.M)]


def first(moves, cands, bonus, wanted):
    """The first candidate (in engine order) whose child has the wanted value, solving batch by batch."""
    for start in range(0, len(cands), THREADS):
        batch = cands[start:start + THREADS]
        t0 = time.time()
        vals = solve([moves + [c] for c in batch], bonus)
        log('    bonus %d  tried %s  values %s  %.1fs' % (bonus, start + len(batch), vals, time.time() - t0))
        for c, v in zip(batch, vals):
            if v == wanted:
                return c
    return None


def main():
    moves = sys.argv[1].split()
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 48
    log('START %s  (red to move, blue proven to win)' % ' '.join(moves))
    # red's first choice: the smallest margin among the engine's `limit` favourite moves
    cands = [m['s'] for m in children(moves)['moves']][:limit]
    margin, choice = 1, None
    while choice is None:
        choice = first(moves, cands, margin, 0)  # blue to move, wins by exactly `margin`
        if choice is None:
            margin += 1
    moves.append(choice)
    log('PLY %d red %s  -> blue wins by %d' % (len(moves), choice, margin))
    while True:
        node = children(moves)
        if node['over']:
            break
        cands = [m['s'] for m in node['moves']]
        if node['stm'] == 1:  # blue keeps the margin: child (red to move) still lost by more than margin - 1
            choice = first(moves, cands, margin - 1, -1)
        else:  # red holds it to exactly the margin
            choice = first(moves, cands, margin, 0)
        if choice is None:
            log('NO MOVE KEEPS THE MARGIN?!')
            break
        moves.append(choice)
        log('PLY %d %s %s' % (len(moves), 'blue' if node['stm'] == 1 else 'red', choice))
    log('DONE margin %d: %s' % (margin, ' '.join(moves)))


if __name__ == '__main__':
    main()
