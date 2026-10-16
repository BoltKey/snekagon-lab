// Refutation sweep: does the side to move (the attacker) win, or can each of its moves be refuted?
// For every attacker move, look for a defender reply after which the attacker does not win (a win
// test of that unit; with REFUTE_LOSS=1 one after which the attacker loses, which shows that the
// attacker loses). Pass 1: defender replies that refuted other moves first, then the defender's best
// replies by the evaluation, at most REFUTE_TRIES (12) tries of REFUTE_NODES (60M) nodes each,
// attacker moves spread over threads. Pass 2, for the moves pass 1 left open: every defender reply,
// most promising first (parallelSolve with a yes/no question), which ends in a refutation or a proof
// that the move survives. Both passes alternate on chunks of REFUTE_CHUNK (64) attacker moves and
// stop at the first move that survives, so a strong move is proved without refuting every weaker one.
// Attacker moves are taken in the evaluation's order after the 8 best of a REFUTE_SEARCH (2) second
// engine search; moves in `first` (answers that survived in sibling positions) are proved before all,
// in their given order, then (REFUTE_PROVE_TOP=<k>, default 0) the k best of that order, before pass 1.
// Every solve of the sweep, in both passes and all threads, uses one transposition table
// (makeSweepTable, SOLVER_SWEEP_TT): the units are siblings and share much of their trees.
// Log lines (resumable; every refutation is one position anyone can re-solve):
// "<move> refuted <reply> [<nodes> <sec>]", "<move> open", "<move> wins" / "<move> holds". The first line is
// `header` (the position, the question and the bonus); a log that starts otherwise is not resumed, so a
// log written for another position or question can never be read as this one's.
#pragma once
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "search.hpp"
#include "solver.hpp"

struct RefuteResult {
    std::string survivor;  // an attacker move that survives (wins, or holds with REFUTE_LOSS); empty if none
    size_t refuted = 0, total = 0;
    std::string error;     // the log belongs to something else or cannot be written: nothing was done
};

