// Heuristic evaluation: "who gets where first" (Voronoi / Tron style) from the
// growable snake ends of both players, turned into an estimate of each player's
// final longest snake.
#pragma once
#include <cmath>
#include <cstdio>
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
inline void voronoi(const Pos& s, const Shape sh[2], BB claim[2], int head = -1) {
    if (head < 0) head = EP.head;
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
        int tb = t - head;
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
inline void features(const Pos& s, PlayerFeat f[2], int head) {
    Shape sh[2] = {shapeOf(s.own[0]), shapeOf(s.own[1])};
    BB claim[2];
    voronoi(s, sh, claim, head);
    BB occ = s.occ();
    for (int c = 0; c < 2; c++) {
        PlayerFeat& x = f[c];
        x.L = s.L[c];
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
        for (int j = 0; j < s.ncomp[c]; j++) {
            BB C = s.comp[c][j];
            x.comps++;
            BB seed = expand(C & sh[c].ends) & claim[c];
            int t = seed ? popc(flood(seed, claim[c])) : 0;
            int v = popc(C) + std::min(t, x.rem);
            if (v > bestS || (v == bestS && t > x.singleTerr)) { bestS = v; x.singleOwn = popc(C); x.singleTerr = t; }
        }
        if (!sh[c].own) {
            // no snake yet (player 2 before its first move): describe the potential of a
            // fresh snake instead of "nothing" - this situation is not in the training data
            BB iso = s.empty() & ~expand(occ);
            x.fresh = 0;
            while (iso) { BB r = flood(bit(lsb(iso)), iso); iso &= ~r; x.fresh = std::max(x.fresh, popc(r)); }
            x.singleTerr = x.bestTerr = std::min(x.fresh, 40);
            x.comps = 1;
            x.ends = x.liveEnds = 2;
        }
    }
}

// Learned evaluation: 100 * logit(P(side to move wins)), weights fitted on
// solver-labelled positions (tools/fit_eval.py).
enum { F_bias, F_dL, F_dSingle, F_dCluster, F_dDeficit, F_dUB, F_dRem, F_dTerr20, F_dTerr, F_dSingleTerr, F_dComps, F_dLiveEnds, F_dFresh, F_stmIsP1, NFEAT };
inline const char* FEAT_NAMES[NFEAT] = {"bias", "dL", "dSingle", "dCluster", "dDeficit", "dUB", "dRem", "dTerr20", "dTerr", "dSingleTerr", "dComps", "dLiveEnds", "dFresh", "stmIsP1"};
// weight sets: 0 = compiled in, 1 = loaded at runtime (engine "ab:<file>")
inline double EW[2][NFEAT] = {{W_bias, W_dL, W_dSingle, W_dCluster, W_dDeficit, W_dUB, W_dRem, W_dTerr20, W_dTerr, W_dSingleTerr, W_dComps, W_dLiveEnds, W_dFresh, W_stmIsP1}, {}};
inline int EHEAD[2] = {EVAL_HEAD, 0};  // Voronoi head start the weights were trained with
inline int g_evalSet = 0;
inline bool loadWeights(const std::string& path, int set) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char name[64];
    double w;
    int n = 0;
    EHEAD[set] = 1;  // files without a "head" line were trained with head start 1
    while (fscanf(f, "%63s %lf", name, &w) == 2) {
        if (std::string(name) == "head") EHEAD[set] = (int)w;
        for (int i = 0; i < NFEAT; i++) if (std::string(name) == FEAT_NAMES[i]) { EW[set][i] = w; n++; }
    }
    fclose(f);
    return n == NFEAT;
}

inline void featureVector(const Pos& s, double x[NFEAT]) {
    PlayerFeat f[2];
    features(s, f, EHEAD[g_evalSet]);
    const PlayerFeat& a = f[s.stm];
    const PlayerFeat& b = f[s.stm ^ 1];
    auto single = [](const PlayerFeat& p) { return p.singleOwn + std::min(p.rem, p.singleTerr); };
    auto cluster = [](const PlayerFeat& p) { return p.bestOwn + std::min(p.rem, p.bestTerr); };
    auto deficit = [](const PlayerFeat& p) { return std::max(0, p.rem - p.singleTerr); };
    x[F_bias] = 1;
    x[F_dL] = a.L - b.L;
    x[F_dSingle] = single(a) - single(b);
    x[F_dCluster] = cluster(a) - cluster(b);
    x[F_dDeficit] = deficit(a) - deficit(b);
    x[F_dUB] = a.ub - b.ub;
    x[F_dRem] = a.rem - b.rem;
    x[F_dTerr20] = std::min(a.terr, 20) - std::min(b.terr, 20);
    x[F_dTerr] = std::min(a.terr, 60) - std::min(b.terr, 60);
    x[F_dSingleTerr] = std::min(a.singleTerr, 40) - std::min(b.singleTerr, 40);
    x[F_dComps] = a.comps - b.comps;
    x[F_dLiveEnds] = a.liveEnds - b.liveEnds;
    x[F_dFresh] = std::min(a.fresh, 30) - std::min(b.fresh, 30);
    x[F_stmIsP1] = s.stm == 0 ? 1.0 : -1.0;
}

// Learned evaluation: logit(P(side to move wins)), weights fitted on
// solver-labelled positions (tools/fit_eval.py).
// Kept out of line: inlined into some callers, the compiler fuses its multiply-adds (FMA) and two
// near-equal logits can then order moves differently than elsewhere (node counts, never values).
[[gnu::noinline]] inline double learnedLogit(const Pos& s) {
    double x[NFEAT];
    featureVector(s, x);
    double z = 0;
    for (int i = 0; i < NFEAT; i++) z += EW[g_evalSet][i] * x[i];
    if (g_evalSet == 0) z *= EVAL_PLY_SCALE[std::min<int>(s.ply, 15)];  // early-game calibration
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
        L[c] = s.L[c];
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
