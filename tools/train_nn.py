"""Trainer of the "nn" engine's evaluation network (src/nn.hpp), numpy on the CPU.

    python tools/train_nn.py --snek build/snek.exe --data a.csv b.csv --out net.bin [options]
    python tools/train_nn.py --embed net.bin src/nn_net.hpp      (make it the engine's default network)

Data: `snek datagen` files (one row per position: kind, game, ply, stm, score, exact, result,
margin, flag, move, pos). The network inputs come from `snek nnfeatures` (cached next to each CSV
as <csv>.nnf), so the trainer and the engine share one definition of the inputs.

Value target, for the side to move (draws count 1/2):
  - the exact value where known (engine verdicts, exact labels, siblings),
  - else a blend of the search score and the game result: lam * sigmoid(score / 100) + (1 - lam) * result.
Censored rows (the exact label ran out of time) get the blended target: they are not dropped.
Ranking loss: the exactly solved siblings (kind c rows and the game's own position at the same ply)
are compared in pairs: a child that is better for the parent should get the lower value.
Augmentation: one of the 12 hex symmetries per mini-batch. Hold-out (held_out): every game whose id
hashes to 0 mod 10; train_policy.py holds out the same games. The test report gives the
log-loss on exact-label rows and on blended-target rows apart: the network is trained to fit the
blended target (search score and game result, the score partly from searches with earlier networks)
and the logistic evaluation is not, so only exact rows compare the two fairly. Output: the int16 file
the engine loads (nn.hpp Net::loadBytes).
"""
import argparse, os, struct, subprocess, sys, time
import numpy as np

HIDDEN, BUCKETS, QA, QB = 128, 16, 255, 128


def run_features(snek, csv):
    """The inputs of every row (cached per CSV and executable: the input layout belongs to the build)."""
    out = f'{csv}.{os.path.splitext(os.path.basename(snek))[0]}.nnf'
    if not os.path.exists(out) or os.path.getmtime(out) < max(os.path.getmtime(csv), os.path.getmtime(snek)):
        subprocess.run([snek, 'nnfeatures', csv, out], check=True)
    return out


def load_features(path):
    """Ragged inputs of both perspectives + the logistic logit per row."""
    raw = np.fromfile(path, dtype='<u2')
    i, n = 0, len(raw)
    # rows: n_s, idx..., n_o, idx..., float32 logit (two uint16 words)
    starts = []
    while i < n:
        a = int(raw[i]); s0 = i + 1; i = s0 + a
        b = int(raw[i]); o0 = i + 1; i = o0 + b
        lg = i; i += 2
        starts.append((s0, a, o0, b, lg))
    st = np.array(starts, dtype=np.int64)
    lg = raw[np.stack([st[:, 4], st[:, 4] + 1], 1)].astype(np.uint32)
    logit = (lg[:, 0] | (lg[:, 1] << 16)).view(np.float32)
    return raw, st[:, 0], st[:, 1], st[:, 2], st[:, 3], logit


def load_csv(path):
    rows = []
    with open(path) as f:
        hdr = f.readline().strip().split(',')
        col = {c: k for k, c in enumerate(hdr)}
        for line in f:
            p = line.rstrip('\n').split(',')
            # a repeated header (appended runs) or a cut-off last line: kept as an empty row, so
            # the rows stay aligned with the feature file
            if len(p) != len(hdr) or p[0] == hdr[0] or not p[col.get('game', 0)].lstrip('-').isdigit():
                p = None
            rows.append(p)
    return col, rows


