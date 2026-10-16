// Snake lengths and sound bounds on a player's final longest snake.
//
// The bounds are what makes the exact solver fast: once one player's guaranteed length (lower
// bound) beats everything the opponent can still reach (upper bound), the game is decided and
// nothing below that node has to be searched.
#pragma once
#include <functional>
#include "rules.hpp"

inline int longestSnakeIn(Bitboard cells) {
    return largestRegionSize(cells);
}

// The longest snake of `player` if it placed p (a legal placement for that player) now: the piece
// and every own snake it touches become one snake.
inline int longestSnakeAfter(const Position& pos, int player, const Placement& p) {
    int joined = p.cellCount;
    for (int j = 0; j < pos.snakeCount[player]; j++)
        if (pos.snakes[player][j] & p.border) joined += countCells(pos.snakes[player][j]);
    return std::max<int>(pos.longestSnake[player], joined);
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
//  - Every old snake ends up in the final snake whole or not at all, and along the final snake
//    old snakes and new pieces alternate. With k pieces left it therefore contains at most k + 1
//    old snakes: only the k + 1 longest snakes of a cluster count.
//
// Implementation: clusters are built from the growable regions, merged by the snakes touching
// them (regions are small blobs and cheap to flood; snakes are only attached by adjacency).

// A player's clusters, which the solver keeps to look for threats (solver.hpp).
struct SnakeClusters {
    int count = 0;
    Bitboard cells[BOARD_CELLS];    // the growable cells and own snakes of each cluster
    int upper[BOARD_CELLS];         // the longest final snake possible inside each cluster
    int regionCount = 0;
    Bitboard regions[BOARD_CELLS];  // the connected regions of growable cells (of all clusters)
};

// The cells `player` may still occupy: empty and not next to its snake body.
inline Bitboard growableCells(const Position& pos, int player) {
    return pos.emptyCells() & ~neighboursOf(snakeShapeOf(pos.cells[player]).body);
}

// Upper bound (at least `lower`) on the final longest snake of a player with the pieces `left`,
// the snakes snakes[0 .. snakeCount) and the growable cells `growable`, cluster by cluster as
// described above. With `record`, the clusters are kept there as well.
inline int clusterUpperBound(Bitboard growable, const Bitboard* snakes, int snakeCount, PieceSet left, int lower,
                             SnakeClusters* record) {
    if (record) record->count = record->regionCount = 0;
    Bitboard clusterCells[BOARD_CELLS];
    int ownCells[BOARD_CELLS], newCells[BOARD_CELLS];  // own cells / most new cells per cluster
    int clusters = 0;
    forEachRegion(growable, [&](Bitboard region) {
        clusterCells[clusters] = region;
        ownCells[clusters] = 0;
        newCells[clusters] = pieceTables.bestFill[left][countCells(region)];
        clusters++;
        if (record) record->regions[record->regionCount++] = region;
    });

    for (int j = 0; j < snakeCount; j++) {
        Bitboard snake = snakes[j], around = neighboursOf(snake);
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

    // more snakes than one final snake can join: only the k + 1 longest of a cluster count
    int joinable = pieceCount(left) + 1;
    if (snakeCount > joinable) {
        for (int i = 0; i < clusters; i++) {
            int lengths[MAX_SNAKES], inCluster = 0;
            for (int j = 0; j < snakeCount; j++)
                if (clusterCells[i] & snakes[j]) lengths[inCluster++] = countCells(snakes[j]);
            if (inCluster <= joinable) continue;
            std::partial_sort(lengths, lengths + joinable, lengths + inCluster, std::greater<int>());
            ownCells[i] = 0;
            for (int j = 0; j < joinable; j++) ownCells[i] += lengths[j];
        }
    }

    int upper = lower;
    for (int i = 0; i < clusters; i++) {
        int reachable = ownCells[i] + pieceTables.bestFill[left][std::min(newCells[i], GRID_SIZE)];
        upper = std::max(upper, reachable);
        if (record) {
            record->cells[record->count] = clusterCells[i];
            record->upper[record->count++] = reachable;
        }
    }
    return upper;
}

// The bounds of `player`; with `clusters`, its clusters are kept there as well.
inline SnakeBounds snakeBounds(const Position& pos, int player, SnakeClusters* clusters = nullptr) {
    SnakeBounds bounds;
    bounds.lower = pos.longestSnake[player];
    if (clusters) clusters->count = 0;
    PieceSet left = pos.piecesLeft[player];
    if (!left) { bounds.upper = bounds.lower; return bounds; }
    bounds.upper = clusterUpperBound(growableCells(pos, player), pos.snakes[player], pos.snakeCount[player], left,
                                     bounds.lower, clusters);
    return bounds;
}

// The upper bound of `player` after the opponent occupies the empty cells `blocked` (one
// placement), given the player's clusters before that. Only the clusters that `blocked` touches
// can change, and only those are recomputed - unless the block provably changes nothing: a piece
// lies inside one region of growable cells, and when that region stays connected without it (the
// cells next to the piece are still connected within distance 2 of it), still holds as many piece
// cells, and still touches every snake it touched, the cluster's bound is the same.
inline int upperBoundAfterBlock(const Position& pos, int player, const SnakeClusters& clusters, int lower, Bitboard blocked) {
    int upper = lower;
    PieceSet left = pos.piecesLeft[player];
    Bitboard own = pos.cells[player];
    for (int i = 0; i < clusters.count; i++) {
        Bitboard cluster = clusters.cells[i];
        if (!(cluster & blocked)) {
            upper = std::max(upper, clusters.upper[i]);
            continue;
        }
        Bitboard snakesIn[MAX_SNAKES];
        int snakesInCount = 0;
        for (int j = 0; j < pos.snakeCount[player]; j++)
            if (cluster & pos.snakes[player][j]) snakesIn[snakesInCount++] = pos.snakes[player][j];

        Bitboard blockedGrowable = blocked & cluster & ~own;
        Bitboard region = 0;  // the growable region the block lies in
        for (int k = 0; k < clusters.regionCount; k++)
            if (clusters.regions[k] & blockedGrowable) { region = clusters.regions[k]; break; }
        Bitboard regionAfter = region & ~blocked, besideBlock = neighboursOf(blocked) & regionAfter;
        bool unchanged = besideBlock && !(blockedGrowable & ~region) &&
                         pieceTables.bestFill[left][countCells(regionAfter)] == pieceTables.bestFill[left][countCells(region)];
        if (unchanged) {
            Bitboard nearBlock = neighboursOf(neighboursOf(blocked)) & regionAfter;
            Bitboard connected = floodFill(cellBit(firstCell(besideBlock)), nearBlock);
            unchanged = !(besideBlock & ~connected);
            for (int j = 0; j < snakesInCount && unchanged; j++) {
                Bitboard around = neighboursOf(snakesIn[j]);
                if ((around & region) && !(around & regionAfter)) unchanged = false;
            }
        }
        Bitboard growableAfter = cluster & ~own & ~blocked;
        upper = std::max(upper, unchanged ? clusters.upper[i] : clusterUpperBound(growableAfter, snakesIn, snakesInCount, left, lower, nullptr));
    }
    return upper;
}

// ---- the exact one-player optimum ------------------------------------------------------------
//
// Could `player` make a snake of at least `target` cells if the opponent never moved again? This
// is a relaxation of the real game, so a "no" is a sound upper bound, and a much tighter one than
// snakeBounds(): it follows how the fixed piece shapes chain into a path.
//  - The player's future cells are growable cells, and the final snake lies in one cluster.
//  - The opponent's future cells, the order of the player's moves and the rule that a new snake
//    touches nothing are dropped: each of them only takes options away.
//  - The final snake is an induced path or ring of whole old snakes and whole new pieces, and it
//    can be built by attaching each new piece at an end of the path built so far (a ring is
//    closed by the last one).
// A depth-first search builds the path from a seed: an old snake, or, for a path of new pieces
// only, its lowest piece (by piece number). A path with several old snakes is built from the one
// with the lowest index only, so merging a snake with a lower index than the seed is skipped. It
// extends one end, then the other; the extensions are the move generator's grow rules with
// "touches an own cell" narrowed to "touches the end being extended". A path is given up when its
// remaining pieces and the snakes they could still merge are too short, and the last piece is
// placed at either end in one step. Answers are remembered per cluster, keyed by the cluster's
// cells, the player's cells in it and the pieces left: nothing outside the cluster matters.
class OnePlayerOptimum {
  public:
    static constexpr uint64_t SEARCH_BUDGET = 20000;  // search nodes per cluster; beyond it the answer is "maybe"

    struct Statistics {
        uint64_t questions = 0;     // asked of mayReach() and not answered by the longest snake alone
        uint64_t unreachable = 0;   // answered "no"
        uint64_t searchNodes = 0;
        uint64_t outOfBudget = 0;   // a cluster's search ran out of budget (the answer was "maybe")
        void add(const Statistics& o) {
            questions += o.questions;
            unreachable += o.unreachable;
            searchNodes += o.searchNodes;
            outOfBudget += o.outOfBudget;
        }
    } stats;

    void clear() { std::fill(memo.begin(), memo.end(), MemoEntry{}); }

    // False only if `player` surely cannot make a snake of `target` cells; `clusters`: its clusters.
    bool mayReach(const Position& pos, int player, const SnakeClusters& clusters, int target) {
        if (pos.longestSnake[player] >= target) return true;
        PieceSet left = pos.piecesLeft[player];
        if (!left) return false;
        stats.questions++;
        for (int i = 0; i < clusters.count; i++) {
            if (clusters.upper[i] < target) continue;
            Bitboard cluster = clusters.cells[i], own = pos.cells[player] & cluster;
            MemoEntry& known = memo[memoSlot(cluster, own, left)];
            if (known.cluster != cluster || known.own != own || known.left != left) known = MemoEntry{cluster, own, left};
            if (known.reachable >= target) return true;
            if (known.unreachable <= target) continue;
            Bitboard snakesIn[MAX_SNAKES];
            int snakesInCount = 0;
            for (int j = 0; j < pos.snakeCount[player]; j++)
                if (pos.snakes[player][j] & cluster) snakesIn[snakesInCount++] = pos.snakes[player][j];
            Answer answer = reachableInCluster(own, cluster & ~pos.cells[player], snakesIn, snakesInCount, left, target);
            if (answer == UNREACHABLE) {
                known.unreachable = (int8_t)target;
                continue;
            }
            if (answer == REACHABLE) known.reachable = (int8_t)target;
            else stats.outOfBudget++;
            return true;
        }
        stats.unreachable++;
        return false;
    }

  private:
    enum Answer { UNREACHABLE = 0, REACHABLE = 1, OUT_OF_BUDGET = -1 };
    static constexpr int MEMO_LOG2 = 16;
    struct MemoEntry {
        Bitboard cluster = 0, own = 0;
        PieceSet left = 0;
        int8_t reachable = 0;     // the longest target known to be reachable
        int8_t unreachable = 127; // the shortest target known to be unreachable
    };
    std::vector<MemoEntry> memo = std::vector<MemoEntry>(size_t(1) << MEMO_LOG2);

    static size_t memoSlot(Bitboard cluster, Bitboard own, PieceSet left) {
        uint64_t h = (uint64_t)cluster * 0x9E3779B97F4A7C15ULL ^ (uint64_t)(cluster >> 64) * 0xC2B2AE3D27D4EB4FULL ^
                     (uint64_t)own * 0x165667B19E3779F9ULL ^ (uint64_t)(own >> 64) * 0x27D4EB2F165667C5ULL ^ left * 0x85EBCA77C2B2AE63ULL;
        return (h ^ (h >> 29)) & ((size_t(1) << MEMO_LOG2) - 1);
    }

    // The question being searched, inside one cluster.
    Bitboard own = 0, freeCells = 0;  // the player's cells in the cluster, the cluster's growable cells
    Bitboard snakes[MAX_SNAKES];
    int snakeLength[MAX_SNAKES], snakeCount = 0;
    int target = 0;
    bool newPiecesOnly = false;  // a path of new pieces only: it never touches an old snake ...
    int seedPiece = 0;           // ... and its other pieces have higher numbers than its seed piece
    int seedSnake = -1;          // the seed snake: snakes with a lower index are not merged
    uint64_t searchNodes = 0;

    Answer reachableInCluster(Bitboard own_, Bitboard free_, const Bitboard* snakes_, int count, PieceSet left, int target_) {
        target = target_;
        own = own_;
        freeCells = free_;
        snakeCount = count;
        for (int j = 0; j < snakeCount; j++) {
            snakes[j] = snakes_[j];
            snakeLength[j] = countCells(snakes[j]);
        }
        searchNodes = 0;
        Answer answer = searchFromSeeds(left);
        stats.searchNodes += searchNodes;
        return answer;
    }

    Answer searchFromSeeds(PieceSet left) {
        Answer answer = UNREACHABLE;
        // seeds: the old snakes
        newPiecesOnly = false;
        for (int j = 0; j < snakeCount; j++) {
            seedSnake = j;
            Bitboard ends = snakeShapeOf(snakes[j]).ends;
            if (!ends) continue;  // a ring cannot grow
            Answer a = extend(snakes[j], own, 0, left, firstCell(ends), false);
            if (a == REACHABLE) return REACHABLE;
            if (a == OUT_OF_BUDGET) answer = OUT_OF_BUDGET;
            if (searchNodes > SEARCH_BUDGET) return OUT_OF_BUDGET;
        }
        // seeds: paths of new pieces only, seeded by their lowest piece. Such a path lies in one
        // region of the growable cells away from the old snakes, which must hold target cells.
        if (cellsInPieces(left) < target) return answer;
        newPiecesOnly = true;
        Bitboard awayFromSnakes = freeCells & ~neighboursOf(own), bigRegions = 0;
        forEachRegion(awayFromSnakes, [&](Bitboard region) {
            if (pieceTables.bestFill[left][std::min(countCells(region), GRID_SIZE)] >= target) bigRegions |= region;
        });
        if (!bigRegions) return answer;
        for (int piece = 0; piece < NUM_PIECES; piece++) {
            if (!hasPiece(left, piece)) continue;
            PieceSet higher = (PieceSet)(left & ~((2u << piece) - 1));
            if (PIECE_SIZE[piece] + cellsInPieces(higher) < target) continue;
            seedPiece = piece;
            for (int o = pieceTables.firstOrientation[piece]; o < pieceTables.firstOrientation[piece + 1]; o++) {
                const Orientation& orientation = pieceTables.orientations[o];
                Bitboard anchors = geometry.allCells;
                for (int k = 0; k < orientation.size; k++) anchors &= cellsWithOffsetIn(bigRegions, orientation.dq[k], orientation.dr[k]);
                while (anchors) {
                    const Placement& p = pieceTables.placements[orientation.placementAt[firstCell(anchors)]];
                    anchors = withoutFirstCell(anchors);
                    Answer a = extend(p.cells, own | p.cells, p.cells, higher, firstCell(snakeShapeOf(p.cells).ends), false);
                    if (a == REACHABLE) return REACHABLE;
                    if (a == OUT_OF_BUDGET) answer = OUT_OF_BUDGET;
                    if (searchNodes > SEARCH_BUDGET) return OUT_OF_BUDGET;
                }
            }
        }
        return answer;
    }

    // Extends the path `path` (a snake of the player's cells `mine`, of which `placed` are new
    // pieces) with the pieces `left`. keptEnd: the end not extended now; secondEnd: the first end
    // is finished and the other one is being extended.
    Answer extend(Bitboard path, Bitboard mine, Bitboard placed, PieceSet left, int keptEnd, bool secondEnd) {
        int length = countCells(path);
        if (length >= target) return REACHABLE;
        if (!left) return UNREACHABLE;
        if (++searchNodes > SEARCH_BUDGET) return OUT_OF_BUDGET;
        // too short even with every piece left and the longest snakes they could merge?
        int most = length + cellsInPieces(left);
        if (!newPiecesOnly) {
            int lengths[MAX_SNAKES], count = 0;
            for (int j = 0; j < snakeCount; j++)
                if (!(snakes[j] & path)) lengths[count++] = snakeLength[j];
            int merged = std::min(pieceCount(left), count);
            std::partial_sort(lengths, lengths + merged, lengths + count, std::greater<int>());
            for (int j = 0; j < merged; j++) most += lengths[j];
        }
        if (most < target) return UNREACHABLE;
        SnakeShape shape = snakeShapeOf(mine);
        Bitboard ends = path & shape.ends;  // the path is one snake of `mine`
        if (!ends) return UNREACHABLE;      // a ring
        int end = countCells(ends) == 1 ? firstCell(ends) : (ends & ~cellBit(keptEnd)) ? firstCell(ends & ~cellBit(keptEnd)) : firstCell(ends);
        Bitboard activeEnds = secondEnd ? cellBit(end) : ends;
        // with one piece left, placing it at either active end is the last step
        bool lastPiece = pieceCount(left) == 1;
        int childKeptEnd = countCells(ends) == 1 ? end : keptEnd;
        Bitboard extendAt = lastPiece ? activeEnds : cellBit(end);
        int end0 = firstCell(extendAt), end1 = withoutFirstCell(extendAt) ? firstCell(withoutFirstCell(extendAt)) : end0;

        // the extensions, bit-parallel per orientation (growingAnchors with "touches an own cell"
        // narrowed to "touches the end being extended")
        Bitboard usable = freeCells & ~placed;
        if (newPiecesOnly) usable &= ~neighboursOf(own);
        Bitboard notNextToBody = usable & ~neighboursOf(shape.body);
        Bitboard allowed[3] = {notNextToBody & ~shape.touches3 & ~shape.touchesAdjacentPair, notNextToBody & ~shape.touches2,
                               notNextToBody & ~shape.touches1};
        Bitboard mineWithNeighbour = mine & shape.touches1;
        Answer answer = UNREACHABLE;
        for (int piece = NUM_PIECES - 1; piece >= 0; piece--) {  // big pieces first: a path that reaches is found sooner
            if (!hasPiece(left, piece) || (newPiecesOnly && piece <= seedPiece)) continue;
            for (int o = pieceTables.firstOrientation[piece]; o < pieceTables.firstOrientation[piece + 1]; o++) {
                const Orientation& orientation = pieceTables.orientations[o];
                Bitboard anchors = orientation.anchorsNextTo[end0] | orientation.anchorsNextTo[end1];
                for (int k = 0; k < orientation.size && anchors; k++)
                    anchors &= cellsWithOffsetIn(allowed[orientation.innerNeighbours[k]], orientation.dq[k], orientation.dr[k]);
                if (!anchors) continue;
                for (auto [q, r] : orientation.wedgeCells) anchors &= ~cellsWithOffsetIn(mine, q, r);
                for (auto [q, r] : orientation.bridgeCells) anchors &= ~cellsWithOffsetIn(mineWithNeighbour, q, r);
                while (anchors) {
                    const Placement& p = pieceTables.placements[orientation.placementAt[firstCell(anchors)]];
                    anchors = withoutFirstCell(anchors);
                    // the piece joins the path and every old snake it touches
                    Bitboard longer = path | p.cells;
                    bool mergesLowerSnake = false;
                    for (int j = 0; j < snakeCount; j++)
                        if ((snakes[j] & p.border) && !(snakes[j] & path)) {
                            if (!newPiecesOnly && j < seedSnake) mergesLowerSnake = true;
                            longer |= snakes[j];
                        }
                    if (lastPiece) {
                        if (countCells(longer) >= target) return REACHABLE;
                        continue;
                    }
                    if (mergesLowerSnake) continue;
                    Answer a = extend(longer, mine | p.cells, placed | p.cells, (PieceSet)(left & ~(1u << piece)), childKeptEnd, secondEnd);
                    if (a == REACHABLE) return REACHABLE;
                    if (a == OUT_OF_BUDGET) answer = OUT_OF_BUDGET;
                    if (searchNodes > SEARCH_BUDGET) return OUT_OF_BUDGET;
                }
            }
        }
        if (!lastPiece && !secondEnd && countCells(ends) == 2) {
            Answer a = extend(path, mine, placed, left, end, true);
            if (a != UNREACHABLE) return a;
        }
        return answer;
    }
};

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
        std::vector<int> snakeLengths;
        forEachRegion(cluster & own, [&](Bitboard snake) { snakeLengths.push_back(countCells(snake)); });
        std::sort(snakeLengths.begin(), snakeLengths.end(), std::greater<int>());
        int joinable = std::min<int>(snakeLengths.size(), pieceCount(left) + 1);
        int ownReachable = 0;
        for (int j = 0; j < joinable; j++) ownReachable += snakeLengths[j];
        int reachable = ownReachable + pieceTables.bestFill[left][std::min(capacity, GRID_SIZE)];
        bounds.upper = std::max(bounds.upper, reachable);
    });
    return bounds;
}
