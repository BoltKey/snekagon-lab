"""Fit the evaluation function on solver-labelled positions.

    python tools/fit_eval.py data/labels_v1.csv data/labels_good_v3.csv ...

Each row is a position (features from both players' view, `_s` = side to move,
`_o` = opponent) with its exact game-theoretic value for the side to move
(-1/0/1). We fit a logistic regression P(side to move wins) and write
src/eval_weights.hpp + data/weights_latest.txt. Draws count as half a win.

Files whose name contains "good" come from (almost) greedy play; the others from
noisy play, where the move before a recorded position was often a blunder, which
inflates the side-to-move advantage. Noisy rows therefore get their own tempo
terms (bias, stmIsP1, dRem); only the good-play tempo terms are exported.
"""
import sys
import numpy as np

FEATS = [
    ('bias', lambda r: 1.0),
    ('dL', lambda r: r['L_s'] - r['L_o']),
    ('dSingle', lambda r: (r['singleOwn_s'] + min(r['rem_s'], r['singleTerr_s'])) - (r['singleOwn_o'] + min(r['rem_o'], r['singleTerr_o']))),
    ('dCluster', lambda r: (r['bestOwn_s'] + min(r['rem_s'], r['bestTerr_s'])) - (r['bestOwn_o'] + min(r['rem_o'], r['bestTerr_o']))),
    ('dDeficit', lambda r: max(0, r['rem_s'] - r['singleTerr_s']) - max(0, r['rem_o'] - r['singleTerr_o'])),
    ('dUB', lambda r: r['ub_s'] - r['ub_o']),
    ('dRem', lambda r: r['rem_s'] - r['rem_o']),
    ('dTerr20', lambda r: min(r['terr_s'], 20) - min(r['terr_o'], 20)),
    ('dTerr', lambda r: min(r['terr_s'], 60) - min(r['terr_o'], 60)),
    ('dSingleTerr', lambda r: min(r['singleTerr_s'], 40) - min(r['singleTerr_o'], 40)),
    ('dComps', lambda r: r['comps_s'] - r['comps_o']),
    ('dLiveEnds', lambda r: r['liveEnds_s'] - r['liveEnds_o']),
    ('dFresh', lambda r: min(r['fresh_s'], 30) - min(r['fresh_o'], 30)),
    ('stmIsP1', lambda r: 1.0 if r['stm'] == 0 else -1.0),
]
TEMPO = ['bias', 'stmIsP1', 'dRem']


def load(paths):
    rows = []
    for fi, p in enumerate(paths):
        good = 'good' in p
        with open(p) as f:
            head = f.readline().strip().split(',')
            for line in f:
                parts = line.strip().split(',')
                if len(parts) != len(head):
                    continue
                r = {k: int(v) for k, v in zip(head, parts)}
                r['good'] = good
                r['file'] = fi
                rows.append(r)
    return rows


def fit(X, y, l2=1e-3, iters=50):
    w = np.zeros(X.shape[1])
    for _ in range(iters):  # Newton / IRLS
        p = 1 / (1 + np.exp(-X @ w))
        g = X.T @ (p - y) / len(y) + l2 * w
        H = (X.T * (p * (1 - p))) @ X / len(y) + l2 * np.eye(len(w))
        w -= np.linalg.solve(H, g)
    return w


def metrics(logit, y, vals):
    p = 1 / (1 + np.exp(-logit))
    ll = -np.mean(y * np.log(np.clip(p, 1e-9, 1)) + (1 - y) * np.log(np.clip(1 - p, 1e-9, 1)))
    d = vals != 0
    acc = np.mean((logit[d] > 0) == (vals[d] > 0)) if d.any() else float('nan')
    return ll, acc


