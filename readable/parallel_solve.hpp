// Parallel exact solve of one position: the root moves (symmetric duplicates removed, most
// promising first by the learned evaluation) are handed out to threads, each with its own
// solver. Stops as soon as one move is proven to win.
#pragma once
#include <functional>
#include <mutex>
#include <thread>
#include "solver.hpp"

struct RootSolveResult {
    int value = Solver::UNKNOWN;  // value of the position for the side to move
    uint64_t nodes = 0;
    double seconds = 0;
    std::vector<std::pair<Move, int>> perMove;  // value of every root move for the side to move (UNKNOWN if not solved)
};

using RootMoveCallback = std::function<void(Move, int value, uint64_t nodes, double seconds)>;

// known: root moves already solved earlier (e.g. by an interrupted run) - they are skipped.
// onResult: called (under a lock) for every newly solved root move.
// Environment SOLVER_SHARED=<log2 entries>: all threads share one transposition table.
inline RootSolveResult parallelSolve(const Position& root, double seconds, int threads, int tableLog2 = 22,
                                     const std::vector<std::pair<Move, int>>& known = {},
                                     RootMoveCallback onResult = nullptr) {
    auto startTime = std::chrono::steady_clock::now();
    auto elapsed = [&](std::chrono::steady_clock::time_point since) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    };

    std::vector<Move> moves = symmetryUniqueMoves(root);
    std::vector<double> promise(moves.size());
    for (size_t i = 0; i < moves.size(); i++) {
        Position child = root;
        child.play(moves[i]);
        promise[i] = child.isOver() ? -gameResult(child) * 1000.0 : -learnedLogit(child);
    }
    std::vector<size_t> order(moves.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return promise[a] > promise[b]; });

    std::vector<int> value(moves.size(), Solver::UNKNOWN);
    std::atomic<bool> stop{false};
    for (auto& [move, v] : known)
        for (size_t i = 0; i < moves.size(); i++)
            if (moves[i] == move) {
                value[i] = v;
                if (v == 1) stop = true;
            }

    std::atomic<size_t> nextIndex{0};
    std::atomic<uint64_t> totalNodes{0};
    std::mutex resultLock;
    threads = std::max(1, std::min<int>(threads, (int)moves.size()));
    int sharedLog2 = envInt("SOLVER_SHARED", 0);
    std::shared_ptr<TranspositionTable> sharedTable = sharedLog2 ? std::make_shared<TranspositionTable>(sharedLog2 - 1) : nullptr;

    auto worker = [&] {
        std::unique_ptr<Solver> solver(sharedTable ? new Solver(sharedTable) : new Solver(tableLog2));
        solver->stopFlag = &stop;
        for (;;) {
            size_t k = nextIndex++;
            if (k >= order.size() || stop) break;
            size_t i = order[k];
            if (value[i] != Solver::UNKNOWN) continue;  // known from a previous run
            Position child = root;
            child.play(moves[i]);
            auto moveStart = std::chrono::steady_clock::now();
            double timeLeft = seconds - elapsed(startTime);
            if (timeLeft <= 0) break;
            int v = solver->solve(child, timeLeft);
            totalNodes += solver->nodes;
            if (v == Solver::UNKNOWN) continue;
            std::lock_guard<std::mutex> lock(resultLock);
            value[i] = -v;
            if (-v == 1) stop = true;
            if (onResult) onResult(moves[i], -v, solver->nodes, elapsed(moveStart));
        }
    };
    std::vector<std::thread> pool;
    for (int w = 0; w < threads; w++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    RootSolveResult result;
    int best = -2;
    bool allKnown = true;
    for (size_t i = 0; i < moves.size(); i++) {
        result.perMove.push_back({moves[i], value[i]});
        if (value[i] == Solver::UNKNOWN) allKnown = false;
        else best = std::max(best, value[i]);
    }
    if (best == 1 || (allKnown && best > -2)) result.value = best;
    result.nodes = totalNodes;
    result.seconds = elapsed(startTime);
    return result;
}
