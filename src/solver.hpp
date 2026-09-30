// Exact solver: negamax alpha-beta over {-1, 0, +1} (loss/draw/win for the side
// to move), transposition table keyed by the symmetry-canonical hash, snake-length
// bounds for early cutoffs, TT-move + history move ordering.
#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include "core.hpp"

struct TTEntry {
    uint64_t key;
    int8_t lo, hi;  // value bounds (side to move)
    int16_t best;   // best move gid in canonical frame (-1 none / pass)
};

struct Solver {
    std::vector<TTEntry> tt;
    uint64_t mask;
    std::vector<int32_t> hist[2];
    uint64_t nodes = 0;
    std::atomic<bool>* stop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool useDeadline = false;
    bool aborted = false;
    uint64_t nodeLimit = UINT64_MAX;

    explicit Solver(int log2size = 22) {
        tt.assign(size_t(1) << log2size, TTEntry{0, -2, 2, -1});
        mask = (uint64_t(1) << log2size) - 1;
        for (auto& h : hist) h.assign(T.NGID, 0);
    }
    void clear() {
        std::fill(tt.begin(), tt.end(), TTEntry{0, -2, 2, -1});
        for (auto& h : hist) std::fill(h.begin(), h.end(), 0);
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

        // bound cutoffs
        int lbMe, ubMe, lbOp, ubOp;
        snakeBounds(s, s.stm, lbMe, ubMe);
        snakeBounds(s, s.stm ^ 1, lbOp, ubOp);
        int lo = lbMe > ubOp ? 1 : lbMe >= ubOp ? 0 : -1;
        int hi = ubMe < lbOp ? -1 : ubMe <= lbOp ? 0 : 1;
        if (lo == hi || lo >= beta) return lo;
        if (hi <= alpha) return hi;

        int t;
        uint64_t k = s.key(t);
        TTEntry& e = tt[k & mask];
        int ttMove = -1;
        if (e.key == k) {
            lo = std::max<int>(lo, e.lo);
            hi = std::min<int>(hi, e.hi);
            if (lo >= hi || lo >= beta) return lo;
            if (hi <= alpha) return hi;
            if (e.best >= 0) ttMove = T.gidSym[e.best][T.inv[t]];
        }
        alpha = std::max(alpha, lo);
        beta = std::min(beta, hi);
        int a0 = alpha, b0 = beta;

        // generate + order
        Move moves[3000];
        int score[3000];
        int n = 0;
        BB me = s.own[s.stm], op = s.own[s.stm ^ 1];
        auto& H = hist[s.stm];
        s.forEachLegal([&](Move m) {
            const Placement& pl = m.pl();
            int sc = H[m.gid] + PSIZE[m.piece()] * 64;
            if (pl.nb & me) sc += 256;
            if (pl.nb & op) sc += 32;
            if (m.gid == ttMove) sc = 1 << 30;
            moves[n] = m; score[n] = sc; n++;
        });
        int best = -2, bestGid = -1;
        if (n == 0) {
            Pos c = s;
            c.play(Move{});
            best = -search(c, -beta, -alpha);
        } else {
            for (int i = 0; i < n; i++) {
                // selection sort step
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
                    break;
                }
            }
        }
        if (aborted) return 0;
        // store
        int8_t nlo = -2, nhi = 2;
        if (best <= a0) nhi = best;
        else if (best >= b0) nlo = best;
        else nlo = nhi = best;
        if (e.key == k) {
            e.lo = std::max<int8_t>(e.lo, nlo);
            e.hi = std::min<int8_t>(e.hi, nhi);
        } else {
            e.key = k; e.lo = nlo; e.hi = nhi;
        }
        if (bestGid >= 0) e.best = (int16_t)T.gidSym[bestGid][t];
        return best;
    }

    // Returns value for the side to move, or -9 if aborted.
    int solve(const Pos& s, double seconds = 0) {
        aborted = false;
        nodes = 0;
        useDeadline = seconds > 0;
        if (useDeadline)
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::microseconds((long long)(seconds * 1e6));
        int v = search(s, -1, 1);
        return aborted ? -9 : v;
    }
};
