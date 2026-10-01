// Exact solver: finds the game-theoretic value of a position (win / draw / loss for the side to
// move) with no heuristics in the result - every shortcut it takes is a proof.
//
// It is a negamax alpha-beta search over the values {-1, 0, +1}. What makes it fast:
//   - transposition table (transposition_table.hpp), keyed by the symmetry-canonical hash;
//   - snake-length bounds (bounds.hpp): a position is decided as soon as one player's sure
//     length beats the other's best possible length;
//   - move ordering: table move, killer moves, history, simple shape heuristics and, while many
//     pieces are left, the learned evaluation of the child positions. With only three possible
//     values a good first move usually ends the search of a node immediately.
#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include "evaluation.hpp"
#include "transposition_table.hpp"

class Solver {
  public:
    static constexpr int UNKNOWN = -9;  // solve() result when a time or node limit stopped it

    // Move-ordering settings (environment variables override them for experiments).
    int evalOrderFromPieces = envInt("SOLVER_EVAL_REM", 7);  // order by evaluation with >= this many pieces left (both players)
    // Below that, order by evaluation only where the side to move does not look lost: the losing
    // side has to try every move anyway, so ordering its moves is wasted work.
    int evalOrderIfHopefulFromPieces = envInt("SOLVER_EVAL_WIN_REM", 7);
    double hopefulLogit = envInt("SOLVER_EVAL_WIN_THR", -100) / 100.0;
    int etcFromPieces = envInt("SOLVER_ETC_REM", 99);  // enhanced transposition cutoffs (off by default)

    uint64_t nodes = 0;                        // nodes searched by the last solve()
    std::atomic<bool>* stopFlag = nullptr;     // set from outside to abort the search

    // Per-ply statistics (benchset with PROFILE=1): nodes visited, cut by the table, cut by the
    // bounds, expanded.
    struct Profile {
        uint64_t visited[16] = {}, tableCut[16] = {}, boundCut[16] = {}, expanded[16] = {};
    } profile;

    // log2Entries: table size (16 bytes per entry)
    explicit Solver(int log2Entries = 22) : table(std::make_shared<TranspositionTable>(log2Entries - 1)) { init(); }
    explicit Solver(std::shared_ptr<TranspositionTable> shared) : table(std::move(shared)) { init(); }

    // Forget everything learned (table, history, killers).
    void clear() {
        table->clear();
        for (auto& h : history) std::fill(h.begin(), h.end(), 0);
        std::memset(killers, -1, sizeof killers);
    }