class Data:
    def __init__(self, snek, files, lam):
        raws, offs = [], 0
        fs = []
        for fi, csv in enumerate(files):
            col, rows = load_csv(csv)
            raw, s0, sl, o0, ol, logit = load_features(run_features(snek, csv))
            if len(rows) != len(s0):
                sys.exit(f'{csv}: {len(rows)} rows but {len(s0)} feature rows')
            ok = np.array([r is not None and sl[k] > 0 for k, r in enumerate(rows)])
            def colv(name, default, conv=int):
                if name not in col:
                    return np.full(len(rows), default)
                return np.array([conv(r[col[name]]) if r is not None else default for r in rows])
            game = colv('game', -1)
            if 'kind' in col:
                kind = np.array([r[col['kind']] == 'c' if r is not None else False for r in rows])
                leaf = np.array([r[col['kind']] == 'l' if r is not None else False for r in rows])
                exact = colv('exact', -9)
                score = colv('score', 0)
                result = colv('result', 0)
            else:  # label files: game,ply,stm,value,...
                kind = np.zeros(len(rows), bool)
                leaf = np.zeros(len(rows), bool)
                exact = colv('value', -9)
                score = np.zeros(len(rows), int)
                result = exact.copy()
            fs.append(dict(raw=raw, s0=s0[ok] + offs, sl=sl[ok], o0=o0[ok] + offs, ol=ol[ok], logit=logit[ok],
                           game=game[ok], file=np.full(ok.sum(), fi), ply=colv('ply', 0)[ok], exact=exact[ok], score=score[ok],
                           result=result[ok], kind=kind[ok], leaf=leaf[ok]))
            offs += len(raw)
            raws.append(raw)
        self.flat = np.concatenate(raws).astype(np.int64)
        cat = lambda k: np.concatenate([f[k] for f in fs])
        self.s0, self.sl, self.o0, self.ol = cat('s0'), cat('sl'), cat('o0'), cat('ol')
        self.logit, self.game, self.file, self.ply = cat('logit'), cat('game'), cat('file'), cat('ply')
        self.exact, self.score, self.result, self.kind = cat('exact'), cat('score'), cat('result'), cat('kind')
        self.leaf = cat('leaf')
        known = np.isin(self.exact, [-1, 0, 1])
        proven = np.abs(self.score) >= 50000
        y_search = np.where(proven, (np.sign(self.score) + 1) / 2, 1 / (1 + np.exp(-np.clip(self.score, -3000, 3000) / 100)))
        y_result = (self.result + 1) / 2
        blend = np.where(self.kind | self.leaf, y_search, lam * y_search + (1 - lam) * y_result)  # no game result there
        self.y = np.where(known, (self.exact + 1) / 2, blend).astype(np.float32)
        self.known = known
        # ranking target of a row: a logit (exact values +-3 / 0, else the search score)
        self.rt = np.where(known, 3.0 * self.exact, np.clip(self.score / 100, -3, 3)).astype(np.float32)
        # sibling groups: (file, game, ply) of the kind-c rows, with the game's own row at that ply (by file
        # too: label files number their games independently of datagen's ids)
        self.groups = []
        cidx = np.nonzero(self.kind & (known | (self.score != 0)))[0]
        if len(cidx):
            key = np.unique(np.stack([self.file, self.game, self.ply], 1), axis=0, return_inverse=True)[1].reshape(-1)
            own = np.nonzero(~self.kind & ~self.leaf & np.isin(key, key[cidx]))[0]
            allidx = np.concatenate([cidx, own])
            allidx = allidx[np.argsort(key[allidx], kind='stable')]
            cuts = np.nonzero(np.diff(key[allidx]))[0] + 1
            for g in np.split(allidx, cuts):
                if len(g) > 1 and self.rt[g].max() - self.rt[g].min() >= RANK_MIN_GAP:
                    self.groups.append(g)

    def batch(self, rows, perm):
        """Dense 0/1 input matrices of both perspectives (symmetry `perm` applied)."""
        B = len(rows)
        out = []
        for st, ln in ((self.s0, self.sl), (self.o0, self.ol)):
            lens = ln[rows]
            tot = int(lens.sum())
            rid = np.repeat(np.arange(B), lens)
            pos = np.arange(tot) - np.repeat(np.cumsum(lens) - lens, lens) + np.repeat(st[rows], lens)
            X = np.zeros((B, NIN), np.float32)
            X[rid, perm[self.flat[pos]]] = 1
            out.append(X)
        return out


