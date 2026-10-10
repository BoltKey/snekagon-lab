// Exact solver: negamax alpha-beta over {-1, 0, +1} (loss/draw/win for the side
// to move), transposition table keyed by the symmetry-canonical hash, snake-length
// bounds for early cutoffs, TT-move + killer + history + learned-eval move ordering,
// enhanced transposition cutoffs (ETC). The TT is lockless and can be shared by threads.
#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include "core.hpp"
#include "eval.hpp"

// Lockless 2-way transposition table. Each slot stores key^data and data, so an entry
// torn by concurrent writes fails the key check instead of returning garbage.
// Slot 0 of a bucket keeps the entry with the most work behind it, slot 1 is always replaced.
struct LTT {
    struct Slot { std::atomic<uint64_t> k{0}, d{0}; };
    std::unique_ptr<Slot[]> s;
    size_t nb;
    uint64_t mask;
    explicit LTT(int log2buckets) : s(new Slot[size_t(2) << log2buckets]), nb(size_t(1) << log2buckets), mask(nb - 1) {}
    void clear() {
        for (size_t i = 0; i < 2 * nb; i++) { s[i].k.store(0, std::memory_order_relaxed); s[i].d.store(0, std::memory_order_relaxed); }
    }
    static uint64_t pack(int lo, int hi, int best, int work) {
        return uint64_t(lo + 2) | uint64_t(hi + 2) << 3 | uint64_t(best + 1) << 6 | uint64_t(work) << 22;
    }
    bool probe(uint64_t key, int& lo, int& hi, int& best) const {
        size_t b = (key & mask) * 2;
        for (int i = 0; i < 2; i++) {
            uint64_t k = s[b + i].k.load(std::memory_order_relaxed), d = s[b + i].d.load(std::memory_order_relaxed);
            if (d && (k ^ d) == key) {
                lo = int(d & 7) - 2; hi = int(d >> 3 & 7) - 2; best = int(d >> 6 & 0xFFFF) - 1;
                return true;
            }
        }
        return false;
    }
    void store(uint64_t key, int lo, int hi, int best, int work) {
        size_t b = (key & mask) * 2;
        uint64_t d = pack(lo, hi, best, work);
        uint64_t k0 = s[b].k.load(std::memory_order_relaxed), d0 = s[b].d.load(std::memory_order_relaxed);
        int i = ((k0 ^ d0) == key || work >= int(d0 >> 22 & 63)) ? 0 : 1;
        s[b + i].d.store(d, std::memory_order_relaxed);
        s[b + i].k.store(key ^ d, std::memory_order_relaxed);
    }
};

inline int envInt(const char* name, int def) { const char* v = getenv(name); return v ? atoi(v) : def; }
// SOLVER_RED_BONUS=b counts b extra cells for red at the end of the game (default 0 = the real rules).
// With it, "blue still wins" means blue wins by more than b: used to find the exact margin of a line.
inline const int RED_BONUS = envInt("SOLVER_RED_BONUS", 0);

struct Solver {
    std::shared_ptr<LTT> tt;
    std::vector<int32_t> hist[2];
    int16_t killer[32][2];
    uint64_t nodes = 0;
    std::atomic<bool>* stop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool useDeadline = false;
    bool aborted = false;
    uint64_t nodeLimit = UINT64_MAX;
    // profiling counters per ply: visited, cut by TT, cut by bounds, expanded
    uint64_t pVisit[16] = {}, pTT[16] = {}, pBound[16] = {}, pExp[16] = {};
    int evalOrderRem = envInt("SOLVER_EVAL_REM", 7);  // order children by the learned evaluation when >= this many pieces are left
    // deeper down, eval-order only where the side to move does not look lost: the losing side
    // has to try every move anyway, so ordering its moves is wasted work
    int evalOrderRemWin = envInt("SOLVER_EVAL_WIN_REM", 7);
    double evalWinThreshold = envInt("SOLVER_EVAL_WIN_THR", -100) / 100.0;
    int etcRem = envInt("SOLVER_ETC_REM", 99);        // enhanced transposition cutoffs when >= this many pieces are left

    // log2size: log2 of the number of entries (16 bytes each)
    explicit Solver(int log2size = 22) : tt(std::make_shared<LTT>(log2size - 1)) { init(); }
    explicit Solver(std::shared_ptr<LTT> shared) : tt(std::move(shared)) { init(); }
    std::vector<Move> mvBuf[32];
    std::vector<int> scBuf[32];
    void init() {
        for (int i = 0; i < 32; i++) { mvBuf[i].resize(3000); scBuf[i].resize(3000); }
        for (auto& h : hist) h.assign(T.NGID, 0);
        std::memset(killer, -1, sizeof killer);
    }
    void clear() {
        tt->clear();
        for (auto& h : hist) std::fill(h.begin(), h.end(), 0);
        std::memset(killer, -1, sizeof killer);
    }

