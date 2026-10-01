// CSV rows of training data for the learned evaluation (read by tools/fit_eval.py):
//   game, ply, stm, value (exact, for the side to move), the raw features of the side to move
//   (columns *_s) and of the opponent (*_o), the hand-made evaluation, the position as text.
#pragma once
#include <cstdio>
#include <string>
#include "evaluation.hpp"
#include "notation.hpp"

inline void printDataHeader() {
    printf("game,ply,stm,value");
    for (const char* side : {"s", "o"})
        for (const char* column : PLAYER_FEATURE_COLUMNS) printf(",%s_%s", column, side);
    printf(",eval,pos\n");
}

inline std::string dataRow(int game, const Position& pos, int value, int headStart) {
    PlayerFeatures f[2];
    computeFeatures(pos, f, headStart);
    char buffer[512];
    snprintf(buffer, sizeof buffer, "%d,%d,%d,%d", game, (int)pos.ply, (int)pos.sideToMove, value);
    std::string row = buffer;
    for (int c : {(int)pos.sideToMove, (int)pos.sideToMove ^ 1}) {
        const PlayerFeatures& x = f[c];
        snprintf(buffer, sizeof buffer, ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", x.longest, x.cellsLeft, x.piecesLeft,
                 x.territory, x.clusterTerritory, x.clusterOwn, x.lowerBound, x.upperBound, x.freshRegion, x.ends,
                 x.liveEnds, x.snakeOwn, x.snakeTerritory, x.snakes);
        row += buffer;
    }
    snprintf(buffer, sizeof buffer, ",%d,", evaluateHandMade(pos));
    row += buffer;
    row += positionToString(pos) + "\n";
    return row;
}
