// Correctness checks and micro-benchmarks: perft, verify, info, speed.
#include <chrono>
#include <cstdio>
#include "bounds.hpp"
#include "commands.hpp"
#include "evaluation.hpp"

// Number of move sequences of length `depth` (a forced pass counts as a move).
static uint64_t perft(const Position& pos, int depth) {
    if (depth == 0 || pos.isOver()) return 1;
    std::vector<Move> moves;
    generateMoves(pos, moves);
    if (moves.empty()) {
        Position child = pos;
        child.play(Move::pass());
        return perft(child, depth - 1);
    }
    if (depth == 1) return moves.size();
    uint64_t count = 0;
    for (Move m : moves) {
        Position child = pos;
        child.play(m);
        count += perft(child, depth - 1);
    }
    return count;
}

// snek perft [depth]: counts for depth 1..depth; compare with ref/snek_ref.py.
int cmdPerft(const Args& args) {
    int maxDepth = args.arg(2, 3);
    Position start = Position::initial();
    for (int depth = 1; depth <= maxDepth; depth++) {
        auto t0 = std::chrono::steady_clock::now();
        uint64_t count = perft(start, depth);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("perft %d = %llu  (%.2fs)\n", depth, (unsigned long long)count, seconds);
        fflush(stdout);
    }
    return 0;
}

// The reference move list: every placement checked one by one with the plain rule check.
static void referenceMoves(const Position& pos, std::vector<Move>& out) {
    out.clear();
    PieceSet allowed = pos.piecesAllowedNow();
    for (int piece = 0; piece < NUM_PIECES; piece++)
        if (hasPiece(allowed, piece))
            for (const Placement& p : placementsOf(piece))
                if (isLegalPlacement(pos, p)) out.push_back(Move::place(p));
}

// Checks of one position; prints the problem and returns false on a mismatch.
static bool verifyPosition(const Position& pos, const std::vector<Move>& fast, int game) {
    std::vector<Move> slow;
    referenceMoves(pos, slow);
    if (fast.size() != slow.size() || !std::equal(fast.begin(), fast.end(), slow.begin())) {
        printf("MOVEGEN MISMATCH game %d ply %d: fast %zu slow %zu\n", game, (int)pos.ply, fast.size(), slow.size());
        return false;
    }
    if (hasLegalMove(pos) != !fast.empty()) { printf("HASMOVE MISMATCH\n"); return false; }
    for (int c = 0; c < 2; c++) {
        // incrementally tracked snakes == recomputed ones; fast bounds == reference bounds
        if (pos.longestSnake[c] != longestSnakeIn(pos.cells[c])) { printf("LONGEST MISMATCH\n"); return false; }
        Bitboard allSnakes = 0;
        for (int j = 0; j < pos.snakeCount[c]; j++) allSnakes |= pos.snakes[c][j];
        if (allSnakes != pos.cells[c]) { printf("COMPONENT MISMATCH\n"); return false; }
        SnakeBounds fastBounds = snakeBounds(pos, c), slowBounds = snakeBoundsReference(pos, c);
        if (fastBounds.lower != slowBounds.lower || fastBounds.upper != slowBounds.upper) {
            printf("BOUNDS MISMATCH game %d ply %d: [%d,%d] vs ref [%d,%d]\n", game, (int)pos.ply, fastBounds.lower,
                   fastBounds.upper, slowBounds.lower, slowBounds.upper);
            return false;
        }
    }
    return true;
}

// The one-player optimum of `player` (the longest snake OnePlayerOptimum allows), asked of the
// given clusters, or -1 if a question ran out of budget.
static int onePlayerOptimum(OnePlayerOptimum& optimum, const Position& pos, int player, const SnakeClusters& clusters, int upper) {
    for (int target = upper; target > pos.longestSnake[player]; target--) {
        uint64_t outOfBudget = optimum.stats.outOfBudget;
        bool may = optimum.mayReach(pos, player, clusters, target);
        if (optimum.stats.outOfBudget != outOfBudget) return -1;
        if (may) return target;
    }
    return pos.longestSnake[player];
}

// The longest final snake of `player` under the real rules if the opponent never moved again (it
// passes), by brute force; -1 when `budget` positions are used up.
static int oneHandedBest(const Position& pos, int player, uint64_t& budget) {
    if (budget == 0) return -1;
    budget--;
    int best = pos.longestSnake[player];
    Position mine = pos;
    mine.sideToMove = player;
    if (!mine.piecesLeft[player]) return best;
    std::vector<Move> moves;
    generateMoves(mine, moves);
    for (Move m : moves) {
        Position next = mine;
        next.play(m);
        int length = oneHandedBest(next, player, budget);
        if (length < 0) return -1;
        best = std::max(best, length);
    }
    return best;
}

