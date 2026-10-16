// A game position: who owns which cells, which pieces are left, whose turn it is - plus the
// snakes of both players and the hash keys, which play() keeps up to date incrementally.
#pragma once
#include "pieces.hpp"
#include "random.hpp"

constexpr int RED = 0;   // player 1, places the 1-hex first
constexpr int BLUE = 1;  // player 2
constexpr int MAX_SNAKES = 8;  // every placed piece starts at most one new snake

// Up to this ply the position hash is also kept for all 12 mirrored/rotated versions of the
// board, so that symmetric positions near the root share one transposition-table entry. Deeper
// positions are practically never symmetric, so only the plain hash is maintained there.
constexpr int SYMMETRY_PLY = 6;

// ---- Zobrist hashing -------------------------------------------------------------------------
// The hash of a position is the XOR of one random key per occupied cell (per player), one per
// piece still in hand (per player) and one for "blue to move".

struct ZobristKeys {
    uint64_t cell[2][GRID_SIZE];
    uint64_t piece[2][NUM_PIECES];
    uint64_t blueToMove;
};
inline ZobristKeys zobrist;

inline void initZobrist() {
    uint64_t seed = 0x5eca9011ULL;  // fixed: hash keys (and so search results) are reproducible
    for (int player = 0; player < 2; player++) {
        for (int i = 0; i < GRID_SIZE; i++) zobrist.cell[player][i] = splitmix64(seed);
        for (int p = 0; p < NUM_PIECES; p++) zobrist.piece[player][p] = splitmix64(seed);
    }
    zobrist.blueToMove = splitmix64(seed);
}

// ---- the position ----------------------------------------------------------------------------

struct Position {
    Bitboard cells[2] = {0, 0};  // cells of red / blue
    PieceSet piecesLeft[2] = {ALL_PIECES, ALL_BUT_SINGLE_HEX};
    uint8_t sideToMove = RED;
    uint8_t passesInRow = 0;     // two passes in a row end the game
    uint8_t ply = 0;             // moves played so far (passes included)
    // hash[t]: hash of the position after symmetry t; t > 0 only while ply <= SYMMETRY_PLY
    uint64_t hash[NUM_SYMMETRIES] = {};
    // the snakes (connected groups of cells) of each player and the length of the longest one
    Bitboard snakes[2][MAX_SNAKES];
    uint8_t snakeCount[2] = {0, 0};
    uint8_t longestSnake[2] = {0, 0};

    static Position initial() {
        Position pos;
        pos.recomputeDerivedState();
        return pos;
    }

    bool isOver() const { return passesInRow >= 2 || (piecesLeft[RED] == 0 && piecesLeft[BLUE] == 0); }
    Bitboard occupied() const { return cells[RED] | cells[BLUE]; }
    Bitboard emptyCells() const { return geometry.allCells & ~occupied(); }
    Bitboard ownCells() const { return cells[sideToMove]; }
    Bitboard opponentCells() const { return cells[sideToMove ^ 1]; }

    // The pieces the side to move may place now (the very first move must be the 1-hex).
    PieceSet piecesAllowedNow() const { return ply == 0 ? (piecesLeft[sideToMove] & 1) : piecesLeft[sideToMove]; }

    // Hash key used by the transposition tables. Near the root it is the smallest of the 12
    // symmetric hashes, so all symmetric variants of a position share one key; `symmetry`
    // returns the transformation that maps this position onto that canonical variant.
    uint64_t canonicalKey(int& symmetry) const {
        symmetry = 0;
        if (ply > SYMMETRY_PLY) return hash[0];
        for (int t = 1; t < NUM_SYMMETRIES; t++)
            if (hash[t] < hash[symmetry]) symmetry = t;
        return hash[symmetry];
    }

