// The playing engine: iterative-deepening alpha-beta (principal variation search) on the
// learned evaluation. Unlike the exact solver its results are estimates, because it
//   - stops at a depth limit and uses the evaluation there,
//   - searches only the `width` most promising moves at nodes with depth >= 2 (forward pruning).
// Exact knowledge is used wherever it is cheap: snake-length bounds, and the exact solver once
// few pieces are left. The root moves are searched in parallel, with a multi-PV window so the
// UI gets real scores for the several best moves, not just the best one.
//
// Scores: centi-logits from the evaluation, or +-WIN (+ 100 per cell of margin) for proven results.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include "solver.hpp"

constexpr int WIN = 100000;
constexpr int INF = 1000000;

struct EngineParams {
    int solvePiecesLeft = 6;       // try the exact solver when at most this many pieces are left (both players)
    uint64_t solveNodes = 20000;   // node budget for that attempt
    int width = 24;                // moves searched at nodes with depth >= 2 (best by 1-ply evaluation)
};
inline EngineParams engineParams;

// Score of a finished game for the side to move.
inline int finishedGameScore(const Position& pos) {
    int lead = pos.longestSnake[pos.sideToMove] - pos.longestSnake[pos.sideToMove ^ 1];
    return lead > 0 ? WIN + 100 * lead : lead < 0 ? -WIN + 100 * lead : 0;
}

inline bool isProvenScore(int score) { return std::abs(score) >= WIN / 2; }

// Logistic win probability of a score (proven results: 1 or 0).
inline double scoreToWinProb(int score) {
    if (score >= WIN / 2) return 1.0;
    if (score <= -WIN / 2) return 0.0;
    return 1.0 / (1.0 + std::exp(-score / 100.0));
}

class AlphaBetaSearcher {
  public:
    uint64_t nodes = 0;
    std::atomic<bool>* stopFlag = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool aborted = false;

    explicit AlphaBetaSearcher(int log2Entries = 19) : solver(19) {
        table.assign(size_t(1) << log2Entries, Entry{0, 0, -1, 0, -1});
        mask = (uint64_t(1) << log2Entries) - 1;
        for (auto& h : history) h.assign(pieceTables.placements.size(), 0);
        std::memset(killers, -1, sizeof killers);
    }

    // Exact score when cheaply known (bounds decide the game, or the solver finishes within its
    // small budget).
    bool exactScore(const Position& pos, int& score) {
        SnakeBounds me = snakeBounds(pos, pos.sideToMove);
        SnakeBounds opp = snakeBounds(pos, pos.sideToMove ^ 1);
        if (me.lower > opp.upper) { score = WIN + 100 * (me.lower - opp.upper); return true; }
        if (me.upper < opp.lower) { score = -WIN - 100 * (opp.lower - me.upper); return true; }
        if (me.lower == me.upper && opp.lower == opp.upper && me.lower == opp.lower) { score = 0; return true; }
        int piecesLeft = pieceCount(pos.piecesLeft[RED]) + pieceCount(pos.piecesLeft[BLUE]);
        if (piecesLeft <= engineParams.solvePiecesLeft) {
            int result = solver.solve(pos, 0, engineParams.solveNodes);
            if (result != Solver::UNKNOWN) { score = result > 0 ? WIN : result < 0 ? -WIN : 0; return true; }
        }
        return false;
    }

