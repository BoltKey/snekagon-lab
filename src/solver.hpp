// Exact solver: negamax alpha-beta over {-1, 0, +1} (loss/draw/win for the side
// to move), transposition table keyed by the symmetry-canonical hash, snake-length
// bounds for early cutoffs, TT-move + killer + history move ordering.
#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include "core.hpp"
#include "eval.hpp"

struct TTEntry {
    uint64_t key;
    int8_t lo, hi;  // value bounds (side to move)
    int16_t best;   // best move gid in canonical frame (-1 none / pass)
};

struct Solver {
    std::vector<TTEntry> tt;
    uint64_t mask;
    std::vector<int32_t> hist[2];
    int16_t killer[32][2];
    uint64_t nodes = 0;
    std::atomic<bool>* stop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool useDeadline = false;
    bool aborted = false;
    uint64_t nodeLimit = UINT64_MAX;
    int evalOrderRem = 7;  // order children by the learned evaluation when >= this many pieces are left

    explicit Solver(int log2size = 22) {
        tt.assign(size_t(1) << log2size, TTEntry{0, -2, 2, -1});
        mask = (uint64_t(1) << log2size) - 1;
        for (auto& h : hist) h.assign(T.NGID, 0);
        std::memset(killer, -1, sizeof killer);
    }
    void clear() {
        std::fill(tt.begin(), tt.end(), TTEntry{0, -2, 2, -1});
        for (auto& h : hist) std::fill(h.begin(), h.end(), 0);
        std::memset(killer, -1, sizeof killer);
    }

    static int terminalValue(const Pos& s) {
        int d = finalScore(s);
        int v = d > 0 ? 1 : d < 0 ? -1 : 0;
        return s.stm == 0 ? v : -v;
    }