class Net:
    def __init__(self, rng):
        self.p = {'w1': (rng.standard_normal((NIN, HIDDEN)) * 0.05).astype(np.float32),
                  'b1': np.full(HIDDEN, 0.25, np.float32),
                  'wo': (rng.standard_normal((BUCKETS, 2 * HIDDEN)) * 0.05).astype(np.float32),
                  'bo': np.zeros(BUCKETS, np.float32)}
        self.m = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.v = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.t = 0

    def forward(self, Xs, Xo, bucket):
        p = self.p
        As = Xs @ p['w1'] + p['b1']; Ao = Xo @ p['w1'] + p['b1']
        Cs = np.clip(As, 0, 1); Co = np.clip(Ao, 0, 1)
        Hc = np.concatenate([Cs * Cs, Co * Co], 1)
        Wb = p['wo'][bucket]
        z = (Hc * Wb).sum(1) + p['bo'][bucket]
        return z, (Xs, Xo, As, Ao, Cs, Co, Hc, Wb, bucket)

    def backward(self, dz, cache):
        Xs, Xo, As, Ao, Cs, Co, Hc, Wb, bucket = cache
        g = {}
        dWb = dz[:, None] * Hc
        g['wo'] = np.zeros_like(self.p['wo']); np.add.at(g['wo'], bucket, dWb)
        g['bo'] = np.bincount(bucket, weights=dz, minlength=BUCKETS).astype(np.float32)
        dH = dz[:, None] * Wb
        dAs = dH[:, :HIDDEN] * 2 * Cs * ((As > 0) & (As < 1))
        dAo = dH[:, HIDDEN:] * 2 * Co * ((Ao > 0) & (Ao < 1))
        g['w1'] = Xs.T @ dAs + Xo.T @ dAo
        g['b1'] = dAs.sum(0) + dAo.sum(0)
        return g

    def adam(self, g, lr, wd):
        self.t += 1
        for k in self.p:
            self.m[k] = 0.9 * self.m[k] + 0.1 * g[k]
            self.v[k] = 0.999 * self.v[k] + 0.001 * g[k] ** 2
            mh = self.m[k] / (1 - 0.9 ** self.t); vh = self.v[k] / (1 - 0.999 ** self.t)
            self.p[k] -= lr * (mh / (np.sqrt(vh) + 1e-8) + wd * self.p[k])
        # keep the int16 export exact: |wo| * QB <= 127, accumulators far from the int16 limit
        np.clip(self.p['wo'], -127 / QB, 127 / QB, out=self.p['wo'])
        np.clip(self.p['w1'], -4, 4, out=self.p['w1'])

    def save(self, path):
        p = self.p
        w1 = np.clip(np.round(p['w1'] * QA), -32767, 32767).astype('<i2')
        b1 = np.clip(np.round(p['b1'] * QA), -32767, 32767).astype('<i2')
        wo = np.clip(np.round(p['wo'] * QB), -127, 127).astype('<i2')
        bo = np.round(p['bo'] * QA * QB).astype('<i4')
        with open(path, 'wb') as f:
            f.write(b'SNN1' + struct.pack('<3i', NIN, HIDDEN, BUCKETS))
            for a in (w1, b1, wo, bo):
                f.write(np.ascontiguousarray(a).tobytes())

    def quantized_forward(self, data, rows, ident):
        """Integer arithmetic as nn.hpp does it (for the C++ cross-check)."""
        p = self.p
        w1 = np.round(p['w1'] * QA).astype(np.int64); b1 = np.round(p['b1'] * QA).astype(np.int64)
        wo = np.clip(np.round(p['wo'] * QB), -127, 127).astype(np.int64); bo = np.round(p['bo'] * QA * QB).astype(np.int64)
        Xs, Xo = data.batch(rows, ident)
        As = Xs.astype(np.int64) @ w1 + b1; Ao = Xo.astype(np.int64) @ w1 + b1
        vs = np.clip(As, 0, QA); vo = np.clip(Ao, 0, QA)
        b = bucket_of(data.ply[rows])
        s = (vs * vs * wo[b][:, :HIDDEN]).sum(1) + (vo * vo * wo[b][:, HIDDEN:]).sum(1)
        s = np.trunc(s / QA).astype(np.int64) + bo[b]
        return np.clip(np.trunc(s * 100 / (QA * QB)), -3000, 3000)


