"""Independent, deliberately naive reference implementation of the Snekagon rules.

Used only to cross-check the C++ move generator (see ref/crosscheck.py).
Cells are (q, r) axial coordinates; the engine's cell index is (r+5)*11 + (q+5).
"""
R = 5
CELLS = [(q, r) for r in range(-R, R + 1) for q in range(-R, R + 1) if abs(q + r) <= R]
CELLSET = set(CELLS)
DIRS = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, -1), (-1, 1)]

PIECES = {
    '1': [(0, 0)],
    '2': [(0, 0), (1, 0)],
    '3I': [(0, 0), (0, 1), (0, 2)],
    '3V': [(0, 0), (1, 0), (1, 1)],
    '4I': [(0, 0), (-1, 1), (-2, 2), (-3, 3)],
    '4A': [(0, 0), (1, 0), (1, 1), (1, 2)],
    '4B': [(0, 0), (0, 1), (1, 1), (1, 2)],
    '4C': [(0, 0), (-1, 1), (-1, 2), (0, 2)],
}


def idx(c):
    return (c[1] + R) * 11 + (c[0] + R)


def neighbours(c):
    return [(c[0] + dq, c[1] + dr) for dq, dr in DIRS if (c[0] + dq, c[1] + dr) in CELLSET]


def transforms(shape):
    out = set()
    for flip in (False, True):
        cur = [(r, q) for q, r in shape] if flip else list(shape)
        for _ in range(6):
            out.add(frozenset(cur))
            cur = [(-r, q + r) for q, r in cur]
    return out


def placements(name):
    res = set()
    for o in transforms(PIECES[name]):
        for (aq, ar) in CELLS:
            cells = frozenset((q + aq, r + ar) for q, r in o)
            if cells <= CELLSET:
                res.add(cells)
    return res


PLACEMENTS = {n: placements(n) for n in PIECES}


def valid_snakes(cells):
    """Every cell has <= 2 own neighbours and no three cells are mutually adjacent."""
    for c in cells:
        nb = [n for n in neighbours(c) if n in cells]
        if len(nb) > 2:
            return False
        if len(nb) == 2 and nb[1] in neighbours(nb[0]):
            return False
    return True


class State:
    def __init__(self):
        self.own = [set(), set()]
        self.avail = [set(PIECES), set(PIECES) - {'1'}]
        self.stm = 0
        self.ply = 0
        self.passes = 0

    def legal_moves(self):
        me, opp = self.own[self.stm], self.own[1 - self.stm]
        occ = me | opp
        pieces = ['1'] if self.ply == 0 else sorted(self.avail[self.stm])
        res = []
        for p in pieces:
            if p not in self.avail[self.stm]:
                continue
            for pl in PLACEMENTS[p]:
                if pl & occ:
                    continue
                touching = {n for c in pl for n in neighbours(c)} - pl
                if touching & occ and not touching & me:
                    continue
                if not valid_snakes(me | pl):
                    continue
                res.append((p, pl))
        return res

    def play(self, mv):
        if mv is None:
            self.passes += 1
        else:
            p, pl = mv
            self.own[self.stm] |= pl
            self.avail[self.stm].discard(p)
            self.passes = 0
        self.stm ^= 1
        self.ply += 1

    def over(self):
        return self.passes >= 2 or (not self.avail[0] and not self.avail[1])


def move_str(mv):
    if mv is None:
        return 'pass'
    p, pl = mv
    return p + ':' + ','.join(str(i) for i in sorted(idx(c) for c in pl))


def longest(cells):
    seen, best = set(), 0
    for c in cells:
        if c in seen:
            continue
        stack, n = [c], 0
        seen.add(c)
        while stack:
            x = stack.pop()
            n += 1
            for y in neighbours(x):
                if y in cells and y not in seen:
                    seen.add(y)
                    stack.append(y)
        best = max(best, n)
    return best
