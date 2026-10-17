"""Score, Elo with a 95% interval and the SPRT log-likelihood ratio of engine A from `snek arena` logs.

    python tools/arena_stats.py [--elo0 0 --elo1 15 --alpha 0.05 --beta 0.1] arena1.txt [arena2.txt ...]

Every opening is played twice with colours swapped (a pair; A is P1 in the pair's first game), so
the statistics are pentanomial: the pair scores 0, 1/4, ..., 1 are the samples. The SPRT is the
generalised SPRT on the pair scores (normal approximation) for logistic Elo elo0 against elo1.
"""
import argparse, math, re


def elo(s):
    s = min(max(s, 1e-6), 1 - 1e-6)
    return -400 * math.log10(1 / s - 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--elo0', type=float, default=0)
    ap.add_argument('--elo1', type=float, default=15)
    ap.add_argument('--alpha', type=float, default=0.05)
    ap.add_argument('--beta', type=float, default=0.1)
    a = ap.parse_args()
    pairs, names = {}, set()
    for fi, path in enumerate(a.logs):
        for line in open(path):
            m = re.match(r'pair (\d+)\s+P1=(\S+) P2=(\S+)\s+(\d+)-(\d+)', line)
            if not m:
                continue
            key = (fi, int(m[1]))
            l1, l2 = int(m[4]), int(m[5])
            p1 = 1.0 if l1 > l2 else 0.0 if l1 < l2 else 0.5
            g = pairs.setdefault(key, [])
            if not g:
                names.add((m[2], m[3]))
            g.append(p1 if not g else 1 - p1)  # A is P1 in the first game of a pair
    full = [v for v in pairs.values() if len(v) == 2]
    n = len(full)
    if n < 2:
        print(f'{n} complete pairs: not enough data')
        return
    ps = [sum(v) / 2 for v in full]
    mean = sum(ps) / n
    var = sum((x - mean) ** 2 for x in ps) / (n - 1)
    se = math.sqrt(var / n)
    games = [x for v in full for x in v]
    w, d, l = games.count(1.0), games.count(0.5), games.count(0.0)
    penta = [sum(1 for v in full if sum(v) == k / 2) for k in range(5)]
    split = sum(1 for v in full if v[0] != v[1] and sum(v) == 1) / n
    print(f'engines (A first): {sorted(names)}')
    print(f'{n} pairs, {2 * n} games: A +{w} ={d} -{l}; pair scores 0/0.5/1/1.5/2: {penta}; pairs split 1-1 by colour {split:.2f}')
    print(f'A score {mean:.4f} +- {1.96 * se:.4f} (95%), Elo {elo(mean):+.1f} [{elo(mean - 1.96 * se):+.1f}, {elo(mean + 1.96 * se):+.1f}]')
    s0, s1 = 1 / (1 + 10 ** (-a.elo0 / 400)), 1 / (1 + 10 ** (-a.elo1 / 400))
    llr = n * (s1 - s0) * (2 * mean - s0 - s1) / (2 * var) if var > 0 else 0.0
    lo, hi = math.log(a.beta / (1 - a.alpha)), math.log((1 - a.beta) / a.alpha)
    verdict = 'H1 accepted (A stronger by elo1)' if llr >= hi else 'H0 accepted (no gain of elo1)' if llr <= lo else 'continue'
    print(f'SPRT elo0={a.elo0:g} elo1={a.elo1:g} alpha={a.alpha:g} beta={a.beta:g}: LLR {llr:.2f} in [{lo:.2f}, {hi:.2f}] -> {verdict}')


if __name__ == '__main__':
    main()
