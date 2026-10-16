// Static evaluation of a position, from the side to move's point of view.
//
// The default is a learned evaluation: a logistic regression that predicts
// P(side to move wins) from 14 features. The weights are fitted by tools/fit_eval.py on
// positions labelled by the exact solver and compiled in from src/eval_weights.hpp.
// Scores are 100 * logit(P): 0 = even, +100 = about 73 %, +200 = about 88 %.
//
// The first, hand-made evaluation (estimated final snake lengths in centi-cells) is still here:
// gendata writes it into the training data and the engine "abhand" uses it.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include "../src/eval_weights.hpp"
#include "features.hpp"

// ---- learned evaluation ----------------------------------------------------------------------

// Feature names match the weight files and tools/fit_eval.py. "d" = side to move minus opponent.
enum Feature { F_bias, F_dL, F_dSingle, F_dCluster, F_dDeficit, F_dUB, F_dRem, F_dTerr20, F_dTerr, F_dSingleTerr, F_dComps, F_dLiveEnds, F_dFresh, F_stmIsP1, NUM_FEATURES };
inline const char* const FEATURE_NAMES[NUM_FEATURES] = {"bias", "dL", "dSingle", "dCluster", "dDeficit", "dUB", "dRem", "dTerr20", "dTerr", "dSingleTerr", "dComps", "dLiveEnds", "dFresh", "stmIsP1"};

// Two weight sets: 0 = compiled in, 1 = loaded at runtime (engine "ab:<file>").
inline double featureWeights[2][NUM_FEATURES] = {
    {W_bias, W_dL, W_dSingle, W_dCluster, W_dDeficit, W_dUB, W_dRem, W_dTerr20, W_dTerr, W_dSingleTerr, W_dComps, W_dLiveEnds, W_dFresh, W_stmIsP1},
    {}};
inline int featureHeadStart[2] = {EVAL_HEAD, 0};  // territory head start the weights were trained with
inline int activeWeightSet = 0;
inline bool useHandMadeEval = false;  // engine "abhand"

// Reads a weights file ("<name> <value>" per line, optional "head <n>") into a weight set.
inline bool loadWeights(const std::string& path, int set) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return false;
    char name[64];
    double value;
    int found = 0;
    featureHeadStart[set] = 1;  // files without a "head" line were trained with head start 1
    while (fscanf(file, "%63s %lf", name, &value) == 2) {
        if (std::string(name) == "head") featureHeadStart[set] = (int)value;
        for (int i = 0; i < NUM_FEATURES; i++)
            if (std::string(name) == FEATURE_NAMES[i]) { featureWeights[set][i] = value; found++; }
    }
    fclose(file);
    return found == NUM_FEATURES;
}

inline void featureVector(const Position& pos, double x[NUM_FEATURES]) {
    PlayerFeatures f[2];
    computeFeatures(pos, f, featureHeadStart[activeWeightSet]);
    const PlayerFeatures& me = f[pos.sideToMove];
    const PlayerFeatures& opp = f[pos.sideToMove ^ 1];
    auto single = [](const PlayerFeatures& p) { return p.snakeOwn + std::min(p.cellsLeft, p.snakeTerritory); };
    auto cluster = [](const PlayerFeatures& p) { return p.clusterOwn + std::min(p.cellsLeft, p.clusterTerritory); };
    auto deficit = [](const PlayerFeatures& p) { return std::max(0, p.cellsLeft - p.snakeTerritory); };
    x[F_bias] = 1;
    x[F_dL] = me.longest - opp.longest;
    x[F_dSingle] = single(me) - single(opp);
    x[F_dCluster] = cluster(me) - cluster(opp);
    x[F_dDeficit] = deficit(me) - deficit(opp);
    x[F_dUB] = me.upperBound - opp.upperBound;
    x[F_dRem] = me.cellsLeft - opp.cellsLeft;
    x[F_dTerr20] = std::min(me.territory, 20) - std::min(opp.territory, 20);
    x[F_dTerr] = std::min(me.territory, 60) - std::min(opp.territory, 60);
    x[F_dSingleTerr] = std::min(me.snakeTerritory, 40) - std::min(opp.snakeTerritory, 40);
    x[F_dComps] = me.snakes - opp.snakes;
    x[F_dLiveEnds] = me.liveEnds - opp.liveEnds;
    x[F_dFresh] = std::min(me.freshRegion, 30) - std::min(opp.freshRegion, 30);
    x[F_stmIsP1] = pos.sideToMove == RED ? 1.0 : -1.0;
}

// logit(P(side to move wins))
// Kept out of line, so that every caller gets the same rounding: inlined and unrolled, the sum's
// multiply-adds may be fused (FMA) in some callers and not in others, which changes the last bit
// of the logit, now and then a move order, and so the node counts.
[[gnu::noinline]] inline double learnedLogit(const Position& pos) {
    double x[NUM_FEATURES];
    featureVector(pos, x);
    double z = 0;
    for (int i = 0; i < NUM_FEATURES; i++) z += featureWeights[activeWeightSet][i] * x[i];
    if (activeWeightSet == 0) z *= EVAL_PLY_SCALE[std::min<int>(pos.ply, 15)];  // early-game calibration
    return z;
}

// ---- hand-made evaluation --------------------------------------------------------------------

// Estimated final longest snake of player c: the best cluster of own cells + territory, or a
// fresh snake somewhere the opponent does not reach first.
inline int estimateFinalLength(const Position& pos, int c, const SnakeShape& shape, Bitboard myClaim, Bitboard opponentClaim) {
    PieceSet left = pos.piecesLeft[c];
    int current = pos.longestSnake[c];
    if (!left) return current;
    int estimate = current;
    forEachRegion(shape.own | myClaim, [&](Bitboard cluster) {
        int territory = countCells(cluster & myClaim);
        if (!territory) return;
        int reachable = countCells(cluster & shape.own) + pieceTables.bestFill[left][territory * handEval.usablePercent / 100];
        estimate = std::max(estimate, reachable);
    });
    Bitboard fresh = pos.emptyCells() & ~neighboursOf(pos.occupied()) & ~opponentClaim;
    int freshRegion = largestRegionSize(fresh);
    if (freshRegion)
        estimate = std::max(estimate, (int)pieceTables.bestFill[left][freshRegion * handEval.usablePercent / 100] - handEval.newSnakePenalty);
    return estimate;
}

// Centi-cells for the side to move.
inline int evaluateHandMade(const Position& pos) {
    SnakeShape shape[2] = {snakeShapeOf(pos.cells[RED]), snakeShapeOf(pos.cells[BLUE])};
    Bitboard claim[2];
    splitTerritory(pos, shape, claim);
    int estimate[2], territory[2], length[2];
    for (int c = 0; c < 2; c++) {
        length[c] = pos.longestSnake[c];
        territory[c] = countCells(claim[c]);
        estimate[c] = estimateFinalLength(pos, c, shape[c], claim[c], claim[c ^ 1]);
    }
    int a = pos.sideToMove, b = a ^ 1;
    return 100 * (estimate[a] - estimate[b]) +
           handEval.territoryWeight * (std::min(territory[a], handEval.territoryCap) - std::min(territory[b], handEval.territoryCap)) +
           handEval.lengthWeight * (length[a] - length[b]);
}

// ---- the evaluation used by the engine -------------------------------------------------------

// Integer score for the side to move: 100 * logit, clamped to +-3000.
inline int evaluate(const Position& pos) {
    if (useHandMadeEval) return evaluateHandMade(pos);
    double z = learnedLogit(pos);
    z = std::max(-30.0, std::min(30.0, z));
    return (int)std::lround(100 * z);
}