def jumpiness(rows, logit, mask):
    """Mean |change| of red's predicted win probability between consecutive plies of the
    same good-play game, next to the same quantity for the exact value."""
    red_pred, red_true = {}, {}
    for i, r in enumerate(rows):
        if not mask[i]:
            continue
        p = 1 / (1 + np.exp(-logit[i]))
        v = (r['value'] + 1) / 2
        key = (r['file'], r['game'], r['ply'])
        red_pred[key] = p if r['stm'] == 0 else 1 - p
        red_true[key] = v if r['stm'] == 0 else 1 - v
    dp, dt = [], []
    for (f, g, ply), p in red_pred.items():
        nxt = (f, g, ply + 1)
        if nxt in red_pred:
            dp.append(abs(red_pred[nxt] - p))
            dt.append(abs(red_true[nxt] - red_true[(f, g, ply)]))
    return (np.mean(dp), np.mean(dt), len(dp)) if dp else (float('nan'), float('nan'), 0)


def design(rows):
    X = np.array([[f(r) for _, f in FEATS] for r in rows], dtype=float)
    noisy = np.array([0.0 if r['good'] else 1.0 for r in rows])
    extra = np.stack([X[:, [n for n, _ in FEATS].index(t)] * noisy for t in TEMPO], axis=1)
    return np.hstack([X, extra])


def main():
    rows = load(sys.argv[1:])
    vals = np.array([r['value'] for r in rows])
    y = (vals + 1) / 2
    good = np.array([r['good'] for r in rows])
    plies = np.array([r['ply'] for r in rows])
    print(f'{len(rows)} positions ({good.sum()} from good play)')
    for name, m in (('noisy', ~good), ('good', good)):
        if m.any():
            v = vals[m]
            print(f'  {name}: side to move wins {np.mean(v == 1):.1%}, draws {np.mean(v == 0):.1%}, loses {np.mean(v == -1):.1%}')
    X = design(rows)
    ev = np.array([r['eval'] for r in rows], dtype=float)
    # hold-out split by game so consecutive plies stay together
    gid = np.array([r['file'] * 100000 + r.get('game', i) for i, r in enumerate(rows)])
    ug = np.unique(gid)
    rng = np.random.default_rng(0)
    test_games = set(rng.choice(ug, size=len(ug) // 5, replace=False))
    te = np.array([g in test_games for g in gid])
    tr = ~te
    w = fit(X[tr], y[tr])
    nb = len(FEATS)
    logit_all = X[:, :nb] @ w[:nb]  # inference: good-play tempo terms only
    for name, m in (('all', te), ('good-play', te & good)):
        if m.any():
            ll, acc = metrics(logit_all[m], y[m], vals[m])
            print(f'test {name:10s} logloss {ll:.4f}  decisive-accuracy {acc:.3f}  (n={m.sum()})')
    if (te & good).any():
        for ply in sorted(set(plies[te & good])):
            m = te & good & (plies == ply)
            ll, acc = metrics(logit_all[m], y[m], vals[m])
            print(f'   good ply {ply:2d}: logloss {ll:.4f} accuracy {acc:.3f} (n={m.sum()})')
        jp, jt, n = jumpiness(rows, logit_all, te & good)
        print(f'jumpiness on good-play test games: predicted {jp:.3f}, exact {jt:.3f}  ({n} consecutive pairs)')
        old = ev / 100.0
        jp0, _, _ = jumpiness(rows, old, te & good)
        print(f'   (eval column stored in the data, i.e. the eval used when generating: {jp0:.3f})')
    w = fit(X, y)  # final fit on everything
    print('weights (exported):')
    for (name, _), wi in zip(FEATS, w[:nb]):
        print(f'  {name:12s} {wi:+.4f}')
    print('noisy-data tempo terms (not exported): ' + ', '.join(f'{t} {wi:+.3f}' for t, wi in zip(TEMPO, w[nb:])))
    with open('src/eval_weights.hpp', 'w') as f:
        f.write('// Generated by tools/fit_eval.py from solver-labelled positions. Logistic weights.\n')
        f.write('#pragma once\n')
        for (name, _), wi in zip(FEATS, w[:nb]):
            f.write(f'constexpr double W_{name} = {wi:.6f};\n')
    out = sys.argv[0].replace('fit_eval.py', '') + '../data/weights_latest.txt'
    with open(out, 'w') as f:
        for (name, _), wi in zip(FEATS, w[:nb]):
            f.write(f'{name} {wi:.6f}\n')
    print('wrote src/eval_weights.hpp and', out)


if __name__ == '__main__':
    main()
