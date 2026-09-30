// Playing engine: iterative-deepening alpha-beta (PVS) on the heuristic evaluation,
// exact snake-length bounds as hard cutoffs, the exact solver once few pieces are
// left, forward pruning (best-N by 1-ply eval) at inner nodes, parallel over root
// moves with multi-PV so the UI gets real scores for the top moves.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include "core.hpp"
#include "eval.hpp"
#include "solver.hpp"

constexpr int WIN = 100000;
constexpr int INF = 1000000;

struct AbParams {
    int solveRem = 6;            // total pieces left (both players) at which the exact solver is tried
    uint64_t solveNodes = 20000; // node budget for that attempt
    int width = 24;              // moves searched at nodes with depth >= 2 (after 1-ply eval sort)
};
inline AbParams AP;

struct AbEntry {
    uint64_t key;
    int32_t score;
    int8_t depth;
    uint8_t flag;  // 1 lower, 2 upper, 3 exact
    int16_t best;
};

inline int terminalScore(const Pos& s) {
    int d = longest(s.own[s.stm]) - longest(s.own[s.stm ^ 1]);
    return d > 0 ? WIN + 100 * d : d < 0 ? -WIN + 100 * d : 0;
}

struct AbSearcher {
    std::vector<AbEntry> tt;
    uint64_t mask;
    Solver solver;
    std::vector<int32_t> hist[2];
    int16_t killer[32][2];
    uint64_t nodes = 0;
    std::atomic<bool>* stop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool aborted = false;

    explicit AbSearcher(int log2 = 19) : solver(19) {
        tt.assign(size_t(1) << log2, AbEntry{0, 0, -1, 0, -1});
        mask = (uint64_t(1) << log2) - 1;
        for (auto& h : hist) h.assign(T.NGID, 0);
        std::memset(killer, -1, sizeof killer);
    }

