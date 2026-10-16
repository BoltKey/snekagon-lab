// The eight pieces, their orientations, and every way to place them on the board.
//
// A "placement" is a piece put on concrete cells. All placements of all pieces are numbered
// once at start-up (piece by piece, orientation by orientation, anchor cells ascending); a move
// is simply such a placement id. The fast move generator enumerates placements in exactly this
// order, which keeps move lists (and therefore search results) reproducible.
#pragma once
#include <array>
#include <string>
#include <vector>
#include "board.hpp"

enum Piece { PIECE_1, PIECE_2, PIECE_3I, PIECE_3V, PIECE_4I, PIECE_4A, PIECE_4B, PIECE_4C, NUM_PIECES };

inline const char* const PIECE_NAME[NUM_PIECES] = {"1", "2", "3I", "3V", "4I", "4A", "4B", "4C"};
inline const int PIECE_SIZE[NUM_PIECES] = {1, 2, 3, 3, 4, 4, 4, 4};

// Cell offsets (q, r) of each piece, read off the print & play sheet. Pieces are double-sided,
// so mirrored orientations are allowed too.
inline const std::vector<std::pair<int, int>> PIECE_SHAPE[NUM_PIECES] = {
    {{0, 0}},                            // 1
    {{0, 0}, {1, 0}},                    // 2
    {{0, 0}, {0, 1}, {0, 2}},            // 3I  straight
    {{0, 0}, {1, 0}, {1, 1}},            // 3V  bent
    {{0, 0}, {-1, 1}, {-2, 2}, {-3, 3}}, // 4I  straight
    {{0, 0}, {1, 0}, {1, 1}, {1, 2}},    // 4A
    {{0, 0}, {0, 1}, {1, 1}, {1, 2}},    // 4B
    {{0, 0}, {-1, 1}, {-1, 2}, {0, 2}},  // 4C
};

// A set of pieces as a bit mask: bit p is set when piece p is in the set.
using PieceSet = uint8_t;
constexpr PieceSet ALL_PIECES = 0xFF;          // red starts with all eight pieces ...
constexpr PieceSet ALL_BUT_SINGLE_HEX = 0xFE;  // ... blue never gets the 1-hex

inline bool hasPiece(PieceSet set, int piece) { return (set >> piece) & 1; }
inline int pieceCount(PieceSet set) { return __builtin_popcount(set); }

inline int cellsInPieces(PieceSet set) {
    int cells = 0;
    for (int p = 0; p < NUM_PIECES; p++)
        if (hasPiece(set, p)) cells += PIECE_SIZE[p];
    return cells;
}

struct Placement {
    int id;                // position in pieceTables.placements
    int piece;
    Bitboard cells;        // the cells covered
    Bitboard border;       // the cells adjacent to the piece
    uint8_t cellList[4];   // the covered cells as indices
    uint8_t cellCount;
};

// One orientation of a piece, prepared for the bit-parallel move generator (see rules.hpp).
// The offsets are relative to the "anchor", the first cell in (r, q) order.
struct Orientation {
    int piece;
    int size;
    int dq[4], dr[4];
    int innerNeighbours[4];  // for each piece cell: how many other cells of the piece it touches
    // Outside cells touching two piece cells that touch each other (or three or more piece
    // cells): an own cell there would close a triangle or get too many neighbours.
    std::vector<std::pair<int, int>> wedgeCells;
    // Outside cells touching exactly two piece cells that do not touch each other: an own cell
    // there is fine only if it has no own neighbour yet (it becomes a snake body cell).
    std::vector<std::pair<int, int>> bridgeCells;
    int16_t placementAt[GRID_SIZE];  // anchor cell -> placement id (-1: piece would leave the board)
    // cell -> the anchors where the piece covers a neighbour of that cell but not the cell itself
    // (how the one-player optimum extends a snake end, bounds.hpp)
    Bitboard anchorsNextTo[GRID_SIZE];
};

struct PieceTables {
    std::vector<Placement> placements;          // all placements, grouped by piece
    int firstPlacement[NUM_PIECES + 1];         // placements of piece p: [first[p], first[p+1])
    std::vector<Orientation> orientations;      // all orientations, grouped by piece
    int firstOrientation[NUM_PIECES + 1];
    std::vector<std::array<int, NUM_SYMMETRIES>> symmetricPlacement;  // id, t -> id after symmetry t
    // bestFill[pieces][n]: the most cells the given pieces can cover using at most n cells
    // (best subset sum of the piece sizes). Used by the snake-length bounds and the evaluation.
    uint8_t bestFill[256][GRID_SIZE + 1];
};
inline PieceTables pieceTables;

struct PlacementRange {
    const Placement* first;
    const Placement* last;
    const Placement* begin() const { return first; }
    const Placement* end() const { return last; }
    size_t size() const { return last - first; }
    const Placement& operator[](size_t i) const { return first[i]; }
};
inline PlacementRange placementsOf(int piece) {
    const Placement* all = pieceTables.placements.data();
    return {all + pieceTables.firstPlacement[piece], all + pieceTables.firstPlacement[piece + 1]};
}

// A move: a placement id, or -1 for a pass.
struct Move {
    int16_t placementId = -1;

    static Move pass() { return Move{}; }
    static Move place(const Placement& p) { return Move{(int16_t)p.id}; }
    bool isPass() const { return placementId < 0; }
    const Placement& placement() const { return pieceTables.placements[placementId]; }
    int piece() const { return placement().piece; }
    bool operator==(const Move& other) const { return placementId == other.placementId; }
};

// ---- building the tables ---------------------------------------------------------------------

using CellOffsets = std::vector<std::pair<int, int>>;

