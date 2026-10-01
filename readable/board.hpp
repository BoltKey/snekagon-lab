// Board geometry and bitboards.
//
// The board is a hexagon with radius 5 (91 cells). A cell has axial coordinates (q, r) with
// |q| <= 5, |r| <= 5 and |q + r| <= 5. The cells are laid out in an 11 x 11 square grid,
//
//     index = (r + 5) * 11 + (q + 5)          (0..120; 91 of these indices are on the board)
//
// so any set of cells fits into one 128-bit integer, a "bitboard". In this layout the six
// neighbours of a cell are at index offsets +1, -1, +11, -11, -10 and +10, which turns
// "all neighbours of a set of cells" into six shifts instead of a loop over cells.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>

using Bitboard = unsigned __int128;

constexpr int BOARD_RADIUS = 5;
constexpr int GRID_WIDTH = 2 * BOARD_RADIUS + 1;    // 11
constexpr int GRID_SIZE = GRID_WIDTH * GRID_WIDTH;  // 121 cell indices
constexpr int BOARD_CELLS = 91;                     // indices that are on the board
constexpr int NUM_SYMMETRIES = 12;                  // 6 rotations, each optionally mirrored

// The six neighbour directions in ring order: directions d and d+1 lead to two cells that are
// neighbours of each other as well.
constexpr int DIR_Q[6] = {1, 1, 0, -1, -1, 0};
constexpr int DIR_R[6] = {0, -1, -1, 0, 1, 1};

// ---- coordinates ---------------------------------------------------------------------------

inline int cellIndex(int q, int r) { return (r + BOARD_RADIUS) * GRID_WIDTH + (q + BOARD_RADIUS); }
inline int cellQ(int index) { return index % GRID_WIDTH - BOARD_RADIUS; }
inline int cellR(int index) { return index / GRID_WIDTH - BOARD_RADIUS; }

inline bool isOnBoard(int q, int r) {
    return std::abs(q) <= BOARD_RADIUS && std::abs(r) <= BOARD_RADIUS && std::abs(q + r) <= BOARD_RADIUS;
}

inline bool areAdjacent(int q1, int r1, int q2, int r2) {
    for (int d = 0; d < 6; d++)
        if (q1 + DIR_Q[d] == q2 && r1 + DIR_R[d] == r2) return true;
    return false;
}

// ---- single cells in a bitboard --------------------------------------------------------------

inline Bitboard cellBit(int index) { return (Bitboard)1 << index; }
inline bool hasCell(Bitboard b, int index) { return (b >> index) & 1; }

inline int countCells(Bitboard b) {
    return __builtin_popcountll((uint64_t)b) + __builtin_popcountll((uint64_t)(b >> 64));
}

// Lowest cell index in b (b must not be empty).
inline int firstCell(Bitboard b) {
    uint64_t low = (uint64_t)b;
    return low ? __builtin_ctzll(low) : 64 + __builtin_ctzll((uint64_t)(b >> 64));
}

inline Bitboard withoutFirstCell(Bitboard b) { return b & (b - 1); }

// Calls f(index) for every cell of b, in ascending index order.
template <class F>
inline void forEachCell(Bitboard b, F&& f) {
    while (b) {
        f(firstCell(b));
        b = withoutFirstCell(b);
    }
}

// ---- precomputed board tables ----------------------------------------------------------------

struct BoardGeometry {
    Bitboard allCells = 0;           // the 91 cells of the board
    Bitboard notEastEdge = 0;        // cells with q < 5: a step in +q does not wrap into the next row
    Bitboard notWestEdge = 0;        // cells with q > -5
    Bitboard columnWindow[17];       // [dq + 8]: cells whose q lies in [-5 + dq, 5 + dq]
    Bitboard neighbours[GRID_SIZE];  // the neighbours of a single cell
    int boardCells[BOARD_CELLS];     // indices of the board cells, ascending
    int symmetric[NUM_SYMMETRIES][GRID_SIZE];  // cell index -> cell index after symmetry t
    int inverseSymmetry[NUM_SYMMETRIES];       // t -> the symmetry that undoes t
};
inline BoardGeometry geometry;

// ---- operations on sets of cells -------------------------------------------------------------