    bool checkAbort() {
        if (aborted) return true;
        if ((nodes & 1023) == 0) {
            if ((stop && stop->load(std::memory_order_relaxed)) || std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        return aborted;
    }

    // exact knowledge from bounds / small endgames; returns true and sets v if known
    bool exactKnown(const Pos& s, int& v) {
        int lbM, ubM, lbO, ubO;
        snakeBounds(s, s.stm, lbM, ubM);
        snakeBounds(s, s.stm ^ 1, lbO, ubO);
        if (lbM > ubO) { v = WIN + 100 * (lbM - ubO); return true; }
        if (ubM < lbO) { v = -WIN - 100 * (lbO - ubM); return true; }
        if (lbM == ubM && lbO == ubO && lbM == lbO) { v = 0; return true; }
        int rem = pieceCount(s.avail[0]) + pieceCount(s.avail[1]);
        if (rem <= AP.solveRem) {
            int r = solver.solve(s, 0, AP.solveNodes);
            if (r != -9) { v = r > 0 ? WIN : r < 0 ? -WIN : 0; return true; }
        }
        return false;
    }

    int ab(const Pos& s, int depth, int alpha, int beta) {
        nodes++;
        if (checkAbort()) return 0;
        if (s.over()) return terminalScore(s);
        if (depth <= 0) return evaluate(s);

        int t;
        uint64_t key = s.key(t);
        AbEntry& e = tt[key & mask];
        int ttMove = -1;
        if (e.key == key) {
            if (e.depth >= depth) {
                if (e.flag == 3) return e.score;
                if (e.flag == 1 && e.score >= beta) return e.score;
                if (e.flag == 2 && e.score <= alpha) return e.score;
            }
            if (e.best >= 0) ttMove = T.gidSym[e.best][T.inv[t]];
        }
        int ev;
        if (exactKnown(s, ev)) return ev;

        std::vector<Move> mv;
        s.genMoves(mv);
        if (mv.empty()) {
            Pos c = s; c.play(Move{});
            return -ab(c, depth, -beta, -alpha);  // a pass does not use up depth
        }
        int n = (int)mv.size();
        std::vector<int> sc(n);
        int ply = std::min<int>(s.ply, 31);
        auto& H = hist[s.stm];
        if (depth >= 2) {
            // order by 1-ply evaluation, keep the best `width`
            for (int i = 0; i < n; i++) {
                Pos c = s; c.play(mv[i]);
                sc[i] = c.over() ? -terminalScore(c) : -evaluate(c);
                if (mv[i].gid == ttMove) sc[i] += 10 * WIN;
                else if (mv[i].gid == killer[ply][0] || mv[i].gid == killer[ply][1]) sc[i] += 200;
            }
        } else {
            BB me = s.own[s.stm];
            BB oppFront = expand(shapeOf(s.own[s.stm ^ 1]).ends) & s.empty();
            for (int i = 0; i < n; i++) {
                const Placement& pl = mv[i].pl();
                int x = H[mv[i].gid] + PSIZE[mv[i].piece()] * 64;
                if (pl.nb & me) x += 512;
                if (pl.m & oppFront) x += 256;
                if (mv[i].gid == ttMove) x = 1 << 30;
                else if (mv[i].gid == killer[ply][0]) x = 1 << 29;
                else if (mv[i].gid == killer[ply][1]) x = 1 << 28;
                sc[i] = x;
            }
        }
        std::vector<int> idx(n);
        for (int i = 0; i < n; i++) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return sc[a] > sc[b]; });
        if (depth >= 2 && n > AP.width) n = AP.width;

        int a0 = alpha, best = -INF, bestGid = -1;
        for (int k = 0; k < n; k++) {
            Move m = mv[idx[k]];
            Pos c = s; c.play(m);
            int v;
            if (k == 0) v = -ab(c, depth - 1, -beta, -alpha);
            else {
                v = -ab(c, depth - 1, -alpha - 1, -alpha);
                if (v > alpha && v < beta) v = -ab(c, depth - 1, -beta, -alpha);
            }
            if (aborted) return 0;
            if (v > best) { best = v; bestGid = m.gid; }
            if (v > alpha) alpha = v;
            if (alpha >= beta) {
                H[m.gid] += depth * depth;
                if (killer[ply][0] != m.gid) { killer[ply][1] = killer[ply][0]; killer[ply][0] = m.gid; }
                break;
            }
        }
        e.key = key;
        e.score = best;
        e.depth = (int8_t)depth;
        e.flag = best <= a0 ? 2 : best >= beta ? 1 : 3;
        e.best = bestGid >= 0 ? (int16_t)T.gidSym[bestGid][t] : -1;
        return best;
    }
};

struct RootResult {
    Move mv;
    int score = 0;
    bool exact = false;  // score is exact (not just an upper bound)
    int depth = 0;
};

struct AbAnalysis {
    std::vector<RootResult> moves;  // sorted, best first
    int depth = 0;
    uint64_t nodes = 0;
};

inline std::vector<Move> uniqueRootMoves(const Pos& s) {
    std::vector<Move> mv, out;
    s.genMoves(mv);
    if (mv.empty()) return {Move{}};
    std::vector<uint64_t> seen;
    for (Move m : mv) {
        Pos c = s; c.play(m);
        int t; uint64_t k = c.key(t);
        if (std::find(seen.begin(), seen.end(), k) == seen.end()) { seen.push_back(k); out.push_back(m); }
    }
    return out;
}

inline double scoreToWinProb(int score) {
    if (score >= WIN / 2) return 1.0;
    if (score <= -WIN / 2) return 0.0;
    return 1.0 / (1.0 + std::exp(-score / 100.0));
}