// The distinct orientations of a piece: its cells after each of the 12 symmetries, sorted by
// (r, q) and shifted so that the first cell is at (0, 0). Kept in the order they are found.
inline std::vector<CellOffsets> distinctOrientations(int piece) {
    std::vector<CellOffsets> result;
    for (int t = 0; t < NUM_SYMMETRIES; t++) {
        CellOffsets cells;
        for (auto [q, r] : PIECE_SHAPE[piece]) cells.push_back(applySymmetry(t, q, r));
        std::sort(cells.begin(), cells.end(), [](auto a, auto b) {
            return a.second != b.second ? a.second < b.second : a.first < b.first;
        });
        auto anchor = cells[0];
        for (auto& c : cells) { c.first -= anchor.first; c.second -= anchor.second; }
        if (std::find(result.begin(), result.end(), cells) == result.end()) result.push_back(cells);
    }
    return result;
}

inline bool containsOffset(const CellOffsets& cells, std::pair<int, int> c) {
    return std::find(cells.begin(), cells.end(), c) != cells.end();
}

inline Orientation describeOrientation(int piece, const CellOffsets& cells) {
    Orientation o{};
    o.piece = piece;
    o.size = (int)cells.size();
    for (int j = 0; j < o.size; j++) {
        o.dq[j] = cells[j].first;
        o.dr[j] = cells[j].second;
        o.innerNeighbours[j] = 0;
        for (int l = 0; l < o.size; l++)
            if (l != j && areAdjacent(cells[j].first, cells[j].second, cells[l].first, cells[l].second))
                o.innerNeighbours[j]++;
    }
    // classify the outside cells that touch two or more piece cells
    CellOffsets seen;
    for (int j = 0; j < o.size; j++)
        for (int d = 0; d < 6; d++) {
            std::pair<int, int> outside{cells[j].first + DIR_Q[d], cells[j].second + DIR_R[d]};
            if (containsOffset(cells, outside) || containsOffset(seen, outside)) continue;
            seen.push_back(outside);
            std::vector<int> touching;
            for (int l = 0; l < o.size; l++)
                if (areAdjacent(outside.first, outside.second, cells[l].first, cells[l].second)) touching.push_back(l);
            if (touching.size() < 2) continue;
            bool wedge = touching.size() >= 3;
            for (size_t x = 0; x < touching.size(); x++)
                for (size_t y = x + 1; y < touching.size(); y++) {
                    auto a = cells[touching[x]], b = cells[touching[y]];
                    if (areAdjacent(a.first, a.second, b.first, b.second)) wedge = true;
                }
            (wedge ? o.wedgeCells : o.bridgeCells).push_back(outside);
        }
    for (int i = 0; i < GRID_SIZE; i++) {
        o.placementAt[i] = -1;
        o.anchorsNextTo[i] = 0;
    }
    return o;
}

// Adds every on-board placement of orientation `o` (anchor cells ascending).
inline void addPlacements(Orientation& o, const CellOffsets& cells) {
    for (int anchor : geometry.boardCells) {
        Placement p{};
        bool fits = true;
        for (auto [dq, dr] : cells) {
            int q = cellQ(anchor) + dq, r = cellR(anchor) + dr;
            if (!isOnBoard(q, r)) { fits = false; break; }
            p.cellList[p.cellCount++] = (uint8_t)cellIndex(q, r);
            p.cells |= cellBit(cellIndex(q, r));
        }
        if (!fits) continue;
        p.border = neighboursOf(p.cells) & ~p.cells;
        p.piece = o.piece;
        p.id = (int)pieceTables.placements.size();
        o.placementAt[anchor] = (int16_t)p.id;
        forEachCell(p.border, [&](int cell) { o.anchorsNextTo[cell] |= cellBit(anchor); });
        pieceTables.placements.push_back(p);
    }
}

inline void initPieceTables() {
    PieceTables& pt = pieceTables;
    pt.placements.clear();
    pt.orientations.clear();
    for (int piece = 0; piece < NUM_PIECES; piece++) {
        pt.firstOrientation[piece] = (int)pt.orientations.size();
        pt.firstPlacement[piece] = (int)pt.placements.size();
        for (const CellOffsets& cells : distinctOrientations(piece)) {
            Orientation o = describeOrientation(piece, cells);
            addPlacements(o, cells);
            pt.orientations.push_back(o);
        }
    }
    pt.firstOrientation[NUM_PIECES] = (int)pt.orientations.size();
    pt.firstPlacement[NUM_PIECES] = (int)pt.placements.size();

    // where does each placement go under each symmetry?
    pt.symmetricPlacement.assign(pt.placements.size(), {});
    for (const Placement& p : pt.placements)
        for (int t = 0; t < NUM_SYMMETRIES; t++) {
            Bitboard image = 0;
            for (int j = 0; j < p.cellCount; j++) image |= cellBit(geometry.symmetric[t][p.cellList[j]]);
            int found = -1;
            for (const Placement& other : placementsOf(p.piece))
                if (other.cells == image) { found = other.id; break; }
            pt.symmetricPlacement[p.id][t] = found;
        }

    // bestFill: subset sums of piece sizes for every set of pieces
    for (int set = 0; set < 256; set++) {
        std::vector<bool> reachable(GRID_SIZE + 1, false);
        reachable[0] = true;
        for (int p = 0; p < NUM_PIECES; p++)
            if (hasPiece((PieceSet)set, p))
                for (int sum = GRID_SIZE; sum >= PIECE_SIZE[p]; sum--)
                    if (reachable[sum - PIECE_SIZE[p]]) reachable[sum] = true;
        int best = 0;
        for (int n = 0; n <= GRID_SIZE; n++) {
            if (reachable[n]) best = n;
            pt.bestFill[set][n] = (uint8_t)best;
        }
    }
}
