// "Analyse this position" as the UI, arena and selfplay use it: one list of root moves with win
// probabilities, from whichever method fits - an exact solve of all root moves when few pieces
// are left, otherwise the alpha-beta engine (or the old MCTS engine).
#pragma once
#include <string>
#include "engine.hpp"
#include "mcts.hpp"

struct Analysis {
    struct Item {
        Move move;
        int visits;     // MCTS: playouts through the move; alpha-beta: 1 if the score is exact
        double winProb; // for the side to move
        int exact;      // exact value for the side to move, Solver::UNKNOWN if not known
        int score;      // alpha-beta score (centi-logits, +-WIN for proven results)
    };
    bool exact = false;  // all root moves solved exactly
    int value = 0;       // exact value of the position (when exact)
    int depth = 0;
    std::string engine;  // "exact", "ab" or "mcts"
    std::vector<Item> items;  // best first
};

inline double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Solves every root move exactly (only tried when at most 8 pieces are left). Among moves with
// the same exact value, the ones the evaluation likes come first.
inline bool solveAllRootMoves(const Position& pos, double budget, Solver& solver, Analysis& analysis) {
    int piecesLeft = pieceCount(pos.piecesLeft[RED]) + pieceCount(pos.piecesLeft[BLUE]);
    if (piecesLeft > 8) return false;
    std::vector<Move> moves = symmetryUniqueMoves(pos);
    auto start = std::chrono::steady_clock::now();
    std::vector<int> exact(moves.size(), Solver::UNKNOWN);
    for (size_t i = 0; i < moves.size(); i++) {
        double timeLeft = budget - secondsSince(start);
        if (timeLeft <= 0) return false;
        Position child = pos;
        child.play(moves[i]);
        int v = solver.solve(child, timeLeft);
        if (v == Solver::UNKNOWN) return false;
        exact[i] = -v;
    }
    analysis.exact = true;
    analysis.value = -2;
    for (size_t i = 0; i < moves.size(); i++) {
        double winProb = exact[i] > 0 ? 1.0 : exact[i] < 0 ? 0.0 : 0.5;
        analysis.items.push_back({moves[i], 0, winProb, exact[i], exact[i] * WIN});
        analysis.value = std::max(analysis.value, exact[i]);
    }
    std::vector<int> eval(analysis.items.size());
    for (size_t i = 0; i < analysis.items.size(); i++) {
        Position child = pos;
        child.play(analysis.items[i].move);
        eval[i] = child.isOver() ? -finishedGameScore(child) : -evaluate(child);
    }
    std::vector<size_t> order(analysis.items.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (analysis.items[x].exact != analysis.items[y].exact) return analysis.items[x].exact > analysis.items[y].exact;
        return eval[x] > eval[y];
    });
    std::vector<Analysis::Item> sorted;
    for (size_t i : order) sorted.push_back(analysis.items[i]);
    analysis.items = sorted;
    analysis.engine = "exact";
    return true;
}

inline Analysis analyzeMcts(const Position& pos, double seconds, int threads, Solver& solver) {
    Analysis analysis;
    auto start = std::chrono::steady_clock::now();
    if (solveAllRootMoves(pos, seconds * 0.5, solver, analysis)) return analysis;
    double timeLeft = seconds - secondsSince(start);
    for (const MoveStats& s : analyzeWithMcts(pos, std::max(0.05, timeLeft), threads)) {
        double winRate = s.visits ? s.wins / s.visits : 0.5;
        analysis.items.push_back({s.move, s.visits, winRate, Solver::UNKNOWN, 0});
    }
    analysis.engine = "mcts";
    return analysis;
}

inline Analysis analyzeAlphaBeta(const Position& pos, double seconds, int threads, Solver& solver, int multipv) {
    Analysis analysis;
    auto start = std::chrono::steady_clock::now();
    if (solveAllRootMoves(pos, seconds * 0.3, solver, analysis)) return analysis;
    double timeLeft = seconds - secondsSince(start);
    EngineAnalysis result = analyzeWithEngine(pos, std::max(0.05, timeLeft), threads, multipv);
    for (const RootMoveResult& m : result.moves) {
        int exact = Solver::UNKNOWN;
        if (m.exact && m.score >= WIN / 2) exact = 1;
        else if (m.score <= -WIN / 2) exact = -1;
        analysis.items.push_back({m.move, m.exact ? 1 : 0, scoreToWinProb(m.exact ? m.display : m.score), exact, m.score});
    }
    analysis.depth = result.depth;
    analysis.engine = "ab";
    return analysis;
}

// engine: "ab" (default), "abhand" (alpha-beta with the hand-made evaluation),
// "ab:<weights file>" (alpha-beta with runtime-loaded weights) or "mcts".
inline Analysis analyze(const Position& pos, double seconds, int threads, Solver& solver, const std::string& engine, int multipv = 1) {
    if (engine == "mcts") return analyzeMcts(pos, seconds, threads, solver);
    useHandMadeEval = engine == "abhand";
    activeWeightSet = 0;
    if (engine.rfind("ab:", 0) == 0) {
        static std::string loaded;
        if (loaded != engine) {
            if (!loadWeights(engine.substr(3), 1)) {
                fprintf(stderr, "cannot load weights %s\n", engine.c_str() + 3);
                exit(1);
            }
            loaded = engine;
        }
        activeWeightSet = 1;
    }
    Analysis analysis = analyzeAlphaBeta(pos, seconds, threads, solver, multipv);
    useHandMadeEval = false;
    activeWeightSet = 0;
    return analysis;
}
