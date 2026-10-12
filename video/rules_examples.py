# Illegal red placements in a position of the rules game, classified like the rulebook figures.
import os, sys
GAME = '1:38 4A:59,60,61,69 4A:83,91,92,93 4C:80,90,101,102 3V:51,62,72 4B:104,105,113,114 4B:19,20,28,29 4I:32,43,54,65 2:31,41 3V:67,78,88 4C:36,37,46,57 3I:75,85,95 3I:5,6,7 2:10,21 pass pass'.split()
def qr(i): return i % 11 - 5, i // 11 - 5
def on(q, r): return abs(q) <= 5 and abs(r) <= 5 and abs(q + r) <= 5
def idx(q, r): return (r + 5) * 11 + q + 5
D = [(1,0),(1,-1),(0,-1),(-1,0),(-1,1),(0,1)]
def nb(i):
    q, r = qr(i); return {idx(q+a, r+b) for a, b in D if on(q+a, r+b)}
SH = {'2': [(0,0),(1,0)], '3I': [(0,0),(0,1),(0,2)], '3V': [(0,0),(1,0),(1,1)], '4I': [(0,0),(-1,1),(-2,2),(-3,3)], '4A': [(0,0),(1,0),(1,1),(1,2)], '4B': [(0,0),(0,1),(1,1),(1,2)], '4C': [(0,0),(-1,1),(-1,2),(0,2)]}
def orients(shape):
    out = []
    for t in range(12):
        cells = []
        for q, r in shape:
            if t >= 6: q, r = r, q
            for _ in range(t % 6): q, r = -r, q + r
            cells.append((q, r))
        cells.sort(key=lambda c: (c[1], c[0])); a = cells[0]
        cells = tuple((q - a[0], r - a[1]) for q, r in cells)
        if cells not in out: out.append(cells)
    return out
def comps(cells):
    cells, out = set(cells), []
    while cells:
        st = [cells.pop()]; c = set(st)
        while st:
            x = st.pop()
            for n in nb(x):
                if n in cells: cells.discard(n); c.add(n); st.append(n)
        out.append(c)
    return out
K = int(sys.argv[1]); pieces = sys.argv[2].split(',')
own = [set(), set()]
for k, m in enumerate(GAME[:K]):
    if m != 'pass': own[k % 2] |= {int(x) for x in m.split(':')[1].split(',')}
occ = own[0] | own[1]; red = own[0]
cs = comps(red)
print('after move', K, 'red snakes:', [sorted(c) for c in cs])
ends = {c for c in red if len(nb(c) & red) <= 1}
for name in pieces:
    for o in orients(SH[name]):
        for anchor in range(121):
            aq, ar = qr(anchor); cells = []
            if not all(on(aq + q, ar + r) for q, r in o): continue
            cells = [idx(aq + q, ar + r) for q, r in o]
            if set(cells) & occ: continue
            P = set(cells); border = {n for c in P for n in nb(c)} - P
            if border & own[1] or not border & red: continue   # touches only own red cells
            S = red | P
            tri = [c for c in P if len(nb(c) & S) == 2 and list(nb(c) & S)[1] in nb(list(nb(c) & S)[0])]
            deg3 = [c for c in S if len(nb(c) & S) > 2]
            touched = [c for c in cs if c & border]
            kind = None
            if len(touched) == 1 and tri and (border & red) <= ends | set().union(*[nb(e) & red for e in ends]): kind = 'sharp'
            if len(touched) == 1 and not tri and deg3 and all(c in red and c not in ends for c in deg3): kind = 'branch'
            if len(touched) >= 2 and not tri and deg3: kind = 'side'
            if kind: print(kind, name, sorted(P), 'deg3', deg3, 'tri', tri)
