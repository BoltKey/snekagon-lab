// Heuristic evaluation: "who gets where first" (Voronoi / Tron style) from the
// growable snake ends of both players, turned into an estimate of each player's
// final longest snake.
#pragma once
#include <cmath>
#include "core.hpp"
#include "eval_weights.hpp"

struct EvalParams {
    int head = 1;      // side to move expands this many BFS layers before the opponent
    int terrW = 4;     // centi-cells per cell of territory difference (capped)
    int terrCap = 30;  // territory counted up to this many cells
    int lenW = 15;     // centi-cells per cell of current longest-snake difference
    int pack = 100;    // % of claimed territory a snake can actually use
    int newPen = 2;    // cells penalty for having to start a fresh snake
};
inline EvalParams EP;

struct EvalInfo {
    int est[2], terr[2], L[2];
    BB claim[2];
};

// Voronoi split of the growable space between the two players.
inline void voronoi(const Pos& s, const Shape sh[2], BB claim[2]) {
    BB E = s.empty();
    BB G[2], F[2];
    for (int c = 0; c < 2; c++) {
        G[c] = E & ~expand(sh[c].full);
        F[c] = s.avail[c] ? expand(sh[c].ends) & G[c] : 0;
    }
    int a = s.stm, b = a ^ 1;
    BB front[2] = {0, 0}, visited = 0;
    claim[0] = claim[1] = 0;
    for (int t = 0; t < 64; t++) {
        BB na = t == 0 ? F[a] : expand(front[a]) & G[a];
        na &= ~visited;
        int tb = t - EP.head;
        BB nb = tb < 0 ? 0 : tb == 0 ? F[b] : expand(front[b]) & G[b];
        nb &= ~visited;
        BB both = na & nb;
        na &= ~both; nb &= ~both;
        visited |= na | nb | both;
        claim[a] |= na; claim[b] |= nb;
        front[a] = na; front[b] = nb;
        if (tb > 0 && !na && !nb) break;
    }
}

inline int estimateFinal(const Pos& s, int c, const Shape& sh, BB myClaim, BB oppClaim, int L) {
    uint8_t av = s.avail[c];
    if (!av) return L;
    int est = L;
    BB U = sh.own | myClaim;
    while (U) {
        BB K = flood(bit(lsb(U)), U);
        U &= ~K;
        int terr = popc(K & myClaim);
        if (!terr) continue;
        int v = popc(K & sh.own) + T.fit[av][terr * EP.pack / 100];
        est = std::max(est, v);
    }
    // a fresh snake somewhere the opponent does not reach first
    BB occ = s.occ();
    BB fresh = s.empty() & ~expand(occ) & ~oppClaim;
    int bestRegion = 0;
    while (fresh) {
        BB r = flood(bit(lsb(fresh)), fresh);
        fresh &= ~r;
        bestRegion = std::max(bestRegion, popc(r));
    }
    if (bestRegion) est = std::max(est, (int)T.fit[av][bestRegion * EP.pack / 100] - EP.newPen);
    return est;
}

// Raw per-player features for training (see tools/fit_eval.py).
struct PlayerFeat {
    int L, rem, pieces, terr, bestTerr, bestOwn, lb, ub, fresh, ends, liveEnds, singleOwn, singleTerr, comps;
};
inline void features(const Pos& s, PlayerFeat f[2]) {
    Shape sh[2] = {shapeOf(s.own[0]), shapeOf(s.own[1])};
    BB claim[2];
    voronoi(s, sh, claim);
    BB occ = s.occ();
    for (int c = 0; c < 2; c++) {
        PlayerFeat& x = f[c];
        x.L = longest(s.own[c]);
        x.rem = remainingCells(s.avail[c]);
        x.pieces = pieceCount(s.avail[c]);
        x.terr = popc(claim[c]);
        snakeBounds(s, c, x.lb, x.ub);
        // cluster with the best (own + usable territory)
        x.bestTerr = 0; x.bestOwn = x.L;
        int bestV = x.L;
        BB U = sh[c].own | claim[c];
        while (U) {
            BB K = flood(bit(lsb(U)), U);
            U &= ~K;
            int t = popc(K & claim[c]), o = popc(K & sh[c].own);
            int v = o + std::min(t, x.rem);
            if (v > bestV || (v == bestV && t > x.bestTerr)) { bestV = v; x.bestTerr = t; x.bestOwn = o; }
        }
        BB fresh = s.empty() & ~expand(occ) & ~claim[c ^ 1];
        x.fresh = 0;
        while (fresh) { BB r = flood(bit(lsb(fresh)), fresh); fresh &= ~r; x.fresh = std::max(x.fresh, popc(r)); }
        BB G = s.empty() & ~expand(sh[c].full);
        x.ends = popc(sh[c].ends);
        BB le = sh[c].ends & expand(G);
        x.liveEnds = popc(le);
        // best single snake: |C| + territory reachable from its own ends
        x.singleOwn = x.L; x.singleTerr = 0; x.comps = 0;
        int bestS = -1;
        BB b = sh[c].own;
        while (b) {
            BB C = componentOf(b, lsb(b));
            b &= ~C;
            x.comps++;
            BB seed = expand(C & sh[c].ends) & claim[c];
            int t = seed ? popc(flood(seed, claim[c])) : 0;
            int v = popc(C) + std::min(t, x.rem);
            if (v > bestS || (v == bestS && t > x.singleTerr)) { bestS = v; x.singleOwn = popc(C); x.singleTerr = t; }
        }
    }
}