inline AbAnalysis abAnalyze(const Pos& root, double seconds, int threads, int multipv = 1, int maxDepth = 64) {
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    auto deadline = t0 + std::chrono::microseconds((long long)(seconds * 1e6));
    std::atomic<bool> stop{false};
    static std::vector<std::unique_ptr<AbSearcher>> pool;  // reused between calls (TT warm)
    while ((int)pool.size() < threads) pool.emplace_back(new AbSearcher());
    for (int i = 0; i < threads; i++) { pool[i]->stop = &stop; pool[i]->deadline = deadline; pool[i]->aborted = false; pool[i]->nodes = 0; }

    std::vector<Move> mv = uniqueRootMoves(root);
    int n = (int)mv.size();
    std::vector<RootResult> cur(n);
    for (int i = 0; i < n; i++) cur[i].mv = mv[i];
    // depth 1: exact where known, otherwise evaluation
    for (int i = 0; i < n; i++) {
        Pos c = root; c.play(mv[i]);
        int v;
        if (c.over()) v = -terminalScore(c);
        else if (pool[0]->exactKnown(c, v)) v = -v;
        else v = -evaluate(c);
        cur[i].score = v; cur[i].exact = true; cur[i].depth = 1;
    }
    std::stable_sort(cur.begin(), cur.end(), [](auto& a, auto& b) { return a.score > b.score; });
    AbAnalysis res;
    res.moves = cur;
    res.depth = 1;
    if (n == 1) return res;

    for (int depth = 2; depth <= maxDepth; depth++) {
        double el = std::chrono::duration<double>(clk::now() - t0).count();
        if (el > seconds * 0.35) break;
        std::vector<RootResult> next = res.moves;
        for (auto& r : next) { r.exact = false; r.depth = depth; }
        std::atomic<int> nextIdx{0};
        std::mutex mu;
        std::vector<int> exactScores;  // for the multi-PV threshold
        auto threshold = [&]() {
            std::lock_guard<std::mutex> lk(mu);
            if ((int)exactScores.size() < multipv) return -INF;
            std::vector<int> v = exactScores;
            std::nth_element(v.begin(), v.begin() + (multipv - 1), v.end(), std::greater<int>());
            return v[multipv - 1];
        };
        std::vector<char> done(n, 0);
        auto searchOne = [&](AbSearcher& S, int i) {
            Pos c = root; c.play(next[i].mv);
            int a = threshold();
            int v;
            if (a == -INF) v = -S.ab(c, depth - 1, -INF, INF);
            else {
                v = -S.ab(c, depth - 1, -a - 1, -a);
                if (!S.aborted && v > a) v = -S.ab(c, depth - 1, -INF, -a);
            }
            if (S.aborted) return;
            std::lock_guard<std::mutex> lk(mu);
            next[i].score = v;
            next[i].exact = a == -INF || v > a;
            if (next[i].exact) exactScores.push_back(v);
            done[i] = 1;
        };
        auto worker = [&](int w) {
            AbSearcher& S = *pool[w];
            for (;;) {
                int i = nextIdx++;
                if (i >= n || S.aborted) break;
                searchOne(S, i);
            }
        };
        // first move alone (establishes the window), rest in parallel
        searchOne(*pool[0], nextIdx++);
        if (pool[0]->aborted) break;
        std::vector<std::thread> th;
        for (int w = 1; w < threads; w++) th.emplace_back(worker, w);
        worker(0);
        for (auto& x : th) x.join();
        bool complete = true;
        for (int i = 0; i < n; i++) if (!done[i]) complete = false;
        if (!complete) break;
        std::stable_sort(next.begin(), next.end(), [](auto& a, auto& b) {
            if (a.exact != b.exact) return a.exact;
            return a.score > b.score;
        });
        res.moves = next;
        res.depth = depth;
        if (next[0].exact && next[0].score >= WIN / 2) break;  // proven win found
        bool allLost = true;
        for (auto& r : next) if (r.score > -WIN / 2) allLost = false;
        if (allLost) break;
    }
    for (int i = 0; i < threads; i++) res.nodes += pool[i]->nodes;
    return res;
}
