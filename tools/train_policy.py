"""Trainer of the "nn" engine's move ranking (the policy head of src/nn.hpp), numpy on the CPU.

    python tools/train_policy.py --snek build/snek.exe --data a.csv b.csv --out policy.bin [options]
    python tools/train_policy.py --embed policy.bin src/nn_policy.hpp   (make it the engine's default policy)

Data: `snek policygen` files and files of the same layout (kind, group, ply, stm, depth, flag, targets,
pos; tools/policy_rows.py makes them from proof logs and datagen files): per position, target moves with
a value each: W (proven win), D (proven draw or "at least a
draw"), or a search score in centi-logits. Target distribution over the legal moves: uniform over the W
moves if there are any, else over the D moves, else softmax(score / --temp) over the scored moves.

Model (nn.hpp Policy): a logit per placement, from the value network's first layer, which stays frozen
(--net, the engine's network), so the value evaluation is unchanged: SCReLU(acc[stm], acc[opp]) . W[g]
+ b[g] + per piece and plane, weights of the number of the placement's cells and neighbours in each of
the side to move's input planes. Softmax over the legal moves only, cross-entropy against the target
distribution, one of the 12 hex symmetries per mini-batch.

Hold-out: rows whose group is a game id (policygen's and datagen's rows) by game, the games that
train_nn.py holds out; the other rows (the proofs) by line: positions of the centre opening (1-hex on 60)
whose blue cells contain one of HOLDOUT_REPLIES (under any symmetry). Game rows in those lines follow their
game. Training rows whose position (up to symmetry) is a test position are dropped. The report gives, by
kind and ply, the cross-entropy and the top-1, top-5 and mean rank of the first best target (the W moves,
else the D moves, else the best-scored move) among the legal moves.

Output: the file the engine loads (nn.hpp Policy::loadBytes) and <out>.npz with the float weights;
--check compares `snek policyfeatures`'s C++ logits with this trainer's integer model.
"""
import argparse, os, struct, subprocess, sys, time
import numpy as np
from train_nn import HIDDEN, QA, embed, held_out as held_out_games, load_features, run_features