// Learned evaluation: 100 * logit(P(side to move wins)), weights fitted on
// solver-labelled positions (tools/fit_eval.py).
inline double learnedLogit(const Pos& s) {
    PlayerFeat f[2];
    features(s, f);
    const PlayerFeat& a = f[s.stm];
    const PlayerFeat& b = f[s.stm ^ 1];
    auto single = [](const PlayerFeat& x) { return x.singleOwn + std::min(x.rem, x.singleTerr); };
    auto cluster = [](const PlayerFeat& x) { return x.bestOwn + std::min(x.rem, x.bestTerr); };
    auto deficit = [](const PlayerFeat& x) { return std::max(0, x.rem - x.singleTerr); };
    double z = W_bias;
    z += W_dL * (a.L - b.L);
    z += W_dSingle * (single(a) - single(b));
    z += W_dCluster * (cluster(a) - cluster(b));
    z += W_dDeficit * (deficit(a) - deficit(b));
    z += W_dUB * (a.ub - b.ub);
    z += W_dRem * (a.rem - b.rem);
    z += W_dTerr20 * (std::min(a.terr, 20) - std::min(b.terr, 20));
    z += W_dTerr * (std::min(a.terr, 60) - std::min(b.terr, 60));
    z += W_dSingleTerr * (std::min(a.singleTerr, 40) - std::min(b.singleTerr, 40));
    z += W_dComps * (a.comps - b.comps);
    z += W_dLiveEnds * (a.liveEnds - b.liveEnds);
    z += W_dFresh * (std::min(a.fresh, 30) - std::min(b.fresh, 30));
    z += W_stmIsP1 * (s.stm == 0 ? 1.0 : -1.0);
    return z;
}
inline bool g_handEval = false;
inline int evaluateHand(const Pos& s, EvalInfo* info = nullptr);
inline int evaluate(const Pos& s, EvalInfo* info = nullptr) {
    if (g_handEval || info) return evaluateHand(s, info);
    double z = learnedLogit(s);
    z = std::max(-30.0, std::min(30.0, z));
    return (int)std::lround(100 * z);
}

// hand-made evaluation (first version), centi-cells from the side to move's perspective
inline int evaluateHand(const Pos& s, EvalInfo* info) {
    Shape sh[2] = {shapeOf(s.own[0]), shapeOf(s.own[1])};
    BB claim[2];
    voronoi(s, sh, claim);
    int est[2], L[2], terr[2];
    for (int c = 0; c < 2; c++) {
        L[c] = longest(s.own[c]);
        terr[c] = popc(claim[c]);
    }
    for (int c = 0; c < 2; c++) est[c] = estimateFinal(s, c, sh[c], claim[c], claim[c ^ 1], L[c]);
    int a = s.stm, b = a ^ 1;
    int v = 100 * (est[a] - est[b]) +
            EP.terrW * (std::min(terr[a], EP.terrCap) - std::min(terr[b], EP.terrCap)) +
            EP.lenW * (L[a] - L[b]);
    if (info) {
        for (int c = 0; c < 2; c++) {
            info->est[c] = est[c]; info->terr[c] = terr[c]; info->L[c] = L[c]; info->claim[c] = claim[c];
        }
    }
    return v;
}
