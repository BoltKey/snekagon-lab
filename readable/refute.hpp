// Refutation sweep: does the side to move (the attacker) win, or can each of its moves be refuted?
//
// A refutation of an attacker move is a defender reply after which the attacker does not win (with
// REFUTE_LOSS=1: after which the attacker loses, so that refuting every move shows that the
// attacker loses). An attacker move without a refutation survives: it wins (with REFUTE_LOSS: it
// holds), which settles the question.
//
//   pass 1 ("quick"): per attacker move, the defender replies that refuted other moves first, then
//            the defender's best replies by the evaluation; at most REFUTE_TRIES (12) win tests of
//            REFUTE_NODES million (60) nodes each. Attacker moves are spread over the threads.
//   pass 2 ("full"): per attacker move that pass 1 left open, every defender reply, most promising
//            first, with a parallel yes/no solve - either a refutation or a proof that the move
//            survives.
// The passes alternate on chunks of REFUTE_CHUNK (64) attacker moves, so a strong move that
// survives is found without first refuting every weaker one; the sweep stops at the first move
// that survives.
//
// The attacker's moves are taken best first: the 8 best of a REFUTE_SEARCH (2) second engine
// search, which ranks easy winners far higher than the evaluation alone (0: no search), then the
// rest by the evaluation. Moves passed as `first` (answers that won in sibling positions: the same
// move often wins against many defender replies) are settled by pass 2 before all others.
//
// Every solve of the sweep, in both passes and all threads, uses one transposition table
// (makeSweepTable, SOLVER_SWEEP_TT): the units are siblings and share much of their trees.
//
// The log is appended to and read back on a restart (resume). Every refutation in it is one
// position anyone can solve again:
//   <move> refuted <reply> <nodes> <seconds>     (pass 2 logs no nodes and seconds)
//   <move> open                                  (pass 1 found no refutation)
//   <move> wins | holds                          (the move survives; holds: with REFUTE_LOSS)
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "engine.hpp"
#include "notation.hpp"
#include "parallel_solve.hpp"

struct RefuteResult {
    std::string survivor;  // an attacker move that survives (wins, or holds with REFUTE_LOSS); empty if none
    size_t refuted = 0;    // attacker moves refuted
    size_t total = 0;      // attacker moves (symmetric duplicates removed)
    std::string error;     // the log belongs to another position or question, or cannot be written: nothing was done
};