// The first line of `path`, or "" if the file is missing or empty.
inline std::string firstLine(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

inline RefuteResult refuteMoves(const Pos& s, int threads, const std::string& logPath, const std::string& header,
                                const std::vector<std::string>& first = {}) {
    std::string have = firstLine(logPath);
    if (!have.empty() && have != header) {
        RefuteResult bad;
        bad.error = logPath + " starts with \"" + have + "\", not \"" + header + "\"";
        return bad;
    }
    int maxTries = envInt("REFUTE_TRIES", 12);
    bool mustLose = envInt("REFUTE_LOSS", 0) != 0;
    uint64_t budget = (uint64_t)envInt("REFUTE_NODES", 60) * 1000000;
    std::map<std::string, std::string> status;  // attacker move -> refuted / open / wins / holds
    std::map<int, int> killers;                 // defender reply -> refutations so far
    {
        std::ifstream in(logPath);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string m, st, r;
            if (line[0] == '#' || !(ls >> m >> st)) continue;
            status[m] = st;
            Move rm;
            if (st == "refuted" && ls >> r && parseMove(r, rm)) killers[rm.gid]++;
        }
    }
    FILE* log = fopen(logPath.c_str(), "a");
    if (!log) {
        RefuteResult bad;
        bad.error = "cannot write " + logPath;
        return bad;
    }
    if (have.empty()) { fprintf(log, "%s\n", header.c_str()); fflush(log); }
    // attacker moves, its best first by the evaluation (the search's favourites before them)
    std::vector<Move> all = uniqueRootMoves(s);
    std::vector<std::pair<double, Move>> ranked;
    for (Move m : all) {
        Pos c = s; c.play(m);
        ranked.push_back({c.over() ? -Solver::terminalValue(c) * 1000.0 : -learnedLogit(c), m});  // the attacker's view
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    // a short engine search puts its best moves first: it ranks easy winners far higher than the
    // evaluation alone does (REFUTE_SEARCH seconds; 0 = the evaluation's order only)
    int searchSecs = envInt("REFUTE_SEARCH", 2);
    if (searchSecs > 0 && ranked.size() > 1) {
        constexpr int SEARCH_TOP = 8;
        AbAnalysis an = abAnalyze(s, searchSecs, threads, SEARCH_TOP);
        std::vector<std::pair<double, Move>> front, rest;
        for (size_t i = 0; i < an.moves.size() && (int)i < SEARCH_TOP; i++) front.push_back({0.0, an.moves[i].mv});
        for (auto& r : ranked) {
            bool inFront = false;
            for (auto& f : front) inFront |= f.second == r.second;
            if (!inFront) rest.push_back(r);
        }
        ranked = front;
        ranked.insert(ranked.end(), rest.begin(), rest.end());
    }
    std::vector<Move> todo;
    for (auto& [sc, m] : ranked) if (!status.count(moveStr(m))) todo.push_back(m);
    printf("%zu attacker moves, %zu settled earlier\n", all.size(), all.size() - todo.size());
    fflush(stdout);
    const char* survives = mustLose ? "holds" : "wins";
    std::string survivor;
    for (auto& [m, st] : status) if (st == survives) survivor = m;
    std::mutex mu;
    std::shared_ptr<LTT> table = makeSweepTable();
    // pass 1 on todo[from, to): a few node-budgeted tries per move, moves spread over threads
    auto quickPass = [&](size_t from, size_t to) {
        std::atomic<size_t> next{from};
        std::vector<std::thread> th;
        for (int w = 0; w < threads; w++)
            th.emplace_back([&] {
                Solver S(table);
                for (;;) {
                    size_t k = next++;
                    if (k >= to) break;
                    Move m = todo[k];
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        if (status.count(moveStr(m))) continue;  // settled by a pass 2 already
                    }
                    Pos a = s; a.play(m);
                    std::vector<Move> replies;
                    a.genMoves(replies);
                    std::vector<std::pair<double, Move>> order;
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        for (Move r : replies) {
                            auto it = killers.find(r.gid);
                            Pos c = a; c.play(r);  // attacker to move in c: a low attacker logit is good for the defender
                            double sc = it != killers.end() ? 1000.0 + it->second : c.over() ? -1000.0 * Solver::terminalValue(c) : -learnedLogit(c);
                            order.push_back({sc, r});
                        }
                    }
                    std::sort(order.begin(), order.end(), [](auto& x, auto& y) { return x.first > y.first; });
                    bool refuted = false;
                    for (int t = 0; t < maxTries && t < (int)order.size() && !refuted; t++) {
                        Pos u = a; u.play(order[t].second);
                        S.newPosition();
                        auto ts = std::chrono::steady_clock::now();
                        // does the attacker win (avoid losing, with REFUTE_LOSS) after this reply?
                        int v = mustLose ? S.solve(u, 0, budget, -1, 0) : S.solve(u, 0, budget, 0, 1);
                        if (v == -9 || (mustLose ? v >= 0 : v >= 1)) continue;
                        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
                        std::lock_guard<std::mutex> lk(mu);
                        killers[order[t].second.gid]++;
                        fprintf(log, "%s refuted %s %llu %.1f\n", moveStr(m).c_str(), moveStr(order[t].second).c_str(), (unsigned long long)S.nodes, sec);
                        fflush(log);
                        refuted = true;
                    }
                    std::lock_guard<std::mutex> lk(mu);
                    status[moveStr(m)] = refuted ? "refuted" : "open";
                    if (!refuted) { fprintf(log, "%s open\n", moveStr(m).c_str()); fflush(log); }
                    printf("%zu/%zu %s %s\n", k + 1, todo.size(), moveStr(m).c_str(), refuted ? "refuted" : "open");
                    fflush(stdout);
                }
            });
        for (auto& t : th) t.join();
    };
    // pass 2 on every open move, the moves in `first` first (in their order), then the attacker's best
    // first: the defender's whole reply list, either a refutation or a proof that the move survives -
    // which settles the question
    std::vector<Move> proveOrder;
    auto fullPass = [&] {
        for (Move m : proveOrder) {
            std::string ms = moveStr(m);
            if (!survivor.empty()) return;
            auto it = status.find(ms);
            if (it == status.end() || it->second != "open") continue;
            Pos a = s; a.play(m);
            // does the defender lose (or, with REFUTE_LOSS, fail to win)?
            auto r = parallelSolve(a, 1e9, threads, 22, {}, nullptr, mustLose ? WIN_TEST : LOSS_TEST, table);
            int refuting = mustLose ? 1 : 0;  // a defender reply this good refutes the move
            if (r.value != -9 && r.value < refuting) {
                status[ms] = survives;
                survivor = ms;
                fprintf(log, "%s %s\n", ms.c_str(), survives);
            }
            for (auto& [rm, v] : r.perMove)
                if (r.value == refuting && v == refuting) {
                    status[ms] = "refuted";
                    fprintf(log, "%s refuted %s\n", ms.c_str(), moveStr(rm).c_str());
                    break;
                }
            fflush(log);
            // "pass 2: <move> <status>" is a line of its own (the cost follows on the next), so a log
            // reader can match it whole
            printf("pass 2: %s %s\n", ms.c_str(), status[ms].c_str());
            printf("pass 2 cost: %s %llu nodes, %.1f s\n", ms.c_str(), (unsigned long long)r.nodes, r.seconds);
            fflush(stdout);
        }
    };
    for (const std::string& tok : first) {
        Move fm;
        if (!parseMove(tok, fm) || std::find(all.begin(), all.end(), fm) == all.end()) continue;
        if (!status.count(moveStr(fm))) status[moveStr(fm)] = "open";
        if (std::find(proveOrder.begin(), proveOrder.end(), fm) == proveOrder.end()) proveOrder.push_back(fm);
    }
    for (auto& [sc, m] : ranked)
        if (std::find(proveOrder.begin(), proveOrder.end(), m) == proveOrder.end()) proveOrder.push_back(m);
    // REFUTE_PROVE_TOP=<k>: the k best moves of the order above also get a full search before any
    // pass 1 (a move that survives is proved at once, without spending refutation tries on it first)
    int proveTop = envInt("REFUTE_PROVE_TOP", 0);
    for (int i = 0; i < proveTop && i < (int)ranked.size(); i++)
        if (!status.count(moveStr(ranked[i].second))) status[moveStr(ranked[i].second)] = "open";
    size_t chunk = std::max(1, envInt("REFUTE_CHUNK", 64));
    fullPass();
    for (size_t from = 0; from < todo.size() && survivor.empty(); from += chunk) {
        quickPass(from, std::min(todo.size(), from + chunk));
        fullPass();
    }
    fclose(log);
    RefuteResult res;
    res.survivor = survivor;
    res.total = all.size();
    for (Move m : all) {
        auto it = status.find(moveStr(m));
        res.refuted += it != status.end() && it->second == "refuted";
    }
    return res;
}