RANK_MIN_GAP = 0.3  # sibling pairs whose ranking targets differ by less are not compared


def rank_pairs(t):
    """Pairs (hi, lo) of a group whose targets differ by at least RANK_MIN_GAP, hi the better one."""
    return np.nonzero(t[:, None] - t[None, :] >= RANK_MIN_GAP)


def bucket_of(ply):
    return np.minimum(ply, BUCKETS - 1)


def held_out(game):
    """The test split by game id (datagen and policygen number the games seed * 10^6 + g, unique across runs
    with different seeds): the games whose id hashes to 0 mod 10. train_policy.py holds out the same games."""
    return (game * 2654435761 % 2**32) % 10 == 0


def game_bootstrap(d, games, rng, n=1000):
    """95% interval of the mean of d, resampling whole games."""
    ug, inv = np.unique(games, return_inverse=True)
    s, c = np.bincount(inv, d), np.bincount(inv)
    means = []
    for _ in range(n):
        w = np.bincount(rng.integers(len(ug), size=len(ug)), minlength=len(ug))
        means.append((w * s).sum() / (w * c).sum())
    return np.percentile(means, [2.5, 97.5])


def logloss(z, y):
    p = 1 / (1 + np.exp(-np.clip(z, -30, 30)))
    return -(y * np.log(np.clip(p, 1e-9, 1)) + (1 - y) * np.log(np.clip(1 - p, 1e-9, 1)))


def embed(net_path, header_path, tool='train_nn.py', what='network', var='NN_NET_B64'):
    """Write the network file as base64 text into the header the engine compiles in (nn.hpp)."""
    import base64
    b64 = base64.b64encode(open(net_path, 'rb').read()).decode()
    with open(header_path, 'w', newline='\n') as f:
        f.write(f'// Generated by tools/{tool} --embed from {os.path.basename(net_path)}: the default {what} of the\n'
                f'// "nn" engine (src/nn.hpp), base64.\n#pragma once\n// clang-format off\ninline const char {var}[] =\n')
        for k in range(0, len(b64), 120):
            f.write(f'"{b64[k:k + 120]}"\n')
        f.write(';\n')


