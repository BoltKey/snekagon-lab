// The placement rules and the move generator.
//
// Rules (see RULES.md): a piece goes on empty cells and either
//   - starts a new snake: then it must not touch any piece at all, or
//   - grows the player's own snakes: then afterwards every own cell has at most two own
//     neighbours, and no three own cells touch each other (no "triangle" / sharp turn).
// A player without a legal placement passes; the game ends after two passes in a row.
//
// There are two implementations: isLegalPlacement() checks one placement directly from the
// rules; generateMoves() tests all anchor cells of an orientation at once with bitboard
// arithmetic. `snek verify` checks that both always agree.
#pragma once
#include <vector>
#include "position.hpp"

// What a player's cells imply for the cells around them.
struct SnakeShape {
    Bitboard own;
    Bitboard touches1, touches2, touches3;  // cells with at least 1 / 2 / 3 own neighbours
    Bitboard touchesAdjacentPair;           // cells touching two own cells that touch each other
    Bitboard body;   // own cells with 2 own neighbours: no further own cell may ever touch them
    Bitboard ends;   // own cells with at most 1 own neighbour
};

inline SnakeShape snakeShapeOf(Bitboard own) {
    SnakeShape s;
    s.own = own;
    // neighbourInDirection[d]: cells whose neighbour in direction d is an own cell
    Bitboard neighbourInDirection[6];
    for (int d = 0; d < 6; d++) neighbourInDirection[d] = cellsWithOffsetIn(own, DIR_Q[d], DIR_R[d]);
    Bitboard atLeast1 = 0, atLeast2 = 0, atLeast3 = 0, adjacentPair = 0;
    for (int d = 0; d < 6; d++) {
        atLeast3 |= atLeast2 & neighbourInDirection[d];
        atLeast2 |= atLeast1 & neighbourInDirection[d];
        atLeast1 |= neighbourInDirection[d];
        // directions d and d+1 point to cells that touch each other
        adjacentPair |= neighbourInDirection[d] & neighbourInDirection[(d + 1) % 6];
    }
    s.touches1 = atLeast1;
    s.touches2 = atLeast2;
    s.touches3 = atLeast3;
    s.touchesAdjacentPair = adjacentPair;
    s.body = own & atLeast2;
    s.ends = own & ~atLeast2;
    return s;
}

// ---- reference rule check (one placement at a time) ------------------------------------------

// Whether `player` may place p, whether or not it is that player's turn (the solver asks what the
// opponent could play next).
inline bool isLegalPlacementFor(const Position& pos, int player, const Placement& p) {
    Bitboard occupied = pos.occupied();
    if (p.cells & occupied) return false;
    Bitboard own = pos.cells[player];
    bool touchesOwn = p.border & own;
    if (!touchesOwn && (p.border & occupied)) return false;  // a new snake must not touch anything
    Bitboard ownAfter = own | p.cells;
    // Each piece cell: at most two own neighbours, and two only if they do not touch each other.
    // (Any new triangle contains a piece cell, so this also catches triangles with old cells.)
    for (int j = 0; j < p.cellCount; j++) {
        Bitboard around = geometry.neighbours[p.cellList[j]] & ownAfter;
        int degree = countCells(around);
        if (degree > 2) return false;
        if (degree == 2 && (geometry.neighbours[firstCell(around)] & withoutFirstCell(around))) return false;
    }
    // Each own cell next to the piece: still at most two own neighbours.
    Bitboard touchedOwn = p.border & own;
    while (touchedOwn) {
        int i = firstCell(touchedOwn);
        touchedOwn = withoutFirstCell(touchedOwn);
        if (countCells(geometry.neighbours[i] & ownAfter) > 2) return false;
    }
    return true;
}

inline bool isLegalPlacement(const Position& pos, const Placement& p) {
    return isLegalPlacementFor(pos, pos.sideToMove, p);
}

// ---- fast move generator ---------------------------------------------------------------------

// Everything the generator needs to know about the position, computed once per position.
struct MoveGenContext {
    Bitboard freeForNewSnake;   // empty cells that touch no piece
    // allowed[k]: empty cells where a piece cell with k neighbours inside its own piece may go.
    // Such a cell can take 2 - k own neighbours from outside the piece.
    Bitboard allowed[3];
    Bitboard emptyNextToOwn;
    Bitboard own;
    Bitboard ownWithNeighbour;  // own cells that already have an own neighbour
};

inline MoveGenContext moveGenContext(const Position& pos) {
    MoveGenContext g;
    Bitboard empty = pos.emptyCells();
    SnakeShape shape = snakeShapeOf(pos.ownCells());
    g.freeForNewSnake = empty & ~neighboursOf(pos.occupied());
    Bitboard notNextToBody = empty & ~neighboursOf(shape.body);
    g.allowed[0] = notNextToBody & ~shape.touches3 & ~shape.touchesAdjacentPair;
    g.allowed[1] = notNextToBody & ~shape.touches2;
    g.allowed[2] = notNextToBody & ~shape.touches1;
    g.emptyNextToOwn = empty & shape.touches1;
    g.own = shape.own;
    g.ownWithNeighbour = shape.own & shape.touches1;
    return g;
}