    static int terminalValue(const Pos& s) {
        int d = finalScore(s) + RED_BONUS;
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
        uint64_t n0 = nodes;
        int pp = std::min<int>(s.ply, 15);
        pVisit[pp]++;

        int lo = -1, hi = 1;
        int t;
        uint64_t k = s.key(t);
        int ttMove = -1, eLo, eHi, eBest = -1;
        if (tt->probe(k, eLo, eHi, eBest)) {
            lo = eLo; hi = eHi;
            if (lo >= hi || lo >= beta) { pTT[pp]++; return lo; }
            if (hi <= alpha) { pTT[pp]++; return hi; }
            if (eBest >= 0) ttMove = T.gidSym[eBest][T.inv[t]];
        }
        // snake-length bound cutoffs
        int lbMe, ubMe, lbOp, ubOp;
        snakeBounds(s, s.stm, lbMe, ubMe);
        snakeBounds(s, s.stm ^ 1, lbOp, ubOp);
        int bonus = s.stm == 0 ? RED_BONUS : -RED_BONUS;  // the side to move's share of SOLVER_RED_BONUS
        lbMe += bonus;
        ubMe += bonus;
        int blo = lbMe > ubOp ? 1 : lbMe >= ubOp ? 0 : -1;
        int bhi = ubMe < lbOp ? -1 : ubMe <= lbOp ? 0 : 1;
        lo = std::max(lo, blo);
        hi = std::min(hi, bhi);
        if (lo >= hi || lo >= beta) { pBound[pp]++; return lo; }
        if (hi <= alpha) { pBound[pp]++; return hi; }
        pExp[pp]++;
        alpha = std::max(alpha, lo);
        beta = std::min(beta, hi);
        int a0 = alpha, b0 = beta;

        // generate + order
        // per-depth move buffers (no per-node initialisation of 3000-element arrays)
        Move* moves = mvBuf[s.ply & 31].data();
        int* score = scBuf[s.ply & 31].data();
        int n = 0;
        BB me = s.own[s.stm];
        BB opp = s.own[s.stm ^ 1];
        BB oppFront = expand(shapeOf(opp).ends) & s.empty();
        int ply = std::min<int>(s.ply, 31);
        int k1 = killer[ply][0], k2 = killer[ply][1];
        auto& H = hist[s.stm];
        s.forEachLegal([&](Move m) {
            const Placement& pl = m.pl();
            int sc = H[m.gid] + PIECE_CELLS[m.piece()] * 64;
            if (pl.nb & me) sc += 256;
            if (pl.m & oppFront) sc += 128;
            else if (pl.nb & opp) sc += 32;
            if (m.gid == ttMove) sc = 1 << 30;
            else if (m.gid == k1) sc = 1 << 29;
            else if (m.gid == k2) sc = 1 << 28;
            moves[n] = m; score[n] = sc; n++;
        });
        int rem = pieceCount(s.avail[0]) + pieceCount(s.avail[1]);
        bool doEval = n > 1 && (rem >= evalOrderRem || (rem >= evalOrderRemWin && learnedLogit(s) > evalWinThreshold));
        bool doEtc = n > 0 && rem >= etcRem;
        if (doEval || doEtc) {
            for (int i = 0; i < n; i++) {
                Pos c = s; c.play(moves[i]);
                if (doEtc) {
                    // enhanced transposition cutoff: a child already known to be good enough
                    int cv = -9;
                    if (c.over()) cv = -terminalValue(c);
                    else { int ct, clo, chi, cb; if (tt->probe(c.key(ct), clo, chi, cb)) cv = -chi; }  // our value >= -chi
                    if (cv >= beta) {
                        tt->store(k, std::max(lo, cv), hi, T.gidSym[moves[i].gid][t], 0);
                        return cv;
                    }
                }
                if (doEval && score[i] < (1 << 28))
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
        int nlo = lo, nhi = hi;
        if (best <= a0) nhi = std::min(nhi, best);
        else if (best >= b0) nlo = std::max(nlo, best);
        else nlo = nhi = best;
        uint64_t work = nodes - n0 + 1;
        int w = 63 - __builtin_clzll(work);
        tt->store(k, nlo, nhi, bestGid >= 0 ? T.gidSym[bestGid][t] : eBest, w);
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
    // SOLVER_SHARED=<log2 entries>: one transposition table shared by all threads
    int sharedLog2 = envInt("SOLVER_SHARED", 0);
    std::shared_ptr<LTT> shared = sharedLog2 ? std::make_shared<LTT>(sharedLog2 - 1) : nullptr;
    auto work = [&] {
        std::unique_ptr<Solver> SP(shared ? new Solver(shared) : new Solver(ttLog2));
        Solver& S = *SP;
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
    };
    if (threads == 1) work();  // inline, so it also runs where there are no threads (WebAssembly)
    else {
        for (int w = 0; w < threads; w++) th.emplace_back(work);
        for (auto& t : th) t.join();
    }
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