def main():
    global NIN
    if len(sys.argv) == 4 and sys.argv[1] == '--embed':
        embed(sys.argv[2], sys.argv[3])
        return
    ap = argparse.ArgumentParser()
    ap.add_argument('--snek', required=True)
    ap.add_argument('--data', nargs='+', required=True)
    ap.add_argument('--out', default='net.bin')
    ap.add_argument('--epochs', type=int, default=12)
    ap.add_argument('--batch', type=int, default=1024)
    ap.add_argument('--lr', type=float, default=2e-3)
    ap.add_argument('--wd', type=float, default=1e-5)
    ap.add_argument('--lam', type=float, default=0.5, help='weight of the search score in the blended target')
    ap.add_argument('--rank', type=float, default=0.5, help='weight of the sibling ranking loss')
    ap.add_argument('--max-ply', type=int, default=11, help='train on plies up to this')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--init', help='start from this float checkpoint (.npz)')
    ap.add_argument('--report-only', action='store_true', help='no training: report the --init network on every row')
    a = ap.parse_args()
    if a.report_only:
        a.epochs = 0
    rng = np.random.default_rng(a.seed)
    t0 = time.time()
    sym = np.fromfile(run_features(a.snek, a.data[0]) + '.sym', dtype='<i2').astype(np.int64)
    NIN = len(sym) // 12
    perms = sym.reshape(12, NIN)
    D = Data(a.snek, a.data, a.lam)
    test = held_out(D.game)
    if a.report_only:
        test[:] = True
    use = D.ply <= a.max_ply
    tr = np.nonzero(~test & use)[0]
    te = np.nonzero(test & use)[0]
    if not len(te):
        sys.exit(f'{len(D.y)} rows but no held-out game among them (a tenth of the games is held out): more games are needed')
    groups = [g for g in D.groups if not test[g[0]]]
    print(f'{len(D.y)} rows ({D.known.mean():.2f} exact), train {len(tr)}, test {len(te)}, sibling groups {len(groups)}; '
          f'load {time.time() - t0:.0f} s')
    for p in range(0, a.max_ply + 1):
        m = D.ply[tr] == p
        if m.sum():
            print(f'  ply {p:2d}: {m.sum():6d} rows, exact {D.known[tr][m].mean():.2f}, mean target {D.y[tr][m].mean():.3f}, '
                  f'censored {(D.exact[tr][m] == -8).sum()}')
    net = Net(rng)
    if a.init:
        z = np.load(a.init)
        net.p = {k: z[k].astype(np.float32) for k in net.p}
    nb = len(tr) // a.batch
    total = max(1, a.epochs * nb)
    ident = np.arange(NIN)

    def evaluate(rows):
        zs = []
        for k in range(0, len(rows), 4096):
            r = rows[k:k + 4096]
            Xs, Xo = D.batch(r, ident)
            zs.append(net.forward(Xs, Xo, bucket_of(D.ply[r]))[0])
        return np.concatenate(zs)

    def report(tag):
        z = evaluate(te)
        ll, llr = logloss(z, D.y[te]), logloss(D.logit[te], D.y[te])
        ex = D.known[te]
        msg = f'{tag}: test logloss {ll.mean():.4f} (logistic {llr.mean():.4f}); exact rows {ll[ex].mean():.4f} ({llr[ex].mean():.4f})'
        print(msg, f'({time.time() - t0:.0f} s)'); sys.stdout.flush()
        return z

    for ep in range(a.epochs):
        order = rng.permutation(tr)
        for b in range(nb):
            rows = order[b * a.batch:(b + 1) * a.batch]
            perm = perms[rng.integers(12)]
            lr = a.lr * (0.05 + 0.95 * 0.5 * (1 + np.cos(np.pi * net.t / total)))
            Xs, Xo = D.batch(rows, perm)
            z, cache = net.forward(Xs, Xo, bucket_of(D.ply[rows]))
            dz = (1 / (1 + np.exp(-z)) - D.y[rows]) / len(rows)
            g = net.backward(dz.astype(np.float32), cache)
            if a.rank > 0 and groups:
                # RankNet on sibling pairs: P(i above j) = sigmoid(z_i - z_j) against the soft target
                # sigmoid(t_i - t_j) of their ranking targets (the better child for its side to move
                # is the worse move for the parent)
                gs = [groups[i] for i in rng.integers(len(groups), size=64)]
                rr = np.concatenate(gs)
                Xs2, Xo2 = D.batch(rr, perm)
                z2, c2 = net.forward(Xs2, Xo2, bucket_of(D.ply[rr]))
                dz2 = np.zeros_like(z2)
                at, npairs = 0, 0
                for gg in gs:
                    k = len(gg); zz = z2[at:at + k]; tt = D.rt[gg]
                    hi, lo = rank_pairs(tt)
                    gd = 1 / (1 + np.exp(-np.clip(zz[hi] - zz[lo], -30, 30))) - 1 / (1 + np.exp(-(tt[hi] - tt[lo])))
                    np.add.at(dz2, at + hi, gd); np.add.at(dz2, at + lo, -gd)
                    npairs += len(hi); at += k
                if npairs:
                    g2 = net.backward((dz2 * a.rank / npairs).astype(np.float32), c2)
                    for k in g:
                        g[k] += g2[k]
            net.adam(g, lr, a.wd)
        report(f'epoch {ep + 1}')
    z = report('final')
    # "mean": the mean of the network's and the logistic evaluation's logits (the nn engine's evaluation)
    yt, zl, pt = D.y[te], D.logit[te], D.ply[te]
    zm = (z + zl) / 2
    ln, ll, lm = logloss(z, yt), logloss(zl, yt), logloss(zm, yt)
    brng = np.random.default_rng(11)
    print('test by ply. Exact-label rows: n, logloss network / logistic / mean, network - logistic and mean - '
          'logistic [95% game bootstrap], accuracy network / logistic / mean on decisive rows. Blended-target rows: '
          'n, logloss network / logistic / mean')
    for lo, hi in [(p, p) for p in range(a.max_ply + 1)] + [(4, 6), (7, a.max_ply)]:
        m = (pt >= lo) & (pt <= hi)
        me, mb = m & D.known[te], m & ~D.known[te]
        if not m.sum():
            continue
        line = f'  ply {lo:2d}   ' if lo == hi else f'  ply {lo:2d}-{hi:<2d}'
        if me.sum():
            qn = game_bootstrap(ln[me] - ll[me], D.game[te][me], brng)
            qm = game_bootstrap(lm[me] - ll[me], D.game[te][me], brng)
            dec = me & (yt != 0.5)
            acc = lambda v: ((v[dec] > 0) == (yt[dec] > 0.5)).mean() if dec.sum() else float('nan')
            line += (f' exact n={me.sum():6d}  {ln[me].mean():.4f} {ll[me].mean():.4f} {lm[me].mean():.4f}  '
                     f'{ln[me].mean() - ll[me].mean():+.4f} [{qn[0]:+.4f}, {qn[1]:+.4f}]  '
                     f'{lm[me].mean() - ll[me].mean():+.4f} [{qm[0]:+.4f}, {qm[1]:+.4f}]  {acc(z):.3f} {acc(zl):.3f} {acc(zm):.3f}')
        else:
            line += f' exact n={0:6d}' + ' ' * 104
        if mb.sum():
            line += f'  | blended n={mb.sum():6d}  {ln[mb].mean():.4f} {ll[mb].mean():.4f} {lm[mb].mean():.4f}'
        print(line)
    # sibling ranking on held-out groups: share of correctly ordered pairs
    tg = [g for g in D.groups if test[g[0]]]
    if tg:
        rr = np.concatenate(tg)
        zr = evaluate(rr)
        lr_ = D.logit[rr]
        print('held-out sibling pairs by ply (pairs, share ordered like the target: network, logistic, mean):')
        stats = {}
        at = 0
        zm_ = (zr + lr_) / 2
        for gg in tg:
            k = len(gg); hi, lo = rank_pairs(D.rt[gg])
            s = stats.setdefault(int(D.ply[gg[0]]), [0, 0, 0, 0])
            s[0] += len(hi)
            for j, q in enumerate((zr, lr_, zm_)):
                s[j + 1] += (q[at:at + k][hi] > q[at:at + k][lo]).sum()
            at += k
        for p in sorted(stats):
            n_, a_, b_, c_ = stats[p]
            print(f'  ply {p:2d}: {n_:6d} pairs  {a_ / n_:.3f}  {b_ / n_:.3f}  {c_ / n_:.3f}')
    if a.report_only:
        return
    net.save(a.out)
    np.savez(a.out + '.npz', **net.p)
    q = net.quantized_forward(D, te[:2000], ident)
    print(f'quantized vs float (centi-logits) on 2000 test rows: max |diff| {np.abs(q - 100 * z[:2000]).max():.1f}')
    print('wrote', a.out)


if __name__ == '__main__':
    main()