    // Value for the side to move (+1 / 0 / -1), or UNKNOWN when the limits stopped the search.
    // seconds = 0: no time limit.
    int solve(const Position& pos, double seconds = 0, uint64_t maxNodes = UINT64_MAX) {
        aborted = false;
        nodes = 0;
        nodeLimit = maxNodes;
        hasDeadline = seconds > 0;
        if (hasDeadline)
            deadline = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(seconds * 1e6));
        int value = search(pos, -1, 1);
        return aborted ? UNKNOWN : value;
    }

  private:
    static constexpr int MAX_PLY = 32;
    static constexpr int MAX_MOVES = 3000;  // more than the legal moves of any position

    std::shared_ptr<TranspositionTable> table;
    std::vector<int32_t> history[2];  // per side and placement: how often it caused a cutoff (weighted)
    int16_t killers[MAX_PLY][2];      // per ply: the last two moves that caused a cutoff
    std::vector<Move> moveBuffer[MAX_PLY];  // per-ply move lists, allocated once
    std::vector<int> scoreBuffer[MAX_PLY];

    std::chrono::steady_clock::time_point deadline;
    bool hasDeadline = false;
    bool aborted = false;
    uint64_t nodeLimit = UINT64_MAX;

    void init() {
        for (int i = 0; i < MAX_PLY; i++) {
            moveBuffer[i].resize(MAX_MOVES);
            scoreBuffer[i].resize(MAX_MOVES);
        }
        for (auto& h : history) h.assign(pieceTables.placements.size(), 0);
        std::memset(killers, -1, sizeof killers);
    }

    bool shouldAbort() {
        if (aborted) return true;
        if ((nodes & 4095) == 0) {  // checking the clock is not free
            if (stopFlag && stopFlag->load(std::memory_order_relaxed)) aborted = true;
            else if (hasDeadline && std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        if (nodes > nodeLimit) aborted = true;
        return aborted;
    }

    // ---- the search ----

    int search(const Position& pos, int alpha, int beta) {
        nodes++;
        if (shouldAbort()) return 0;
        if (pos.isOver()) return gameResult(pos);
        uint64_t nodesAtStart = nodes;
        int profilePly = std::min<int>(pos.ply, 15);
        profile.visited[profilePly]++;

        // 1. What is already known: the value lies in [lower, upper].
        int lower = -1, upper = 1;
        int symmetry;
        uint64_t key = pos.canonicalKey(symmetry);
        TTEntry stored{-1, 1, -1};
        int tableMove = -1;  // the stored best move, in this position's orientation
        if (table->probe(key, stored)) {
            lower = stored.lower;
            upper = stored.upper;
            if (lower >= upper || lower >= beta) { profile.tableCut[profilePly]++; return lower; }
            if (upper <= alpha) { profile.tableCut[profilePly]++; return upper; }
            if (stored.bestMove >= 0) tableMove = pieceTables.symmetricPlacement[stored.bestMove][geometry.inverseSymmetry[symmetry]];
        }
        narrowByBounds(pos, lower, upper);
        if (lower >= upper || lower >= beta) { profile.boundCut[profilePly]++; return lower; }
        if (upper <= alpha) { profile.boundCut[profilePly]++; return upper; }
        profile.expanded[profilePly]++;
        alpha = std::max(alpha, lower);
        beta = std::min(beta, upper);
        int alphaAtStart = alpha, betaAtStart = beta;

        // 2. Generate and score the moves.
        Move* moves = moveBuffer[pos.ply & 31].data();
        int* scores = scoreBuffer[pos.ply & 31].data();
        int count = scoreMoves(pos, tableMove, moves, scores);
        int piecesLeft = pieceCount(pos.piecesLeft[RED]) + pieceCount(pos.piecesLeft[BLUE]);
        bool orderByEval = count > 1 && (piecesLeft >= evalOrderFromPieces ||
                                         (piecesLeft >= evalOrderIfHopefulFromPieces && learnedLogit(pos) > hopefulLogit));
        bool tryEtc = count > 0 && piecesLeft >= etcFromPieces;
        if (orderByEval || tryEtc) {
            int cutoffValue;
            if (examineChildren(pos, moves, scores, count, orderByEval, tryEtc, beta, key, symmetry, lower, upper, cutoffValue))
                return cutoffValue;
        }

        // 3. Search the moves, best-scored first.
        int best = -2, bestMove = -1;
        if (count == 0) {
            Position child = pos;
            child.play(Move::pass());
            best = -search(child, -beta, -alpha);
        } else {
            for (int i = 0; i < count; i++) {
                bringBestToFront(moves, scores, i, count);
                Position child = pos;
                child.play(moves[i]);
                int value = -search(child, -beta, -alpha);
                if (aborted) return 0;
                if (value > best) { best = value; bestMove = moves[i].placementId; }
                if (value > alpha) alpha = value;
                if (alpha >= beta) {
                    rememberCutoff(pos, moves[i], i);
                    break;
                }
            }
        }
        if (aborted) return 0;

        // 4. Store what we learned.
        int newLower = lower, newUpper = upper;
        if (best <= alphaAtStart) newUpper = std::min(newUpper, best);      // failed low: an upper bound
        else if (best >= betaAtStart) newLower = std::max(newLower, best);  // failed high: a lower bound
        else newLower = newUpper = best;                                    // exact
        uint64_t work = nodes - nodesAtStart + 1;
        int workLog2 = 63 - __builtin_clzll(work);
        int canonicalBest = bestMove >= 0 ? pieceTables.symmetricPlacement[bestMove][symmetry] : stored.bestMove;
        table->store(key, newLower, newUpper, canonicalBest, workLog2);
        return best;
    }

    // Snake-length bounds: if my sure length beats the opponent's best possible one I have won,
    // and so on.
    static void narrowByBounds(const Position& pos, int& lower, int& upper) {
        SnakeBounds me = snakeBounds(pos, pos.sideToMove);
        SnakeBounds opp = snakeBounds(pos, pos.sideToMove ^ 1);
        int boundLower = me.lower > opp.upper ? 1 : me.lower >= opp.upper ? 0 : -1;
        int boundUpper = me.upper < opp.lower ? -1 : me.upper <= opp.lower ? 0 : 1;
        lower = std::max(lower, boundLower);
        upper = std::min(upper, boundUpper);
    }

    // Generates the moves with cheap ordering scores; returns their number.
    int scoreMoves(const Position& pos, int tableMove, Move* moves, int* scores) {
        Bitboard own = pos.ownCells(), opp = pos.opponentCells();
        Bitboard opponentFront = neighboursOf(snakeShapeOf(opp).ends) & pos.emptyCells();
        int ply = std::min<int>(pos.ply, MAX_PLY - 1);
        int killer1 = killers[ply][0], killer2 = killers[ply][1];
        const std::vector<int32_t>& hist = history[pos.sideToMove];
        int count = 0;
        forEachLegalMove(pos, [&](Move m) {
            const Placement& p = m.placement();
            int score = hist[m.placementId] + PIECE_SIZE[p.piece] * 64;  // big pieces first
            if (p.border & own) score += 256;                // grows an own snake
            if (p.cells & opponentFront) score += 128;       // blocks an opponent snake end
            else if (p.border & opp) score += 32;            // at least touches the opponent
            if (m.placementId == tableMove) score = 1 << 30;
            else if (m.placementId == killer1) score = 1 << 29;
            else if (m.placementId == killer2) score = 1 << 28;
            moves[count] = m;
            scores[count] = score;
            count++;
        });
        return count;
    }

    // Plays every move once: re-scores it by the learned evaluation of the child (table and killer
    // moves keep their priority) and/or tries an enhanced transposition cutoff (a child already
    // known to be good enough). Returns true with cutoffValue set when the node is decided.
    bool examineChildren(const Position& pos, Move* moves, int* scores, int count, bool orderByEval, bool tryEtc,
                         int beta, uint64_t key, int symmetry, int lower, int upper, int& cutoffValue) {
        for (int i = 0; i < count; i++) {
            Position child = pos;
            child.play(moves[i]);
            if (tryEtc) {
                int value = UNKNOWN;
                if (child.isOver()) {
                    value = -gameResult(child);
                } else {
                    int childSymmetry;
                    TTEntry e;
                    if (table->probe(child.canonicalKey(childSymmetry), e)) value = -e.upper;  // our value >= -upper
                }
                if (value >= beta) {
                    table->store(key, std::max(lower, value), upper, pieceTables.symmetricPlacement[moves[i].placementId][symmetry], 0);
                    cutoffValue = value;
                    return true;
                }
            }
            if (orderByEval && scores[i] < (1 << 28))
                scores[i] = child.isOver() ? -gameResult(child) * 100000 : -(int)(1000 * learnedLogit(child));
        }
        return false;
    }

    // Selection sort step: moves the best-scored of moves[i..count) to position i. (Usually only
    // the first move or two are ever searched, so sorting everything up front would be wasted.)
    static void bringBestToFront(Move* moves, int* scores, int i, int count) {
        int best = i;
        for (int j = i + 1; j < count; j++)
            if (scores[j] > scores[best]) best = j;
        std::swap(moves[i], moves[best]);
        std::swap(scores[i], scores[best]);
    }

    void rememberCutoff(const Position& pos, Move m, int moveNumber) {
        history[pos.sideToMove][m.placementId] += 1 + moveNumber / 8 + 4 * pos.ply;
        int ply = std::min<int>(pos.ply, MAX_PLY - 1);
        if (killers[ply][0] != m.placementId) {
            killers[ply][1] = killers[ply][0];
            killers[ply][0] = m.placementId;
        }
    }
};