    int search(const Position& pos, int depth, int alpha, int beta) {
        nodes++;
        if (shouldAbort()) return 0;
        if (pos.isOver()) return finishedGameScore(pos);
        if (depth <= 0) return evaluate(pos);

        int symmetry;
        uint64_t key = pos.canonicalKey(symmetry);
        Entry& entry = table[key & mask];
        int tableMove = -1;
        if (entry.key == key) {
            if (entry.depth >= depth) {
                if (entry.bound == EXACT) return entry.score;
                if (entry.bound == LOWER && entry.score >= beta) return entry.score;
                if (entry.bound == UPPER && entry.score <= alpha) return entry.score;
            }
            if (entry.bestMove >= 0) tableMove = pieceTables.symmetricPlacement[entry.bestMove][geometry.inverseSymmetry[symmetry]];
        }
        int exact;
        if (exactScore(pos, exact)) return exact;

        std::vector<Move> moves;
        generateMoves(pos, moves);
        if (moves.empty()) {
            Position child = pos;
            child.play(Move::pass());
            return -search(child, depth, -beta, -alpha);  // a pass does not use up depth
        }
        int count = (int)moves.size();
        std::vector<int> scores(count);
        if (depth >= 2) {
            if (!scoreByEvaluation(pos, moves, tableMove, scores)) return 0;  // aborted
        } else {
            scoreCheaply(pos, moves, tableMove, scores);
        }
        std::vector<int> order(count);
        for (int i = 0; i < count; i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return scores[a] > scores[b]; });