// snek verify [games]: plays random games and checks in every position that
//  - the fast move generator returns exactly the reference move list (same order),
//  - the incremental snakes and the fast bounds match their slow versions,
// and at the end of every game that the final snake lengths lie within all earlier bounds, and,
// where the player has at most 4 pieces left, that its one-player optimum lies between its final
// snake and its bound, is the same asked cluster by cluster as over the whole board, and (at most
// 3 pieces) is at least the real-rules optimum when the opponent never moves again.
int cmdVerify(const Args& args) {
    int games = args.arg(2, 2000);
    Rng rng(99);
    long positions = 0, boundChecks = 0;
    constexpr int OPTIMUM_MAX_PIECES = 4, BRUTE_FORCE_MAX_PIECES = 3;
    constexpr uint64_t BRUTE_FORCE_BUDGET = 300000;
    OnePlayerOptimum optimum;
    long optimumChecks = 0, optimumTighter = 0, optimumUnknown = 0, bruteCompared = 0, bruteEqual = 0, bruteUnknown = 0;
    std::vector<Move> moves;
    for (int game = 0; game < games; game++) {
        Position pos = Position::initial();
        std::vector<Position> visited;
        while (!pos.isOver()) {
            generateMoves(pos, moves);
            if (!verifyPosition(pos, moves, game)) return 1;
            positions++;
            visited.push_back(pos);
            // mix uniformly random moves and "grow an own snake" moves, so snakes get long
            Move m = Move::pass();
            if (!moves.empty()) {
                std::vector<Move> growing;
                for (Move x : moves)
                    if (x.placement().border & pos.ownCells()) growing.push_back(x);
                if (!growing.empty() && rng.below(3)) m = growing[rng.below((int)growing.size())];
                else m = moves[rng.below((int)moves.size())];
            }
            pos.play(m);
        }
        int finalLength[2] = {longestSnakeIn(pos.cells[RED]), longestSnakeIn(pos.cells[BLUE])};
        for (const Position& earlier : visited)
            for (int c = 0; c < 2; c++) {
                SnakeBounds b = snakeBounds(earlier, c);
                boundChecks++;
                if (finalLength[c] < b.lower || finalLength[c] > b.upper) {
                    printf("BOUND VIOLATION game %d ply %d player %d: final %d, bounds [%d,%d]\n", game, (int)earlier.ply, c,
                           finalLength[c], b.lower, b.upper);
                    return 1;
                }
                if (earlier.ply == 0 || pieceCount(earlier.piecesLeft[c]) > OPTIMUM_MAX_PIECES) continue;
                SnakeClusters clusters, wholeBoard;
                snakeBounds(earlier, c, &clusters);
                wholeBoard.count = 1;  // one region over the whole board: the clusters must not change the answer
                wholeBoard.cells[0] = earlier.cells[c] | growableCells(earlier, c);
                wholeBoard.upper[0] = GRID_SIZE;
                int best = onePlayerOptimum(optimum, earlier, c, clusters, b.upper);
                int bestWhole = onePlayerOptimum(optimum, earlier, c, wholeBoard, GRID_SIZE);
                if (best < 0 || bestWhole < 0) { optimumUnknown++; continue; }
                optimumChecks++;
                if (best < b.upper) optimumTighter++;
                if (finalLength[c] > best || best != bestWhole) {
                    printf("ONE-PLAYER VIOLATION game %d ply %d player %d: final %d, optimum %d (whole board %d), cheap bound %d\n", game,
                           (int)earlier.ply, c, finalLength[c], best, bestWhole, b.upper);
                    return 1;
                }
                if (pieceCount(earlier.piecesLeft[c]) > BRUTE_FORCE_MAX_PIECES) continue;
                uint64_t budget = BRUTE_FORCE_BUDGET;
                int real = oneHandedBest(earlier, c, budget);
                if (real < 0) { bruteUnknown++; continue; }
                bruteCompared++;
                if (real == best) bruteEqual++;
                if (real > best) {
                    printf("ONE-PLAYER VIOLATION game %d ply %d player %d: real-rules optimum %d > relaxation %d\n", game, (int)earlier.ply, c,
                           real, best);
                    return 1;
                }
            }
    }
    printf("ok: %ld positions movegen identical, %ld bound checks hold\n", positions, boundChecks);
    printf("ok: one-player optimum in %ld checks (%ld unknown): final <= optimum <= cheap bound (strictly below it in %ld), "
           "per cluster == whole board; >= real-rules brute force in %ld (equal %ld, %ld over budget)\n",
           optimumChecks, optimumUnknown, optimumTighter, bruteCompared, bruteEqual, bruteUnknown);
    return 0;
}

int cmdInfo(const Args&) {
    for (int p = 0; p < NUM_PIECES; p++) printf("%s: %zu placements\n", PIECE_NAME[p], placementsOf(p).size());
    printf("total placements %d\n", (int)pieceTables.placements.size());
    return 0;
}

// snek speed: time per call of the hot functions, on all positions of 300 random games.
int cmdSpeed(const Args&) {
    Rng rng(5);
    std::vector<Position> positions;
    std::vector<Move> buffer;
    for (int game = 0; game < 300; game++) {
        Position pos = Position::initial();
        while (!pos.isOver()) {
            positions.push_back(pos);
            generateMoves(pos, buffer);
            pos.play(buffer.empty() ? Move::pass() : buffer[rng.below((int)buffer.size())]);
        }
    }
    auto bench = [&](const char* name, auto&& function) {
        auto t0 = std::chrono::steady_clock::now();
        long sink = 0;  // keeps the compiler from optimising the calls away
        int repetitions = 20;
        for (int r = 0; r < repetitions; r++)
            for (auto& p : positions) sink += function(p);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("%-14s %8.3f us/call   (sink %ld)\n", name, seconds / (repetitions * positions.size()) * 1e6, sink);
    };
    bench("genMoves", [&](const Position& p) { generateMoves(p, buffer); return (long)buffer.size(); });
    bench("hasMove", [&](const Position& p) { return (long)hasLegalMove(p); });
    bench("snakeBounds", [&](const Position& p) { return (long)snakeBounds(p, 0).upper; });
    bench("evalHand", [&](const Position& p) { return (long)evaluateHandMade(p); });
    bench("evalLearned", [&](const Position& p) { return (long)evaluate(p); });
    return 0;
}
