"""Compare weight files on the good-play hold-out games (same split as fit_eval.py).

    python tools/compare_weights.py data/labels_good_v3.csv data/weights_v2.txt data/weights_latest.txt
"""
import sys
import numpy as np
from fit_eval import load, FEATS, metrics, jumpiness

rows = load([sys.argv[1]])
vals = np.array([r['value'] for r in rows]); y = (vals + 1) / 2
X = np.array([[f(r) for _, f in FEATS] for r in rows], dtype=float)
gid = np.array([r['file'] * 100000 + r['game'] for r in rows])
ug = np.unique(gid)
rng = np.random.default_rng(0)
test_games = set(rng.choice(ug, size=len(ug) // 5, replace=False))
names = [n for n, _ in FEATS]
for wf in sys.argv[2:]:
    w = dict(l.split() for l in open(wf))
    wv = np.array([float(w[n]) for n in names])
    z = X @ wv
    for label, m in (('all good-play', np.ones(len(rows), bool)), ('test games', np.array([g in test_games for g in gid]))):
        ll, acc = metrics(z[m], y[m], vals[m])
        jp, jt, n = jumpiness(rows, z, m)
        print(f'{wf:28s} {label:14s} logloss {ll:.4f} acc {acc:.3f}  jumpiness {jp:.3f} (exact {jt:.3f}, {n} pairs)')