// All cells adjacent to at least one cell of b. (A cell of b is included as well when it
// touches another cell of b.)
inline Bitboard neighboursOf(Bitboard b) {
    Bitboard canStepEast = b & geometry.notEastEdge;  // q+1 stays in the same grid row
    Bitboard canStepWest = b & geometry.notWestEdge;  // q-1 stays in the same grid row
    return ((canStepEast << 1) |    // (q+1, r)
            (canStepWest >> 1) |    // (q-1, r)
            (b << 11) |             // (q,   r+1)
            (b >> 11) |             // (q,   r-1)
            (canStepEast >> 10) |   // (q+1, r-1)
            (canStepWest << 10)) &  // (q-1, r+1)
           geometry.allCells;
}

// The cells x for which x + (dq, dr) is in b. The move generator uses this to test one cell of
// a piece for all possible anchor cells at once.
inline Bitboard cellsWithOffsetIn(Bitboard b, int dq, int dr) {
    // only cells of b whose q minus dq is still on the board can be reached without the shift
    // wrapping around into a neighbouring grid row
    Bitboard source = b & geometry.columnWindow[dq + 8];
    int shift = dr * GRID_WIDTH + dq;
    return (shift >= 0 ? (source >> shift) : (source << -shift)) & geometry.allCells;
}

// `seed` plus all cells of `allowed` connected to it.
inline Bitboard floodFill(Bitboard seed, Bitboard allowed) {
    Bitboard region = seed, previous = 0;
    while (region != previous) {
        previous = region;
        region |= neighboursOf(region) & allowed;
    }
    return region;
}

// Calls f(region) for every connected region of `cells`, ordered by their lowest cell.
template <class F>
inline void forEachRegion(Bitboard cells, F&& f) {
    while (cells) {
        Bitboard region = floodFill(cellBit(firstCell(cells)), cells);
        cells &= ~region;
        f(region);
    }
}

inline int largestRegionSize(Bitboard cells) {
    int largest = 0;
    forEachRegion(cells, [&](Bitboard region) { largest = std::max(largest, countCells(region)); });
    return largest;
}

// ---- symmetries ------------------------------------------------------------------------------

// Symmetry t (0..11): mirror (swap q and r) when t >= 6, then rotate by t % 6 times 60 degrees.
inline std::pair<int, int> applySymmetry(int t, int q, int r) {
    if (t >= 6) std::swap(q, r);
    for (int k = 0; k < t % 6; k++) {
        int rotatedQ = -r, rotatedR = q + r;
        q = rotatedQ;
        r = rotatedR;
    }
    return {q, r};
}

inline void initBoardGeometry() {
    BoardGeometry& g = geometry;
    int count = 0;
    for (int i = 0; i < GRID_SIZE; i++) {
        int q = cellQ(i), r = cellR(i);
        if (!isOnBoard(q, r)) continue;
        g.allCells |= cellBit(i);
        g.boardCells[count++] = i;
        if (q < BOARD_RADIUS) g.notEastEdge |= cellBit(i);
        if (q > -BOARD_RADIUS) g.notWestEdge |= cellBit(i);
    }
    for (int dq = -8; dq <= 8; dq++) {
        Bitboard window = 0;
        for (int i = 0; i < GRID_SIZE; i++)
            if (cellQ(i) >= -BOARD_RADIUS + dq && cellQ(i) <= BOARD_RADIUS + dq) window |= cellBit(i);
        g.columnWindow[dq + 8] = window;
    }
    for (int i = 0; i < GRID_SIZE; i++)
        g.neighbours[i] = hasCell(g.allCells, i) ? neighboursOf(cellBit(i)) : 0;

    for (int t = 0; t < NUM_SYMMETRIES; t++)
        for (int i = 0; i < GRID_SIZE; i++) {
            if (!hasCell(g.allCells, i)) { g.symmetric[t][i] = i; continue; }
            auto [q, r] = applySymmetry(t, cellQ(i), cellR(i));
            g.symmetric[t][i] = cellIndex(q, r);
        }
    for (int t = 0; t < NUM_SYMMETRIES; t++)
        for (int u = 0; u < NUM_SYMMETRIES; u++) {
            bool undoesT = true;
            for (int c : g.boardCells)
                if (g.symmetric[u][g.symmetric[t][c]] != c) { undoesT = false; break; }
            if (undoesT) { g.inverseSymmetry[t] = u; break; }
        }
}
