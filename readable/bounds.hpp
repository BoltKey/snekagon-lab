// Snake lengths and sound bounds on a player's final longest snake.
//
// The bounds are what makes the exact solver fast: once one player's guaranteed length (lower
// bound) beats everything the opponent can still reach (upper bound), the game is decided and
// nothing below that node has to be searched.
#pragma once
#include "rules.hpp"

inline int longestSnakeIn(Bitboard cells) {
    return largestRegionSize(cells);
}

struct SnakeBounds {
    int lower;  // the longest snake now (snakes never shrink)
    int upper;  // no continuation of the game can give a longer snake
};

// Why the upper bound holds:
//  - A player can never again use a cell next to one of its body cells (that body cell would get
//    a third neighbour). So all future own cells lie in growable = empty minus such cells.
//  - The final longest snake is connected, so it lies inside one connected cluster of
//    (own cells + growable cells).
//  - New cells come in whole pieces and every piece lies inside one connected region of
//    growable cells, so a cluster gains at most bestFill(sum over its regions of bestFill(region)).
//
// Implementation: clusters are built from the growable regions, merged by the snakes touching
// them (regions are small blobs and cheap to flood; snakes are only attached by adjacency).
inline SnakeBounds snakeBounds(const Position& pos, int player) {
    SnakeBounds bounds;
    bounds.lower = pos.longestSnake[player];
    PieceSet left = pos.piecesLeft[player];
    if (!left) { bounds.upper = bounds.lower; return bounds; }

    SnakeShape shape = snakeShapeOf(pos.cells[player]);
    Bitboard growable = pos.emptyCells() & ~neighboursOf(shape.body);

    Bitboard clusterCells[BOARD_CELLS];
    int ownCells[BOARD_CELLS], newCells[BOARD_CELLS];  // own cells / most new cells per cluster
    int clusters = 0;
    forEachRegion(growable, [&](Bitboard region) {
        clusterCells[clusters] = region;
        ownCells[clusters] = 0;
        newCells[clusters] = pieceTables.bestFill[left][countCells(region)];
        clusters++;
    });

    for (int j = 0; j < pos.snakeCount[player]; j++) {
        Bitboard snake = pos.snakes[player][j], around = neighboursOf(snake);
        int into = -1;  // the cluster this snake joins
        for (int i = 0; i < clusters; i++) {
            if (i == into || !(clusterCells[i] & around)) continue;
            if (into < 0) { into = i; continue; }
            // the snake connects cluster i with cluster `into`: merge them, then move the last
            // cluster into the freed slot i and look at slot i again
            clusterCells[into] |= clusterCells[i];
            ownCells[into] += ownCells[i];
            newCells[into] += newCells[i];
            clusters--;
            clusterCells[i] = clusterCells[clusters];
            ownCells[i] = ownCells[clusters];
            newCells[i] = newCells[clusters];
            if (into == clusters) into = i;
            i--;
        }
        if (into >= 0) {
            clusterCells[into] |= snake;
            ownCells[into] += countCells(snake);
        }
    }

    bounds.upper = bounds.lower;
    for (int i = 0; i < clusters; i++)
        bounds.upper = std::max(bounds.upper, ownCells[i] + pieceTables.bestFill[left][std::min(newCells[i], GRID_SIZE)]);
    return bounds;
}

// The same bounds computed the slow, obvious way (flooding own + growable cells directly).
// `snek verify` checks that snakeBounds() always agrees with it.
inline SnakeBounds snakeBoundsReference(const Position& pos, int player) {
    Bitboard own = pos.cells[player];
    SnakeBounds bounds;
    bounds.lower = longestSnakeIn(own);
    PieceSet left = pos.piecesLeft[player];
    if (!left) { bounds.upper = bounds.lower; return bounds; }
    SnakeShape shape = snakeShapeOf(own);
    Bitboard growable = pos.emptyCells() & ~neighboursOf(shape.body);
    bounds.upper = bounds.lower;
    forEachRegion(own | growable, [&](Bitboard cluster) {
        Bitboard growableInCluster = cluster & growable;
        if (!growableInCluster) return;
        int capacity = 0;
        forEachRegion(growableInCluster, [&](Bitboard region) { capacity += pieceTables.bestFill[left][countCells(region)]; });
        int reachable = countCells(cluster & own) + pieceTables.bestFill[left][std::min(capacity, GRID_SIZE)];
        bounds.upper = std::max(bounds.upper, reachable);
    });
    return bounds;
}