        // Forward pruning: at depth >= 2 only the best `width` moves - unless every move tried so
        // far is a proven loss (a pruned move might still save the game, and we must not report a
        // false forced win for the opponent).
        int searchLimit = depth >= 2 ? std::min(count, engineParams.width) : count;
        int alphaAtStart = alpha, best = -INF, bestMove = -1;
        for (int k = 0; k < count; k++) {
            if (k >= searchLimit && best > -WIN / 2) break;
            Move m = moves[order[k]];
            Position child = pos;
            child.play(m);
            int value;
            if (k == 0) {
                value = -search(child, depth - 1, -beta, -alpha);
            } else {
                // principal variation search: prove with a null window that the move is not
                // better, re-search with the full window only if it is
                value = -search(child, depth - 1, -alpha - 1, -alpha);
                if (value > alpha && value < beta) value = -search(child, depth - 1, -beta, -alpha);
            }
            if (aborted) return 0;
            if (value > best) { best = value; bestMove = m.placementId; }
            if (value > alpha) alpha = value;
            if (alpha >= beta) {
                rememberCutoff(pos, m, depth);
                break;
            }
        }
        entry.key = key;
        entry.score = best;
        entry.depth = (int8_t)depth;
        entry.bound = best <= alphaAtStart ? UPPER : best >= beta ? LOWER : EXACT;
        entry.bestMove = bestMove >= 0 ? (int16_t)pieceTables.symmetricPlacement[bestMove][symmetry] : -1;
        return best;
    }

  private:
    enum Bound : uint8_t { LOWER = 1, UPPER = 2, EXACT = 3 };
    struct Entry {
        uint64_t key;
        int32_t score;
        int8_t depth;
        uint8_t bound;
        int16_t bestMove;  // canonical orientation
    };
    std::vector<Entry> table;  // always-replace hash table
    uint64_t mask;
    Solver solver;
    std::vector<int32_t> history[2];
    int16_t killers[32][2];

    bool shouldAbort() {
        if (aborted) return true;
        // inner nodes are expensive (1-ply evaluation of all children), so check often
        if ((nodes & 63) == 0) {
            if ((stopFlag && stopFlag->load(std::memory_order_relaxed)) || std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        return aborted;
    }

    // Depth >= 2: order by the evaluation after the move (table move first, killers boosted).
    // Returns false if the search was aborted meanwhile.
    bool scoreByEvaluation(const Position& pos, const std::vector<Move>& moves, int tableMove, std::vector<int>& scores) {
        int ply = std::min<int>(pos.ply, 31);
        for (size_t i = 0; i < moves.size(); i++) {
            if ((++nodes & 63) == 0 && shouldAbort()) return false;
            Position child = pos;
            child.play(moves[i]);
            scores[i] = child.isOver() ? -finishedGameScore(child) : -evaluate(child);
            if (moves[i].placementId == tableMove) scores[i] += 10 * WIN;
            else if (moves[i].placementId == killers[ply][0] || moves[i].placementId == killers[ply][1]) scores[i] += 200;
        }
        return true;
    }

    // Depth 1: cheap shape heuristics, like the exact solver.
    void scoreCheaply(const Position& pos, const std::vector<Move>& moves, int tableMove, std::vector<int>& scores) {
        int ply = std::min<int>(pos.ply, 31);
        Bitboard own = pos.ownCells();
        Bitboard opponentFront = neighboursOf(snakeShapeOf(pos.opponentCells()).ends) & pos.emptyCells();
        const std::vector<int32_t>& hist = history[pos.sideToMove];
        for (size_t i = 0; i < moves.size(); i++) {
            const Placement& p = moves[i].placement();
            int score = hist[moves[i].placementId] + PIECE_SIZE[p.piece] * 64;
            if (p.border & own) score += 512;
            if (p.cells & opponentFront) score += 256;
            if (moves[i].placementId == tableMove) score = 1 << 30;
            else if (moves[i].placementId == killers[ply][0]) score = 1 << 29;
            else if (moves[i].placementId == killers[ply][1]) score = 1 << 28;
            scores[i] = score;
        }
    }

    void rememberCutoff(const Position& pos, Move m, int depth) {
        history[pos.sideToMove][m.placementId] += depth * depth;
        int ply = std::min<int>(pos.ply, 31);
        if (killers[ply][0] != m.placementId) {
            killers[ply][1] = killers[ply][0];
            killers[ply][0] = m.placementId;
        }
    }
};

// ---- root search -----------------------------------------------------------------------------

struct RootMoveResult {
    Move move;
    int score = 0;
    bool exact = false;  // the score is exact for this depth (not just an upper bound from the multi-PV window)
    int depth = 0;
    int display = 0;     // score averaged over the last two iterations (damps odd/even swings)
};

struct EngineAnalysis {
    std::vector<RootMoveResult> moves;  // best first
    int depth = 0;
    uint64_t nodes = 0;
};

// Searchers are kept between calls so their tables stay warm.
inline std::vector<std::unique_ptr<AlphaBetaSearcher>>& searcherPool() {
    static std::vector<std::unique_ptr<AlphaBetaSearcher>> pool;
    return pool;
}

// Depth 1: every root move scored by exact knowledge where cheap, otherwise by the evaluation.
inline std::vector<RootMoveResult> scoreRootMovesAtDepth1(const Position& root, const std::vector<Move>& moves, AlphaBetaSearcher& searcher) {
    std::vector<RootMoveResult> results(moves.size());
    for (size_t i = 0; i < moves.size(); i++) {
        Position child = root;
        child.play(moves[i]);
        int score;
        if (child.isOver()) score = -finishedGameScore(child);
        else if (searcher.exactScore(child, score)) score = -score;
        else score = -evaluate(child);
        results[i].move = moves[i];
        results[i].score = score;
        results[i].exact = true;
        results[i].depth = 1;
        results[i].display = score;
    }
    std::stable_sort(results.begin(), results.end(), [](auto& a, auto& b) { return a.score > b.score; });
    return results;
}

// One iteration at `depth` over all root moves, in parallel. The best `multipv` moves get exact
// scores; every other move is only shown to be worse than the multipv-th best so far.
// Returns false if time ran out before all moves were searched.
inline bool searchRootIteration(const Position& root, int depth, int threads, int multipv, std::vector<RootMoveResult>& results) {
    auto& pool = searcherPool();
    int count = (int)results.size();
    for (auto& r : results) { r.exact = false; r.depth = depth; }
    std::atomic<int> nextIndex{0};
    std::mutex lock;
    std::vector<int> exactScores;
    std::vector<char> done(count, 0);

    // the multipv-th best exact score so far: moves must beat it to need an exact score
    auto threshold = [&]() {
        std::lock_guard<std::mutex> guard(lock);
        if ((int)exactScores.size() < multipv) return -INF;
        std::vector<int> v = exactScores;
        std::nth_element(v.begin(), v.begin() + (multipv - 1), v.end(), std::greater<int>());
        return v[multipv - 1];
    };
    auto searchMove = [&](AlphaBetaSearcher& searcher, int i) {
        Position child = root;
        child.play(results[i].move);
        int bar = threshold();
        int score;
        if (bar == -INF) {
            score = -searcher.search(child, depth - 1, -INF, INF);
        } else {
            score = -searcher.search(child, depth - 1, -bar - 1, -bar);
            if (!searcher.aborted && score > bar) score = -searcher.search(child, depth - 1, -INF, -bar);
        }
        if (searcher.aborted) return;
        std::lock_guard<std::mutex> guard(lock);
        results[i].score = score;
        results[i].exact = bar == -INF || score > bar;
        if (results[i].exact) exactScores.push_back(score);
        done[i] = 1;
    };
    auto worker = [&](int w) {
        AlphaBetaSearcher& searcher = *pool[w];
        for (;;) {
            int i = nextIndex++;
            if (i >= count || searcher.aborted) break;
            searchMove(searcher, i);
        }
    };

    // the first (best so far) move alone sets the window, the rest in parallel
    searchMove(*pool[0], nextIndex++);
    if (pool[0]->aborted) return false;
    std::vector<std::thread> helpers;
    for (int w = 1; w < threads; w++) helpers.emplace_back(worker, w);
    worker(0);
    for (auto& t : helpers) t.join();
    for (int i = 0; i < count; i++)
        if (!done[i]) return false;
    return true;
}

// Odd and even depths are biased in opposite directions (the side that moved last looks better),
// so the displayed score is the average of the last two iterations where both are exact.
inline void smoothDisplayedScores(std::vector<RootMoveResult>& current, const std::vector<RootMoveResult>& previous) {
    for (auto& r : current) {
        r.display = r.score;
        if (!r.exact || isProvenScore(r.score)) continue;
        for (auto& p : previous)
            if (p.move == r.move) {
                if (p.exact && !isProvenScore(p.score)) r.display = (r.score + p.score) / 2;
                break;
            }
    }
    // the value of the position (the best move) is smoothed even when the best move changed
    const RootMoveResult& previousBest = previous[0];
    RootMoveResult& best = current[0];
    if (best.exact && previousBest.exact && !isProvenScore(best.score) && !isProvenScore(previousBest.score))
        best.display = (best.score + previousBest.score) / 2;
}

inline EngineAnalysis analyzeWithEngine(const Position& root, double seconds, int threads, int multipv = 1, int maxDepth = 64) {
    using Clock = std::chrono::steady_clock;
    auto startTime = Clock::now();
    auto deadline = startTime + std::chrono::microseconds((long long)(seconds * 1e6));
    std::atomic<bool> stop{false};
    auto& pool = searcherPool();
    while ((int)pool.size() < threads) pool.emplace_back(new AlphaBetaSearcher());
    for (int i = 0; i < threads; i++) {
        pool[i]->stopFlag = &stop;
        pool[i]->deadline = deadline;
        pool[i]->aborted = false;
        pool[i]->nodes = 0;
    }

    std::vector<Move> moves = symmetryUniqueMoves(root);
    EngineAnalysis analysis;
    analysis.moves = scoreRootMovesAtDepth1(root, moves, *pool[0]);
    analysis.depth = 1;
    if (moves.size() == 1) return analysis;

    for (int depth = 2; depth <= maxDepth; depth++) {
        // an iteration takes a few times longer than the previous one: do not start one that
        // cannot finish
        double elapsed = std::chrono::duration<double>(Clock::now() - startTime).count();
        if (elapsed > seconds * 0.35) break;
        std::vector<RootMoveResult> next = analysis.moves;
        if (!searchRootIteration(root, depth, threads, multipv, next)) break;
        std::stable_sort(next.begin(), next.end(), [](auto& a, auto& b) {
            if (a.exact != b.exact) return a.exact;
            return a.score > b.score;
        });
        smoothDisplayedScores(next, analysis.moves);
        analysis.moves = next;
        analysis.depth = depth;
        if (next[0].exact && next[0].score >= WIN / 2) break;  // proven win found
        bool allLost = true;
        for (auto& r : next)
            if (r.score > -WIN / 2) allLost = false;
        if (allLost) break;
    }
    for (int i = 0; i < threads; i++) analysis.nodes += pool[i]->nodes;
    return analysis;
}