QP = 64
NPIECE, NSYM, NPLANES = 8, 12, 9
PNAME = ['1', '2', '3I', '3V', '4I', '4A', '4B', '4C']
# board index (r + 5) * 11 + (q + 5) of each input cell (core.hpp T.cells: the hexagon's cells in index order)
BOARD_CELLS = np.array([i for i in range(121) if max(abs(i % 11 - 5), abs(i // 11 - 5), abs(i % 11 + i // 11 - 10)) <= 5])
CENTRE = 60
HOLDOUT_REPLIES = ['4B:16,27,28,39', '3V:36,37,48', '4A:18,19,30,41']


def run_policy_features(snek, csv, policy=None):
    """Legal moves and canonical keys of every row (cached per CSV and executable), and the placement table."""
    tag = os.path.splitext(os.path.basename(snek))[0]
    out = f'{csv}.{tag}.pf' if policy is None else f'{csv}.{tag}.check.pf'
    if policy is not None or not os.path.exists(out) or os.path.getmtime(out) < max(os.path.getmtime(csv), os.path.getmtime(snek)):
        subprocess.run([snek, 'policyfeatures', csv, out] + ([policy] if policy else []), check=True)
    return out


def load_gid_table(path):
    raw = open(path, 'rb').read()
    ngid, nsym, nc = struct.unpack('<3i', raw[:12])
    rec = np.dtype([('piece', '<i2'), ('sym', '<i2', nsym), ('cover', 'u1', nc), ('touch', 'u1', nc)])
    t = np.frombuffer(raw, rec, ngid, 12)
    return t['piece'].astype(np.int64), t['sym'].astype(np.int64), t['cover'].astype(np.float32), t['touch'].astype(np.float32)


def load_policy_features(path, with_logits=False):
    raw = open(path, 'rb').read()
    keys, gids, logits, at = [], [], [], 0
    while at < len(raw):
        k, n = struct.unpack_from('<QH', raw, at); at += 10
        g = np.frombuffer(raw, '<u2', n, at).astype(np.int64); at += 2 * n
        keys.append(k); gids.append(g)
        if with_logits and n:
            logits.append(np.frombuffer(raw, '<i4', n, at).astype(np.int64)); at += 4 * n
        elif with_logits:
            logits.append(np.zeros(0, np.int64))
    return np.array(keys, np.uint64), gids, logits


def parse_move(s):
    """(piece, bitmask of board cells) of a move in notation (cells separated by ',' or '.')."""
    piece, cells = s.split(':')
    return PNAME.index(piece), sum(1 << int(c) for c in cells.replace('.', ',').split(','))


def load_net(path):
    """The frozen first layer of a value network file (nn.hpp Net::loadBytes): int16 w1 [inputs][HIDDEN], b1."""
    raw = open(path, 'rb').read()
    nin, hidden, buckets = struct.unpack('<3i', raw[4:16])
    assert raw[:4] == b'SNN1' and hidden == HIDDEN
    w1 = np.frombuffer(raw, '<i2', nin * hidden, 16).reshape(nin, hidden).astype(np.float32)
    b1 = np.frombuffer(raw, '<i2', hidden, 16 + 2 * nin * hidden).astype(np.float32)
    return w1, b1


class Data:
    def __init__(self, snek, files, temp):
        flat, offs = [], 0
        cols = dict(s0=[], sl=[], o0=[], ol=[], ply=[], kind=[], group=[], key=[], legal=[], tg=[], tw=[], best=[], pos=[], line=[])
        self.header = None
        self.table = None
        for csv in files:
            raw, s0, sl, o0, ol, _ = load_features(run_features(snek, csv))
            pf = run_policy_features(snek, csv)
            keys, legal, _ = load_policy_features(pf)
            if self.table is None:
                self.table = load_gid_table(pf + '.gid')
                piece, sym, cover, _ = self.table
                self.mask_of = [sum(1 << int(c) for c in BOARD_CELLS[np.nonzero(cover[g])[0]]) for g in range(len(piece))]
                self.gid_of = {(int(piece[g]), self.mask_of[g]): g for g in range(len(piece))}
            lines = open(csv).read().splitlines()
            hdr = lines[0].split(',')
            c = {n: k for k, n in enumerate(hdr)}
            self.header = self.header or lines[0]
            rows = lines[1:]
            if len(rows) != len(s0) or len(rows) != len(keys):
                sys.exit(f'{csv}: {len(rows)} rows, {len(s0)} input rows, {len(keys)} move rows')
            skipped = 0
            for i, ln in enumerate(rows):
                p = ln.split(',')
                if len(p) != len(hdr) or sl[i] == 0 or not len(legal[i]):
                    skipped += 1
                    continue
                g, w, best = self.targets(p[c['targets']], legal[i], temp)
                if g is None:
                    skipped += 1
                    continue
                cols['s0'].append(s0[i] + offs); cols['sl'].append(sl[i]); cols['o0'].append(o0[i] + offs); cols['ol'].append(ol[i])
                cols['ply'].append(int(p[c['ply']])); cols['kind'].append(p[c['kind']]); cols['group'].append(p[c['group']])
                cols['key'].append(keys[i]); cols['legal'].append(legal[i]); cols['tg'].append(g); cols['tw'].append(w)
                cols['pos'].append(p[c['pos']]); cols['best'].append(best); cols['line'].append(ln)
            print(f'{csv}: {len(rows) - skipped} rows used, {skipped} skipped (unreadable, no legal move or no legal target)')
            flat.append(raw)
            offs += len(raw)
        self.flat = np.concatenate(flat).astype(np.int64)
        for k in ('s0', 'sl', 'o0', 'ol', 'ply'):
            setattr(self, k, np.array(cols[k], np.int64))
        self.kind, self.group, self.pos = np.array(cols['kind']), np.array(cols['group']), cols['pos']
        self.key = np.array(cols['key'], np.uint64)
        self.legal, self.tg, self.tw, self.best, self.line = cols['legal'], cols['tg'], cols['tw'], cols['best'], cols['line']
        self.n = len(self.ply)

    def targets(self, field, legal, temp):
        """Target placements and weights of a row, and its best targets: the W or D moves, else the best-scored
        move (None if no target move is legal)."""
        win, draw, scored = [], [], []
        for e in field.split(';'):
            mv, val = e.rsplit('/', 1)
            g = self.gid_of.get(parse_move(mv))
            if g is None or g not in legal:
                continue
            if val == 'W': win.append(g)
            elif val == 'D': draw.append(g)
            else: scored.append((g, float(val)))
        if win or draw:
            g = np.array(win or draw, np.int64)
            return g, np.full(len(g), 1 / len(g), np.float32), g
        if not scored:
            return None, None, None
        g = np.array([x[0] for x in scored], np.int64)
        s = np.array([x[1] for x in scored])
        w = np.exp((s - s.max()) / temp)
        return g, (w / w.sum()).astype(np.float32), g[[np.argmax(s)]]

    def inputs(self, rows, perm):
        """Dense 0/1 inputs of both perspectives (symmetry `perm` applied), as train_nn.Data.batch."""
        B, out = len(rows), []
        for st, ln in ((self.s0, self.sl), (self.o0, self.ol)):
            lens = ln[rows]
            rid = np.repeat(np.arange(B), lens)
            pos = np.arange(int(lens.sum())) - np.repeat(np.cumsum(lens) - lens, lens) + np.repeat(st[rows], lens)
            X = np.zeros((B, NIN), np.float32)
            X[rid, perm[self.flat[pos]]] = 1
            out.append(X)
        return out

    def moves(self, rows, t, ngid, sym):
        """Legal-move mask and target matrix [rows][placements] under symmetry t."""
        B = len(rows)
        mask = np.zeros((B, ngid), bool)
        tgt = np.zeros((B, ngid), np.float32)
        lens = np.array([len(self.legal[r]) for r in rows])
        mask[np.repeat(np.arange(B), lens), sym[np.concatenate([self.legal[r] for r in rows]), t]] = True
        tl = np.array([len(self.tg[r]) for r in rows])
        tgt[np.repeat(np.arange(B), tl), sym[np.concatenate([self.tg[r] for r in rows]), t]] = np.concatenate([self.tw[r] for r in rows])
        return mask, tgt


class Policy:
    def __init__(self, table, w1, b1):
        self.piece, self.sym, self.cover, self.touch = table
        self.ngid = len(self.piece)
        self.w1, self.b1 = w1, b1
        self.onehot = np.zeros((NPIECE, self.ngid), np.float32)
        self.onehot[self.piece, np.arange(self.ngid)] = 1
        self.p = {'w': np.zeros((self.ngid, 2 * HIDDEN), np.float32), 'b': np.zeros(self.ngid, np.float32),
                  'cover': np.zeros((NPIECE, NPLANES), np.float32), 'touch': np.zeros((NPIECE, NPLANES), np.float32)}
        self.m = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.v = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.t = 0

    def hidden(self, Xs, Xo):
        """The clipped accumulators v (0..QA) of both perspectives, exactly as the engine computes them."""
        return np.concatenate([np.clip(Xs @ self.w1 + self.b1, 0, QA), np.clip(Xo @ self.w1 + self.b1, 0, QA)], 1)

    def local_weights(self):
        """[placements][planes * cells]: the cover and touch weights of each placement's cells and neighbours."""
        p = self.p
        nc = self.cover.shape[1]
        cw = p['cover'][self.piece][:, :, None] * self.cover[:, None, :]
        tw = p['touch'][self.piece][:, :, None] * self.touch[:, None, :]
        return (cw + tw).reshape(self.ngid, NPLANES * nc)

    def forward(self, Xs, Xo):
        V = self.hidden(Xs, Xo)
        H = (V / QA) ** 2
        Xp = Xs[:, :NPLANES * self.cover.shape[1]]
        z = H @ self.p['w'].T + self.p['b'] + Xp @ self.local_weights().T
        return z, (H, Xp)

    def backward(self, dz, cache):
        H, Xp = cache
        nc = self.cover.shape[1]
        g = {'w': dz.T @ H, 'b': dz.sum(0)}
        dl = (dz.T @ Xp).reshape(self.ngid, NPLANES, nc)
        gc = (dl * self.cover[:, None, :]).sum(2)
        gt = (dl * self.touch[:, None, :]).sum(2)
        g['cover'] = self.onehot @ gc
        g['touch'] = self.onehot @ gt
        return g

    def adam(self, g, lr, wd):
        self.t += 1
        for k in self.p:
            self.m[k] = 0.9 * self.m[k] + 0.1 * g[k]
            self.v[k] = 0.999 * self.v[k] + 0.001 * g[k] ** 2
            mh = self.m[k] / (1 - 0.9 ** self.t); vh = self.v[k] / (1 - 0.999 ** self.t)
            self.p[k] -= lr * (mh / (np.sqrt(vh) + 1e-8) + (wd * self.p[k] if k == 'w' else 0))
        np.clip(self.p['w'], -127 / QP, 127 / QP, out=self.p['w'])  # int8 export

    def quantized(self):
        p, s = self.p, QA * QP
        return (np.clip(np.round(p['w'] * QP), -127, 127).astype(np.int64), np.round(p['b'] * s).astype(np.int64),
                np.round(p['cover'] * s).astype(np.int64), np.round(p['touch'] * s).astype(np.int64))

    def quantized_logits(self, Xs, Xo):
        """Integer logits of every placement as nn.hpp Policy::logits computes them."""
        wq, bq, cq, tq = self.quantized()
        V = self.hidden(Xs, Xo).astype(np.int64)
        dot = (V * V) @ wq.T
        dot = np.sign(dot) * (np.abs(dot) // QA)  # C++ integer division truncates towards zero
        nc = self.cover.shape[1]
        Xp = Xs[:, :NPLANES * nc].reshape(len(Xs), NPLANES, nc).astype(np.int64)
        fc = np.einsum('bkc,gc->bgk', Xp, self.cover.astype(np.int64))
        ft = np.einsum('bkc,gc->bgk', Xp, self.touch.astype(np.int64))
        return dot + bq + (fc * cq[self.piece]).sum(2) + (ft * tq[self.piece]).sum(2)

    def save(self, path):
        wq, bq, cq, tq = self.quantized()
        with open(path, 'wb') as f:
            f.write(b'SNP1' + struct.pack('<3i', self.ngid, HIDDEN, NPLANES))
            f.write(wq.astype('i1').tobytes())
            for a in (bq, cq, tq):
                f.write(a.astype('<i4').tobytes())


def held_out(D, table):
    """Test rows: the rows of train_nn.py's held-out games; the proof rows of the centre opening's positions that
    contain a held-out blue reply."""
    piece, sym, cover, _ = table
    test = np.zeros(D.n, bool)
    sp = np.array([g.isdigit() for g in D.group])  # the group is a self-play game id
    game = np.array([int(g) if s else 0 for g, s in zip(D.group, sp)], np.int64)
    test[sp] = held_out_games(game[sp])
    replies = []
    for r in HOLDOUT_REPLIES:
        g = D.gid_of[parse_move(r)]
        replies.append({D.mask_of[sym[g, t]] for t in range(NSYM)})
    for i in np.nonzero(~sp)[0]:
        own0, own1 = (int(x, 16) for x in D.pos[i].split('_')[:2])
        if not own0 >> CENTRE & 1:
            continue
        if any(any(own1 & m == m for m in images) for images in replies):
            test[i] = True
    # no position of the test set (up to symmetry) in training
    test_keys = set(D.key[test].tolist())
    drop = ~test & np.isin(D.key, np.array(sorted(test_keys), np.uint64))
    return test, drop


def cross_entropy(z, mask, tgt):
    """Cross-entropy of each row's target distribution under the softmax of z over its legal moves, and the softmax."""
    z = np.where(mask, z, -1e9)
    z = z - z.max(1, keepdims=True)
    e = np.exp(z) * mask
    pr = e / e.sum(1, keepdims=True)
    return -(tgt * np.log(np.clip(pr, 1e-12, 1))).sum(1), pr


def ranks(z, mask, rows, D):
    """Rank (1 = first) of each row's first best target among its legal moves by the logits z (ties count half)."""
    out = []
    for i, r in enumerate(rows):
        zz = np.where(mask[i], z[i], -np.inf)
        zt = zz[D.best[r]][:, None]
        out.append(((zz > zt).sum(1) + 1 + ((zz == zt).sum(1) - 1) / 2).min())
    return np.array(out)


def main():
    global NIN
    if len(sys.argv) == 4 and sys.argv[1] == '--embed':
        embed(sys.argv[2], sys.argv[3], 'train_policy.py', 'move ranking (policy)', 'NN_POLICY_B64')
        return
    ap = argparse.ArgumentParser()
    ap.add_argument('--snek', required=True)
    ap.add_argument('--data', nargs='+', required=True)
    ap.add_argument('--net', default='data/nn/nn_g2.bin', help='the value network whose first layer the policy reads')
    ap.add_argument('--out', default='policy.bin')
    ap.add_argument('--epochs', type=int, default=8)
    ap.add_argument('--batch', type=int, default=512)
    ap.add_argument('--lr', type=float, default=3e-3)
    ap.add_argument('--wd', type=float, default=1e-4)
    ap.add_argument('--temp', type=float, default=40, help='softmax temperature of scored targets (centi-logits)')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--init', help='start from this float checkpoint (.npz)')
    ap.add_argument('--report-only', action='store_true', help='no training: report the --init policy on the test rows')
    ap.add_argument('--check', type=int, default=0, help='compare the C++ logits with the integer model on this many test rows')
    ap.add_argument('--test-out', help='write the test rows to this CSV (to compare other move orders on them)')
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    t0 = time.time()
    sym_in = np.fromfile(run_features(a.snek, a.data[0]) + '.sym', dtype='<i2').astype(np.int64)
    NIN = len(sym_in) // NSYM
    perms = sym_in.reshape(NSYM, NIN)
    D = Data(a.snek, a.data, a.temp)
    sym = D.table[1]
    test, drop = held_out(D, D.table)
    tr = np.nonzero(~test & ~drop)[0]
    te = np.nonzero(test)[0]
    print(f'{D.n} rows, train {len(tr)}, test {len(te)}, dropped (a test position up to symmetry) {drop.sum()}; load {time.time() - t0:.0f} s')
    for kind in np.unique(D.kind):
        m = D.kind == kind
        print(f'  kind {kind}: train {(m & ~test & ~drop).sum():6d}  test {(m & test).sum():6d}  by ply (train): ' +
              ' '.join(f'{p}:{((D.ply == p) & m & ~test & ~drop).sum()}' for p in range(16) if ((D.ply == p) & m).sum()))
    if a.test_out:
        with open(a.test_out, 'w', newline='\n') as f:
            f.write(D.header + '\n' + ''.join(D.line[r] + '\n' for r in te))
    w1, b1 = load_net(a.net)
    P = Policy(D.table, w1, b1)
    if a.init:
        z = np.load(a.init)
        P.p = {k: z[k].astype(np.float32) for k in P.p}
    if a.report_only:
        a.epochs = 0
    nb = max(1, len(tr) // a.batch)
    total = max(1, a.epochs * nb)
    for ep in range(a.epochs):
        order = rng.permutation(tr)
        loss_sum = 0
        for b in range(nb):
            rows = order[b * a.batch:(b + 1) * a.batch]
            t = int(rng.integers(NSYM))
            Xs, Xo = D.inputs(rows, perms[t])
            mask, tgt = D.moves(rows, t, P.ngid, sym)
            z, cache = P.forward(Xs, Xo)
            loss, pr = cross_entropy(z, mask, tgt)
            loss_sum += loss.mean()
            lr = a.lr * (0.05 + 0.95 * 0.5 * (1 + np.cos(np.pi * P.t / total)))
            P.adam(P.backward(((pr - tgt) / len(rows)).astype(np.float32), cache), lr, a.wd)
        print(f'epoch {ep + 1}: train loss {loss_sum / nb:.4f} ({time.time() - t0:.0f} s)'); sys.stdout.flush()
    report(D, P, te, sym)
    if a.check:
        check(D, P, te[:a.check], a)
    if a.report_only:
        return
    P.save(a.out)
    np.savez(a.out + '.npz', **P.p)
    print('wrote', a.out)


def report(D, P, te, sym):
    """Top-1, top-5 and mean rank of the best target on the test rows, by kind and ply."""
    ident = np.arange(NIN)
    res, ce = [], []
    for k in range(0, len(te), 1024):
        rows = te[k:k + 1024]
        Xs, Xo = D.inputs(rows, ident)
        mask, tgt = D.moves(rows, 0, P.ngid, sym)
        z, _ = P.forward(Xs, Xo)
        res.append(ranks(z, mask, rows, D))
        ce.append(cross_entropy(z, mask, tgt)[0])
    rk = np.concatenate(res) if res else np.zeros(0)
    ce = np.concatenate(ce) if ce else np.zeros(0)
    print('test rows: kind, plies, n, cross-entropy, top-1, top-5, mean rank of the first best target among the legal moves')
    for kind in np.unique(D.kind[te]):
        for lo, hi in [(0, 15), (0, 3), (4, 5), (6, 15)]:
            m = (D.kind[te] == kind) & (D.ply[te] >= lo) & (D.ply[te] <= hi)
            if m.sum():
                print(f'  {kind} ply {lo:2d}-{hi:<2d} n={m.sum():6d}  {ce[m].mean():.3f}  {np.mean(rk[m] == 1):.3f}  {np.mean(rk[m] <= 5):.3f}  '
                      f'{rk[m].mean():7.2f}')
    return rk


def check(D, P, rows, a):
    """The C++ logits (policyfeatures with the policy file) against the integer model, on the given rows."""
    import tempfile
    tmp = tempfile.mkdtemp()
    csv = os.path.join(tmp, 'check.csv')
    with open(csv, 'w', newline='\n') as f:
        f.write('kind,group,ply,stm,depth,flag,targets,pos\n')
        for r in rows:
            f.write(f'x,0,{D.ply[r]},0,0,-,-,{D.pos[r]}\n')
    pol = os.path.join(tmp, 'check.bin')
    P.save(pol)
    _, legal, cl = load_policy_features(run_policy_features(a.snek, csv, pol), True)
    Xs, Xo = D.inputs(rows, np.arange(NIN))
    q = P.quantized_logits(Xs, Xo)
    diff = max((np.abs(q[i][D.legal[r]] - cl[i]).max() for i, r in enumerate(rows) if len(cl[i])), default=0)
    same = all(np.array_equal(legal[i], D.legal[r]) for i, r in enumerate(rows))
    print(f'C++ logits vs the integer model on {len(rows)} test rows ({sum(len(x) for x in cl)} moves): max |diff| {diff}; '
          f'same legal moves: {same}')


if __name__ == '__main__':
    main()