// The first line of a file, or "" if it is missing or empty.
inline std::string firstLineOf(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

class RefutationSweep {
  public:
    // Settings (environment variables).
    bool mustLose = envInt("REFUTE_LOSS", 0) != 0;                         // refute "does not lose" instead of "wins"
    int maxTries = envInt("REFUTE_TRIES", 12);                             // pass 1: win tests per attacker move
    uint64_t nodeBudget = (uint64_t)envInt("REFUTE_NODES", 60) * 1000000;  // pass 1: nodes per win test
    size_t chunk = std::max(1, envInt("REFUTE_CHUNK", 64));               // attacker moves per round of the passes
    int searchSeconds = envInt("REFUTE_SEARCH", 2);                        // engine search that picks the first moves
    int proveTop = envInt("REFUTE_PROVE_TOP", 0);                          // the best moves in that order proved before pass 1

    RefutationSweep(const Position& root, int threads) : root(root), threads(threads) {}

    // header: the log's first line, naming the position, the question and the bonus. A log that starts
    // with anything else is not resumed: a refutation of "wins" must never count as one of "loses".
    RefuteResult run(const std::string& logPath, const std::string& header, const std::vector<std::string>& first) {
        std::string existing = firstLineOf(logPath);
        if (!existing.empty() && existing != header) {
            RefuteResult wrongLog;
            wrongLog.error = logPath + " starts with \"" + existing + "\", not \"" + header + "\"";
            return wrongLog;
        }
        readLog(logPath);
        log = fopen(logPath.c_str(), "a");
        if (!log) {
            RefuteResult unwritable;
            unwritable.error = "cannot write " + logPath;
            return unwritable;
        }
        if (existing.empty()) { fprintf(log, "%s\n", header.c_str()); fflush(log); }
        std::vector<Move> all = symmetryUniqueMoves(root);
        bestFirst = attackerMovesBestFirst(all);
        std::vector<Move> todo;  // not settled by an earlier run
        for (Move m : bestFirst)
            if (!status.count(moveToString(m))) todo.push_back(m);
        printf("%zu attacker moves, %zu settled earlier\n", all.size(), all.size() - todo.size());
        fflush(stdout);
        for (auto& [move, state] : status)
            if (state == survives()) survivor = move;

        // the moves to settle first are open for pass 2 (pass 1 skips them)
        for (const std::string& text : first) {
            Move m;
            if (!parseMove(text, m) || std::find(all.begin(), all.end(), m) == all.end()) continue;
            if (!status.count(moveToString(m))) status[moveToString(m)] = "open";
            if (std::find(proveOrder.begin(), proveOrder.end(), m) == proveOrder.end()) proveOrder.push_back(m);
        }
        for (Move m : bestFirst)
            if (std::find(proveOrder.begin(), proveOrder.end(), m) == proveOrder.end()) proveOrder.push_back(m);
        // and so are the best proveTop moves: one that survives is proved without refutation tries first
        for (int i = 0; i < proveTop && i < (int)bestFirst.size(); i++)
            if (!status.count(moveToString(bestFirst[i]))) status[moveToString(bestFirst[i])] = "open";
        fullPass();
        for (size_t from = 0; from < todo.size() && survivor.empty(); from += chunk) {
            quickPass(todo, from, std::min(todo.size(), from + chunk));
            fullPass();
        }
        fclose(log);

        RefuteResult result;
        result.survivor = survivor;
        result.total = all.size();
        for (Move m : all) result.refuted += statusOf(moveToString(m)) == "refuted";
        return result;
    }

  private:
    static constexpr int SEARCH_FAVOURITES = 8;      // the engine search's best moves go first
    static constexpr double NO_TIME_LIMIT = 1e9;     // pass 2 runs until the move is settled

    Position root;
    int threads;
    std::shared_ptr<TranspositionTable> table = makeSweepTable();  // for every solve of the sweep
    std::map<std::string, std::string> status;  // attacker move -> "refuted", "open", "wins" or "holds"
    std::map<int, int> refutations;             // defender reply (placement id) -> attacker moves it refuted
    std::vector<Move> bestFirst;                // all attacker moves, in the order they are settled
    std::vector<Move> proveOrder;               // pass 2's order: the moves in `first` (in their order), then bestFirst
    std::string survivor;
    FILE* log = nullptr;
    std::mutex lock;                            // status, refutations, log and stdout during pass 1

    const char* survives() const { return mustLose ? "holds" : "wins"; }

    std::string statusOf(const std::string& move) const {
        auto it = status.find(move);
        return it == status.end() ? std::string() : it->second;
    }

    // What earlier runs found: the status of attacker moves, and how often each defender reply
    // refuted one.
    void readLog(const std::string& path) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string move, state, reply;
            if (line[0] == '#' || !(fields >> move >> state)) continue;  // the header
            status[move] = state;
            Move replyMove;
            if (state == "refuted" && fields >> reply && parseMove(reply, replyMove)) refutations[replyMove.placementId]++;
        }
    }

    // The engine search's favourites, then the other moves best first by the evaluation.
    std::vector<Move> attackerMovesBestFirst(const std::vector<Move>& all) {
        std::vector<std::pair<double, Move>> ranked;
        for (Move m : all) {
            Position child = root;
            child.play(m);
            ranked.push_back({child.isOver() ? -gameResult(child) * 1000.0 : -learnedLogit(child), m});  // the attacker's view
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::vector<Move> moves;
        if (searchSeconds > 0 && ranked.size() > 1) {
            EngineAnalysis analysis = analyzeWithEngine(root, searchSeconds, threads, SEARCH_FAVOURITES);
            for (size_t i = 0; i < analysis.moves.size() && (int)i < SEARCH_FAVOURITES; i++) moves.push_back(analysis.moves[i].move);
        }
        size_t favourites = moves.size();
        for (auto& [score, m] : ranked)
            if (std::find(moves.begin(), moves.begin() + favourites, m) == moves.begin() + favourites) moves.push_back(m);
        return moves;
    }

    // Pass 1 on todo[from, to): a few node-limited win tests per move, moves spread over threads.
    void quickPass(const std::vector<Move>& todo, size_t from, size_t to) {
        std::atomic<size_t> next{from};
        std::vector<std::thread> pool;
        for (int w = 0; w < threads; w++)
            pool.emplace_back([&] {
                Solver solver(table);
                for (;;) {
                    size_t k = next++;
                    if (k >= to) break;
                    Move m = todo[k];
                    {
                        std::lock_guard<std::mutex> guard(lock);
                        if (status.count(moveToString(m))) continue;  // settled by a pass 2 already
                    }
                    bool refuted = tryRefutations(solver, m);
                    std::lock_guard<std::mutex> guard(lock);
                    status[moveToString(m)] = refuted ? "refuted" : "open";
                    if (!refuted) {
                        fprintf(log, "%s open\n", moveToString(m).c_str());
                        fflush(log);
                    }
                    printf("%zu/%zu %s %s\n", k + 1, todo.size(), moveToString(m).c_str(), refuted ? "refuted" : "open");
                    fflush(stdout);
                }
            });
        for (auto& t : pool) t.join();
    }

    // Pass 1 on one attacker move: node-limited win tests after the most promising defender
    // replies; logs and counts the refutation it finds.
    bool tryRefutations(Solver& solver, Move m) {
        Position afterMove = root;
        afterMove.play(m);
        std::vector<Move> replies;
        generateMoves(afterMove, replies);
        // replies that refuted other moves first, then the ones that leave the attacker the
        // lowest evaluation
        std::vector<std::pair<double, Move>> order;
        {
            std::lock_guard<std::mutex> guard(lock);
            for (Move reply : replies) {
                auto known = refutations.find(reply.placementId);
                Position child = afterMove;
                child.play(reply);  // the attacker to move
                double score = known != refutations.end() ? 1000.0 + known->second
                               : child.isOver()           ? -1000.0 * gameResult(child)
                                                          : -learnedLogit(child);
                order.push_back({score, reply});
            }
        }
        std::sort(order.begin(), order.end(), [](auto& x, auto& y) { return x.first > y.first; });
        for (int t = 0; t < maxTries && t < (int)order.size(); t++) {
            Position afterReply = afterMove;
            afterReply.play(order[t].second);
            solver.newPosition();
            auto start = std::chrono::steady_clock::now();
            // does the attacker win (avoid losing, with REFUTE_LOSS) after this reply?
            int value = mustLose ? solver.solve(afterReply, 0, nodeBudget, -1, 0) : solver.solve(afterReply, 0, nodeBudget, 0, 1);
            if (value == Solver::UNKNOWN || (mustLose ? value >= 0 : value >= 1)) continue;
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::lock_guard<std::mutex> guard(lock);
            refutations[order[t].second.placementId]++;
            fprintf(log, "%s refuted %s %llu %.1f\n", moveToString(m).c_str(), moveToString(order[t].second).c_str(),
                    (unsigned long long)solver.nodes, seconds);
            fflush(log);
            return true;
        }
        return false;
    }

    // Pass 2 on every open move, the attacker's best first: all of the defender's replies, which
    // either refutes the move or proves that it survives.
    void fullPass() {
        for (Move m : proveOrder) {
            std::string move = moveToString(m);
            if (!survivor.empty()) return;
            if (statusOf(move) != "open") continue;
            Position afterMove = root;
            afterMove.play(m);
            // does the defender lose (or, with REFUTE_LOSS, fail to win)?
            RootSolveResult r = parallelSolve(afterMove, NO_TIME_LIMIT, threads, SOLVER_TABLE_LOG2, {}, nullptr,
                                              mustLose ? SolveQuestion::WIN_TEST : SolveQuestion::LOSS_TEST, table);
            int refuting = mustLose ? 1 : 0;  // a defender reply this good refutes the move
            if (r.value != Solver::UNKNOWN && r.value < refuting) {
                status[move] = survives();
                survivor = move;
                fprintf(log, "%s %s\n", move.c_str(), survives());
            }
            for (auto& [reply, value] : r.perMove)
                if (r.value == refuting && value == refuting) {
                    status[move] = "refuted";
                    fprintf(log, "%s refuted %s\n", move.c_str(), moveToString(reply).c_str());
                    break;
                }
            fflush(log);
            // "pass 2: <move> <status>" is a line of its own (the cost follows on the next), so a log
            // reader can match it whole
            printf("pass 2: %s %s\n", move.c_str(), statusOf(move).c_str());
            printf("pass 2 cost: %s %llu nodes, %.1f s\n", move.c_str(), (unsigned long long)r.nodes, r.seconds);
            fflush(stdout);
        }
    }
};

// Settles the attacker's moves after `root` (`first` before all others) until one survives or all
// are refuted, logging to and resuming from `logPath`.
inline RefuteResult refuteMoves(const Position& root, int threads, const std::string& logPath, const std::string& header,
                                const std::vector<std::string>& first = {}) {
    return RefutationSweep(root, threads).run(logPath, header, first);
}