// The anchor cells where orientation `o` grows an own snake, as one bitboard. Each line tests one
// piece cell for every anchor at once: "the cell at anchor + offset satisfies X" is
// cellsWithOffsetIn(X, offset).
inline Bitboard growingAnchors(const Orientation& o, const MoveGenContext& g) {
    Bitboard grow = geometry.allCells, touchesOwn = 0;
    for (int k = 0; k < o.size; k++) {
        grow &= cellsWithOffsetIn(g.allowed[o.innerNeighbours[k]], o.dq[k], o.dr[k]);
        touchesOwn |= cellsWithOffsetIn(g.emptyNextToOwn, o.dq[k], o.dr[k]);
    }
    grow &= touchesOwn;
    if (grow) {
        // own cells the piece would wrap around: triangle, or a third neighbour for that cell
        for (auto [q, r] : o.wedgeCells) grow &= ~cellsWithOffsetIn(g.own, q, r);
        for (auto [q, r] : o.bridgeCells) grow &= ~cellsWithOffsetIn(g.ownWithNeighbour, q, r);
    }
    return grow;
}

// All anchor cells where orientation `o` may be placed: a new snake, or a grown one.
inline Bitboard legalAnchors(const Orientation& o, const MoveGenContext& g) {
    Bitboard newSnake = geometry.allCells;  // every piece cell touches nothing
    for (int k = 0; k < o.size; k++) newSnake &= cellsWithOffsetIn(g.freeForNewSnake, o.dq[k], o.dr[k]);
    if (!g.emptyNextToOwn) return newSnake;
    return newSnake | growingAnchors(o, g);
}

// Which legal placements forEachLegalMoveIn() lists.
enum class MoveFilter { ALL, COVERING, GROWING };

// Calls f(move) for every legal placement (not for a pass) in placement-id order; with COVERING,
// only for the placements that cover at least one cell of `cover`; with GROWING, only for those
// that grow an own snake.
template <MoveFilter Filter, class F>
inline void forEachLegalMoveIn(const Position& pos, Bitboard cover, F&& f) {
    PieceSet allowed = pos.piecesAllowedNow();
    if (!allowed) return;
    MoveGenContext g = moveGenContext(pos);
    if (Filter == MoveFilter::GROWING && !g.emptyNextToOwn) return;
    for (int piece = 0; piece < NUM_PIECES; piece++) {
        if (!hasPiece(allowed, piece)) continue;
        for (int o = pieceTables.firstOrientation[piece]; o < pieceTables.firstOrientation[piece + 1]; o++) {
            const Orientation& orientation = pieceTables.orientations[o];
            Bitboard anchors = Filter == MoveFilter::GROWING ? growingAnchors(orientation, g) : legalAnchors(orientation, g);
            if constexpr (Filter == MoveFilter::COVERING) {
                // the anchors at which some cell of the piece lands on `cover`
                Bitboard covering = 0;
                if (anchors)
                    for (int k = 0; k < orientation.size; k++) covering |= cellsWithOffsetIn(cover, orientation.dq[k], orientation.dr[k]);
                anchors &= covering;
            }
            forEachCell(anchors, [&](int anchor) { f(Move{orientation.placementAt[anchor]}); });
        }
    }
}

// Calls f(move) for every legal placement (not for a pass), in placement-id order.
template <class F>
inline void forEachLegalMove(const Position& pos, F&& f) {
    forEachLegalMoveIn<MoveFilter::ALL>(pos, 0, f);
}

// The same for only the legal placements that cover at least one cell of `cover`.
template <class F>
inline void forEachLegalMoveCovering(const Position& pos, Bitboard cover, F&& f) {
    forEachLegalMoveIn<MoveFilter::COVERING>(pos, cover, f);
}

// The same for only the legal placements that grow an own snake.
template <class F>
inline void forEachGrowingMove(const Position& pos, F&& f) {
    forEachLegalMoveIn<MoveFilter::GROWING>(pos, 0, f);
}

// All legal placements. Empty means the side to move has to pass.
inline void generateMoves(const Position& pos, std::vector<Move>& out) {
    out.clear();
    forEachLegalMove(pos, [&](Move m) { out.push_back(m); });
}

inline bool hasLegalMove(const Position& pos) {
    PieceSet allowed = pos.piecesAllowedNow();
    if (!allowed) return false;
    MoveGenContext g = moveGenContext(pos);
    for (int piece = 0; piece < NUM_PIECES; piece++) {
        if (!hasPiece(allowed, piece)) continue;
        for (int o = pieceTables.firstOrientation[piece]; o < pieceTables.firstOrientation[piece + 1]; o++)
            if (legalAnchors(pieceTables.orientations[o], g)) return true;
    }
    return false;
}

// Legal in this position, including "pass" when (and only when) nothing else is possible.
inline bool isLegalMove(const Position& pos, Move m) {
    if (m.isPass()) return !hasLegalMove(pos);
    if (!hasPiece(pos.piecesAllowedNow(), m.piece())) return false;
    return isLegalPlacement(pos, m.placement());
}

// The legal moves with symmetric duplicates removed (moves leading to positions with the same
// canonical key); a pass when nothing else is legal.
inline std::vector<Move> symmetryUniqueMoves(const Position& pos) {
    std::vector<Move> all, unique;
    generateMoves(pos, all);
    if (all.empty()) return {Move::pass()};
    std::vector<uint64_t> seen;
    for (Move m : all) {
        Position child = pos;
        child.play(m);
        int symmetry;
        uint64_t key = child.canonicalKey(symmetry);
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            unique.push_back(m);
        }
    }
    return unique;
}
