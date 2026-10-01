// Position features for the evaluation: territory ("who gets where first") and what each
// player's snakes could still grow into.
#pragma once
#include "bounds.hpp"

// Parameters of the hand-made evaluation (also the default territory head start).
struct HandEvalParams {
    int headStart = 1;       // the side to move expands this many steps before the opponent
    int territoryWeight = 4; // centi-cells per cell of territory difference ...
    int territoryCap = 30;   // ... counting at most this many cells
    int lengthWeight = 15;   // centi-cells per cell of current longest-snake difference
    int usablePercent = 100; // share of the claimed territory a snake can actually fill
    int newSnakePenalty = 2; // cells lost by having to start a fresh snake
};
inline HandEvalParams handEval;

// Territory split ("Voronoi", as in Tron bots): both players expand at the same time, one layer
// of cells per step, from the empty cells next to their snake ends through the cells they may
// still use. The side to move starts `headStart` steps earlier. A cell reached by both in the
// same step belongs to nobody. claim[player] receives each player's territory.
inline void splitTerritory(const Position& pos, const SnakeShape shape[2], Bitboard claim[2], int headStart = -1) {
    if (headStart < 0) headStart = handEval.headStart;
    Bitboard empty = pos.emptyCells();
    Bitboard usable[2], start[2];
    for (int c = 0; c < 2; c++) {
        usable[c] = empty & ~neighboursOf(shape[c].body);
        start[c] = pos.piecesLeft[c] ? neighboursOf(shape[c].ends) & usable[c] : 0;
    }
    int a = pos.sideToMove, b = a ^ 1;
    Bitboard frontier[2] = {0, 0}, reached = 0;
    claim[0] = claim[1] = 0;
    for (int step = 0; step < 64; step++) {
        Bitboard newA = step == 0 ? start[a] : neighboursOf(frontier[a]) & usable[a];
        newA &= ~reached;
        int stepB = step - headStart;
        Bitboard newB = stepB < 0 ? 0 : stepB == 0 ? start[b] : neighboursOf(frontier[b]) & usable[b];
        newB &= ~reached;
        Bitboard contested = newA & newB;
        newA &= ~contested;
        newB &= ~contested;
        reached |= newA | newB | contested;
        claim[a] |= newA;
        claim[b] |= newB;
        frontier[a] = newA;
        frontier[b] = newB;
        if (stepB > 0 && !newA && !newB) break;
    }
}

// Raw per-player numbers. The training data (gendata) stores them as CSV columns, and
// tools/fit_eval.py turns them into the weighted features of the learned evaluation.
// The CSV column name of each field is given in brackets.
struct PlayerFeatures {
    int longest;             // [L]        longest snake now
    int cellsLeft;           // [rem]      cells in the pieces still in hand
    int piecesLeft;          // [pieces]
    int territory;           // [terr]     cells claimed in the territory split
    int clusterTerritory;    // [bestTerr] best cluster (own cells + territory): its territory ...
    int clusterOwn;          // [bestOwn]  ... and its own cells
    int lowerBound;          // [lb]       snake-length bounds (bounds.hpp)
    int upperBound;          // [ub]
    int freshRegion;         // [fresh]    largest untouched region the opponent does not claim
    int ends;                // [ends]     snake ends
    int liveEnds;            // [liveEnds] snake ends that can still grow
    int snakeOwn;            // [singleOwn]  best single snake: its length ...
    int snakeTerritory;      // [singleTerr] ... and the territory reachable from its ends
    int snakes;              // [comps]    number of snakes
};

inline const char* const PLAYER_FEATURE_COLUMNS[14] = {"L", "rem", "pieces", "terr", "bestTerr", "bestOwn", "lb", "ub",
                                                       "fresh", "ends", "liveEnds", "singleOwn", "singleTerr", "comps"};

inline void bestCluster(const SnakeShape& shape, Bitboard claim, PlayerFeatures& f) {
    f.clusterTerritory = 0;
    f.clusterOwn = f.longest;
    int bestValue = f.longest;
    forEachRegion(shape.own | claim, [&](Bitboard cluster) {
        int territory = countCells(cluster & claim), own = countCells(cluster & shape.own);
        int value = own + std::min(territory, f.cellsLeft);
        if (value > bestValue || (value == bestValue && territory > f.clusterTerritory)) {
            bestValue = value;
            f.clusterTerritory = territory;
            f.clusterOwn = own;
        }
    });
}

// The snake with the most (length + territory reachable from its ends).
inline void bestSingleSnake(const Position& pos, int c, const SnakeShape& shape, Bitboard claim, PlayerFeatures& f) {
    f.snakeOwn = f.longest;
    f.snakeTerritory = 0;
    f.snakes = 0;
    int bestValue = -1;
    for (int j = 0; j < pos.snakeCount[c]; j++) {
        Bitboard snake = pos.snakes[c][j];
        f.snakes++;
        Bitboard seed = neighboursOf(snake & shape.ends) & claim;
        int territory = seed ? countCells(floodFill(seed, claim)) : 0;
        int value = countCells(snake) + std::min(territory, f.cellsLeft);
        if (value > bestValue || (value == bestValue && territory > f.snakeTerritory)) {
            bestValue = value;
            f.snakeOwn = countCells(snake);
            f.snakeTerritory = territory;
        }
    }
}

inline void computeFeatures(const Position& pos, PlayerFeatures f[2], int headStart) {
    SnakeShape shape[2] = {snakeShapeOf(pos.cells[RED]), snakeShapeOf(pos.cells[BLUE])};
    Bitboard claim[2];
    splitTerritory(pos, shape, claim, headStart);
    Bitboard untouched = pos.emptyCells() & ~neighboursOf(pos.occupied());
    for (int c = 0; c < 2; c++) {
        PlayerFeatures& x = f[c];
        x.longest = pos.longestSnake[c];
        x.cellsLeft = cellsInPieces(pos.piecesLeft[c]);
        x.piecesLeft = pieceCount(pos.piecesLeft[c]);
        x.territory = countCells(claim[c]);
        SnakeBounds bounds = snakeBounds(pos, c);
        x.lowerBound = bounds.lower;
        x.upperBound = bounds.upper;
        bestCluster(shape[c], claim[c], x);
        x.freshRegion = largestRegionSize(untouched & ~claim[c ^ 1]);
        Bitboard growable = pos.emptyCells() & ~neighboursOf(shape[c].body);
        x.ends = countCells(shape[c].ends);
        x.liveEnds = countCells(shape[c].ends & neighboursOf(growable));
        bestSingleSnake(pos, c, shape[c], claim[c], x);
        if (!shape[c].own) {
            // No snake yet (blue before its first move): describe the potential of a fresh snake
            // instead of "nothing" - this situation does not occur in the training data.
            x.freshRegion = largestRegionSize(untouched);
            x.snakeTerritory = x.clusterTerritory = std::min(x.freshRegion, 40);
            x.snakes = 1;
            x.ends = x.liveEnds = 2;
        }
    }
}
