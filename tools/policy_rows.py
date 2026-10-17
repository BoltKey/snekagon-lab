"""Training rows for the move ranking (tools/train_policy.py) from other sources than `snek policygen`,
in policygen's layout (kind, group, ply, stm, depth, flag, targets, pos):

    python tools/policy_rows.py proofs <out.csv> <log, summary or directory>...
    python tools/policy_rows.py datagen <out.csv> <datagen.csv>...

proofs: the moves that `snek refute` logs and `snek proveline` summaries prove; a directory stands for
every file below it whose first line is such a header, other files are skipped. In a refute log of
position P ("# refute <P> loss <l> bonus <b>"), "<M> refuted <R>" makes R a target of P after M: a proven
win (W) with loss 1, at least a draw (D) with loss 0; "<M> wins" makes M a W target of P, "<M> holds" a D
target. In a proveline summary of P ("# proveline <P> bonus <b>"), "<R> wins <A>" makes A a W target of P
after R and "<R> holds" R a D target of P. Logs with a red bonus are skipped (the engine plays without
one). One row per position, its targets merged (W over D); kind r (a refutation), h (a move that holds
or wins at the log's position) or w (a proveline answer) and group (the file) of its first target.

datagen: the game positions of `snek datagen` files where the engine played its best move (flag b) and
the side to move was not proven lost, with that move as the one scored target (kind d; group = the game
id, so train_policy.py holds them out by game).
"""
import os, re, sys

PIECES = ['1', '2', '3I', '3V', '4I', '4A', '4B', '4C']
MOVE = re.compile(r'^[1234][IVABC]?:[0-9]+(,[0-9]+)*$')
LOST = -50000  # a datagen score at or below this is a proven loss for the side to move (search.hpp WIN / 2)
HEADER = 'kind,group,ply,stm,depth,flag,targets,pos\n'


def parse_pos(text):
    """datagen.hpp posStr: own0_own1_avail0_avail1_stm_ply_passes."""
    f = text.split('_')
    return [int(f[0], 16), int(f[1], 16)], [int(f[2]), int(f[3])], int(f[4]), int(f[5]), int(f[6])


def pos_str(p):
    own, avail, stm, ply, passes = p
    return f'{own[0]:032x}_{own[1]:032x}_{avail[0]}_{avail[1]}_{stm}_{ply}_{passes}'


def play(p, move):
    """The position after a placement; None if it overlaps a tile or its piece is not left."""
    own, avail, stm, ply, _ = p
    piece, cells = move.split(':')
    k = PIECES.index(piece)
    m = sum(1 << int(c) for c in cells.split(','))
    if m & (own[0] | own[1]) or not avail[stm] >> k & 1:
        return None
    own, avail = list(own), list(avail)
    own[stm] |= m
    avail[stm] &= ~(1 << k)
    return own, avail, stm ^ 1, ply + 1, 0


def proof_header(path):
    """(kind, position, loss) of a refute log or proveline summary (loss None for proveline), else None."""
    with open(path, errors='replace') as f:
        t = f.readline().split()
    if len(t) < 5 or t[0] != '#' or t[1] not in ('refute', 'proveline') or t[-2] != 'bonus' or t[-1] != '0':
        return None
    if t[1] == 'refute':
        return ('refute', parse_pos(t[2]), int(t[4])) if t[3] == 'loss' else None
    return 'proveline', parse_pos(t[2]), None


def proofs(out, sources):
    rows = {}  # position -> [kind, group, ply, stm, {move: W or D}], in the order of first sight
    illegal = 0

    def add(p, move, value, kind, group):
        nonlocal illegal
        if p is None or not MOVE.match(move) or play(p, move) is None:
            illegal += 1
            return
        r = rows.setdefault(pos_str(p), [kind, group, p[3], p[2], {}])
        r[4][move] = 'W' if 'W' in (r[4].get(move), value) else 'D'

    files = []
    for src in sources:
        if os.path.isdir(src):
            for dp, dn, fn in os.walk(src):
                dn.sort()
                files += [(os.path.join(dp, f), os.path.relpath(os.path.join(dp, f), os.path.dirname(os.path.normpath(src))))
                          for f in sorted(fn)]
        else:
            files.append((src, os.path.basename(src)))
    used = 0
    for path, group in files:
        head = proof_header(path)
        if head is None:
            continue
        used += 1
        kind, P, loss = head
        group = group.replace('\\', '/')
        with open(path, errors='replace') as f:
            f.readline()
            for line in f:
                t = line.split()
                if len(t) < 2 or not MOVE.match(t[0]):
                    continue
                if kind == 'refute' and t[1] == 'refuted' and len(t) >= 3:
                    add(play(P, t[0]), t[2], 'W' if loss == 1 else 'D', 'r', group)
                elif kind == 'refute' and t[1] in ('wins', 'holds'):
                    add(P, t[0], 'W' if t[1] == 'wins' else 'D', 'h', group)
                elif kind == 'proveline' and t[1] == 'wins' and len(t) >= 3:
                    add(play(P, t[0]), t[2], 'W', 'w', group)
                elif kind == 'proveline' and t[1] == 'holds':
                    add(P, t[0], 'D', 'h', group)
    with open(out, 'w', newline='\n') as f:
        f.write(HEADER)
        for key, (kind, group, ply, stm, targets) in rows.items():
            tg = ';'.join(m.replace(',', '.') + '/' + v for m, v in targets.items())
            f.write(f'{kind},{group},{ply},{stm},0,-,{tg},{key}\n')
    wins = sum('W' in r[4].values() for r in rows.values())
    print(f'{used} logs: {len(rows)} positions ({wins} with a proven win), {illegal} unplayable targets skipped')


def datagen(out, sources):
    seen, n = set(), 0
    with open(out, 'w', newline='\n') as fo:
        fo.write(HEADER)
        for path in sources:
            with open(path) as f:
                hdr = f.readline().rstrip('\n').split(',')
                c = {k: i for i, k in enumerate(hdr)}
                for line in f:
                    p = line.rstrip('\n').split(',')
                    if len(p) != len(hdr) or p[0] != 'g' or p[c['flag']] != 'b' or p[c['move']] in ('-', 'pass'):
                        continue
                    key = (p[c['game']], p[c['ply']])  # a game that two runs appended once each
                    if int(p[c['score']]) <= LOST or key in seen:
                        continue
                    seen.add(key)
                    fo.write(f"d,{p[c['game']]},{p[c['ply']]},{p[c['stm']]},0,b,{p[c['move']]}/0,{p[c['pos']]}\n")
                    n += 1
    print(f'{n} positions')


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[1] not in ('proofs', 'datagen'):
        sys.exit(__doc__)
    (proofs if sys.argv[1] == 'proofs' else datagen)(sys.argv[2], sys.argv[3:])
