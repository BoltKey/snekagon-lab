# Engine-vs-engine games for the rules section; prints the move lists with a few properties.
import json, subprocess, sys
EXE = 'C:/random/claudeSnekagon/build/snek_w2.exe'
def nb(i):
    q, r = i % 11 - 5, i // 11 - 5
    return {(r+b+5)*11+q+a+5 for a, b in [(1,0),(1,-1),(0,-1),(-1,0),(-1,1),(0,1)] if abs(q+a) <= 5 and abs(r+b) <= 5 and abs(q+a+r+b) <= 5}
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
p = subprocess.Popen([EXE, 'serve', '4'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1, creationflags=0x4000)
def ask(c):
    p.stdin.write(c + '\n'); p.stdin.flush(); return json.loads(p.stdout.readline())
for first in sys.argv[1:]:
    d = ask('set 1:%s' % first)
    moves = ['1:%s' % first]
    while not d.get('over', False):
        r = ask('go 0.4')
        moves.append(r['played']['s']); d = r['state']
    own = [set(), set()]; tags = []
    for k, m in enumerate(moves):
        pl = k % 2
        cells = set() if m == 'pass' else {int(x) for x in m.split(':')[1].split(',')}
        if not cells: tags.append('%d:pass' % (k+1)); continue
        border = {n for c in cells for n in nb(c)} - cells
        before = len(comps(own[pl])) if own[pl] else 0
        touch_own = bool(border & own[pl]); touch_opp = bool(border & own[1-pl])
        own[pl] |= cells
        after = len(comps(own[pl]))
        if not touch_own and own[pl] != cells: tags.append('%d:new' % (k+1))
        if touch_own and after < before: tags.append('%d:JOIN' % (k+1))
        if touch_own and touch_opp: tags.append('%d:grow+opp' % (k+1))
    print(first, d['longest'], ' '.join(moves))
    print('   ', ' '.join(tags))
p.stdin.write('quit\n'); p.stdin.flush()