    // 32 more bits that tell positions with the same key apart (the transposition table stores them),
    // from the cells and pieces, independently of the Zobrist hash. Near the root, where the key is
    // symmetry-canonical, it is 0 (those few positions do not need it).
    uint32_t keyCheck() const {
        if (ply <= SYMMETRY_PLY) return 0;
        uint64_t x = (uint64_t)cells[RED] * 0x9E3779B97F4A7C15ULL ^ (uint64_t)(cells[RED] >> 64) * 0xC2B2AE3D27D4EB4FULL ^
                     (uint64_t)cells[BLUE] * 0x165667B19E3779F9ULL ^ (uint64_t)(cells[BLUE] >> 64) * 0x27D4EB2F165667C5ULL ^
                     uint64_t(piecesLeft[RED] | piecesLeft[BLUE] << 8 | sideToMove << 16) * 0x85EBCA77C2B2AE63ULL;
        return uint32_t(x >> 32);
    }

    void play(Move move) {
        int hashesToUpdate = ply < SYMMETRY_PLY ? NUM_SYMMETRIES : 1;
        if (move.isPass()) {
            passesInRow++;
        } else {
            passesInRow = 0;
            placePiece(move.placement(), hashesToUpdate);
        }
        for (int t = 0; t < hashesToUpdate; t++) hash[t] ^= zobrist.blueToMove;
        sideToMove ^= 1;
        ply++;
    }

    // Recomputes snakes and hashes from cells / piecesLeft / sideToMove (after editing those
    // fields directly, e.g. when reading a position from text).
    void recomputeDerivedState() {
        recomputeSnakes();
        recomputeHashes();
    }

  private:
    void placePiece(const Placement& p, int hashesToUpdate) {
        int me = sideToMove;
        cells[me] |= p.cells;
        piecesLeft[me] &= ~(1u << p.piece);
        for (int t = 0; t < hashesToUpdate; t++) {
            uint64_t h = hash[t] ^ zobrist.piece[me][p.piece];
            for (int j = 0; j < p.cellCount; j++) h ^= zobrist.cell[me][geometry.symmetric[t][p.cellList[j]]];
            hash[t] = h;
        }
        joinSnakes(me, p);
    }

    // The new piece and all own snakes it touches become one snake.
    void joinSnakes(int me, const Placement& p) {
        Bitboard joined = p.cells;
        int kept = 0;
        for (int j = 0; j < snakeCount[me]; j++) {
            if (snakes[me][j] & p.border) joined |= snakes[me][j];
            else snakes[me][kept++] = snakes[me][j];
        }
        snakes[me][kept++] = joined;
        snakeCount[me] = (uint8_t)kept;
        longestSnake[me] = std::max<int>(longestSnake[me], countCells(joined));
    }

    void recomputeSnakes() {
        for (int player = 0; player < 2; player++) {
            snakeCount[player] = 0;
            longestSnake[player] = 0;
            forEachRegion(cells[player], [&](Bitboard snake) {
                snakes[player][snakeCount[player]++] = snake;
                longestSnake[player] = std::max<int>(longestSnake[player], countCells(snake));
            });
        }
    }

    void recomputeHashes() {
        for (int t = 0; t < NUM_SYMMETRIES; t++) {
            uint64_t h = 0;
            for (int player = 0; player < 2; player++) {
                forEachCell(cells[player], [&](int i) { h ^= zobrist.cell[player][geometry.symmetric[t][i]]; });
                for (int p = 0; p < NUM_PIECES; p++)
                    if (hasPiece(piecesLeft[player], p)) h ^= zobrist.piece[player][p];
            }
            if (sideToMove == BLUE) h ^= zobrist.blueToMove;
            hash[t] = h;
        }
    }
};

// ---- results ---------------------------------------------------------------------------------

// Longest red snake minus longest blue snake (> 0: red is ahead). The result of a finished game
// as the exact solver counts it is gameResult() in solver.hpp.
inline int redLead(const Position& pos) { return pos.longestSnake[RED] - pos.longestSnake[BLUE]; }

inline void initTables() {
    initBoardGeometry();
    initPieceTables();
    initZobrist();
}