    bool checkAbort() {
        if (aborted) return true;
        if ((nodes & 4095) == 0) {
            if (stop && stop->load(std::memory_order_relaxed)) aborted = true;
            else if (useDeadline && std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        if (nodes > nodeLimit) aborted = true;
        return aborted;
    }

    int search(const Pos& s, int alpha, int beta) {
        nodes++;
        if (checkAbort()) return 0;
        if (s.over()) return terminalValue(s);

        int lo = -1, hi = 1;
        int t;
        uint64_t k = s.key(t);
        TTEntry& e = tt[k & mask];
        int ttMove = -1;
        bool hit = e.key == k;
        if (hit) {
            lo = e.lo; hi = e.hi;
            if (lo >= hi || lo >= beta) return lo;
            if (hi <= alpha) return hi;
            if (e.best >= 0) ttMove = T.gidSym[e.best][T.inv[t]];
        }
        // snake-length bound cutoffs
        int lbMe, ubMe, lbOp, ubOp;
        snakeBounds(s, s.stm, lbMe, ubMe);
        snakeBounds(s, s.stm ^ 1, lbOp, ubOp);
        int blo = lbMe > ubOp ? 1 : lbMe >= ubOp ? 0 : -1;
        int bhi = ubMe < lbOp ? -1 : ubMe <= lbOp ? 0 : 1;
        lo = std::max(lo, blo);
        hi = std::min(hi, bhi);
        if (lo >= hi || lo >= beta) return lo;
        if (hi <= alpha) return hi;
        alpha = std::max(alpha, lo);
        beta = std::min(beta, hi);
        int a0 = alpha, b0 = beta;

        // generate + order
        Move moves[3000];
        int score[3000];
        int n = 0;
        BB me = s.own[s.stm];
        BB opp = s.own[s.stm ^ 1];
        BB oppFront = expand(shapeOf(opp).ends) & s.empty();
        int ply = std::min<int>(s.ply, 31);
        int k1 = killer[ply][0], k2 = killer[ply][1];
        auto& H = hist[s.stm];
        s.forEachLegal([&](Move m) {
            const Placement& pl = m.pl();
            int sc = H[m.gid] + PSIZE[m.piece()] * 64;
            if (pl.nb & me) sc += 256;
            if (pl.m & oppFront) sc += 128;
            else if (pl.nb & opp) sc += 32;
            if (m.gid == ttMove) sc = 1 << 30;
            else if (m.gid == k1) sc = 1 << 29;
            else if (m.gid == k2) sc = 1 << 28;
            moves[n] = m; score[n] = sc; n++;
        });
        int rem = pieceCount(s.avail[0]) + pieceCount(s.avail[1]);
        if (n > 1 && rem >= evalOrderRem) {
            for (int i = 0; i < n; i++) {
                if (score[i] >= (1 << 28)) continue;  // keep TT / killer priority
                Pos c = s; c.play(moves[i]);
                score[i] = c.over() ? -terminalValue(c) * 100000 : -(int)(1000 * learnedLogit(c));
            }
        }
        int best = -2, bestGid = -1;
        if (n == 0) {
            Pos c = s;
            c.play(Move{});
            best = -search(c, -beta, -alpha);
        } else {
            for (int i = 0; i < n; i++) {
                int bi = i;
                for (int j = i + 1; j < n; j++) if (score[j] > score[bi]) bi = j;
                std::swap(moves[i], moves[bi]); std::swap(score[i], score[bi]);
                Pos c = s;
                c.play(moves[i]);
                int v = -search(c, -beta, -alpha);
                if (aborted) return 0;
                if (v > best) { best = v; bestGid = moves[i].gid; }
                if (v > alpha) alpha = v;
                if (alpha >= beta) {
                    H[moves[i].gid] += 1 + i / 8 + 4 * s.ply;
                    if (killer[ply][0] != moves[i].gid) { killer[ply][1] = killer[ply][0]; killer[ply][0] = moves[i].gid; }
                    break;
                }
            }
        }
        if (aborted) return 0;
        // store
        int8_t nlo = (int8_t)lo, nhi = (int8_t)hi;
        if (best <= a0) nhi = std::min<int8_t>(nhi, best);
        else if (best >= b0) nlo = std::max<int8_t>(nlo, best);
        else nlo = nhi = best;
        e.key = k; e.lo = nlo; e.hi = nhi;
        if (bestGid >= 0) e.best = (int16_t)T.gidSym[bestGid][t];
        else if (!hit) e.best = -1;
        return best;
    }

    // Returns value for the side to move, or -9 if aborted.
    // (parallel version: see parallelSolve below)
    int solve(const Pos& s, double seconds = 0, uint64_t maxNodes = UINT64_MAX) {
        aborted = false;
        nodes = 0;
        nodeLimit = maxNodes;
        useDeadline = seconds > 0;
        if (useDeadline)
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::microseconds((long long)(seconds * 1e6));
        int v = search(s, -1, 1);
        return aborted ? -9 : v;
    }
};

// Parallel exact solve: root moves (symmetry-reduced, ordered by the learned
// evaluation) are distributed over threads, each with its own solver/TT. Stops
// as soon as a winning move is found. perMove receives the value of every root
// move for the side to move (-9 = unknown).
#include <functional>
#include <mutex>
#include <thread>
struct ParallelSolveResult {
    int value = -9;  // for the side to move, -9 unknown
    uint64_t nodes = 0;
    double seconds = 0;
    std::vector<std::pair<Move, int>> perMove;
};
// known: root moves whose value is already known (e.g. from an interrupted run) - skipped.
// onResult: called (under a lock) for every newly solved root move.
inline ParallelSolveResult parallelSolve(const Pos& root, double seconds, int threads, int ttLog2 = 22,
                                         const std::vector<std::pair<Move, int>>& known = {},
                                         std::function<void(Move, int, uint64_t, double)> onResult = nullptr) {
    auto t0 = std::chrono::steady_clock::now();
    ParallelSolveResult res;
    std::vector<Move> all, mv;
    root.genMoves(all);
    if (all.empty()) all.push_back(Move{});
    std::vector<uint64_t> seen;
    for (Move m : all) {
        Pos c = root; c.play(m);
        int t; uint64_t k = c.key(t);
        if (std::find(seen.begin(), seen.end(), k) == seen.end()) { seen.push_back(k); mv.push_back(m); }
    }
    std::vector<double> ord(mv.size());
    for (size_t i = 0; i < mv.size(); i++) {
        Pos c = root; c.play(mv[i]);
        ord[i] = c.over() ? -Solver::terminalValue(c) * 1000.0 : -learnedLogit(c);
    }
    std::vector<size_t> idx(mv.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return ord[a] > ord[b]; });
    std::vector<int> val(mv.size(), -9);
    std::atomic<bool> stop{false};
    for (auto& [m, v] : known)
        for (size_t i = 0; i < mv.size(); i++)
            if (mv[i] == m) { val[i] = v; if (v == 1) stop = true; }
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> nodes{0};
    std::mutex mu;
    threads = std::max(1, std::min<int>(threads, (int)mv.size()));
    std::vector<std::thread> th;
    for (int w = 0; w < threads; w++)
        th.emplace_back([&] {
            Solver S(ttLog2);
            S.stop = &stop;
            for (;;) {
                size_t k = next++;
                if (k >= idx.size() || stop) break;
                size_t i = idx[k];
                if (val[i] != -9) continue;  // known from a previous run
                Pos c = root; c.play(mv[i]);
                auto ts = std::chrono::steady_clock::now();
                double left = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (left <= 0) break;
                int v = S.solve(c, left);
                nodes += S.nodes;
                if (v == -9) continue;
                std::lock_guard<std::mutex> lk(mu);
                val[i] = -v;
                if (-v == 1) stop = true;
                if (onResult) onResult(mv[i], -v, S.nodes, std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
            }
        });
    for (auto& t : th) t.join();
    int best = -2;
    bool allKnown = true;
    for (size_t i = 0; i < mv.size(); i++) {
        res.perMove.push_back({mv[i], val[i]});
        if (val[i] == -9) allKnown = false; else best = std::max(best, val[i]);
    }
    if (best == 1 || (allKnown && best > -2)) res.value = best;
    res.nodes = nodes;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}
