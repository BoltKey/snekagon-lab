// Monte-Carlo tree search (UCT) with lightly-guided random playouts.
// Root-parallel: each thread grows its own tree, root statistics are merged.
#pragma once
#include <cmath>
#include <map>
#include <random>
#include <thread>
#include "core.hpp"

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 1) {}
    uint64_t next() { return splitmix(s); }
    int below(int n) { return (int)((next() >> 33) % (uint64_t)n); }
    double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

// Playout policy: sample random placements; prefer ones that grow an own snake.
inline Move playoutMove(const Pos& s, Rng& rng, std::vector<Move>& buf) {
    uint8_t av = s.avail[s.stm];
    if (s.ply == 0) av &= 1;
    if (!av) return Move{};
    int ps[NPIECE], np = 0;
    for (int p = 0; p < NPIECE; p++) if (av >> p & 1) ps[np++] = p;
    BB me = s.own[s.stm];
    Move fallback{};
    bool haveFallback = false;
    for (int tries = 0; tries < 96; tries++) {
        int p = ps[rng.below(np)];
        const auto& L = T.PL[p];
        const Placement& pl = L[rng.below((int)L.size())];
        if (!s.legal(pl)) continue;
        if (!me || (pl.nb & me)) return Move{(int16_t)pl.gid};
        if (!haveFallback) { fallback = Move{(int16_t)pl.gid}; haveFallback = true; }
        if (tries > 40) return fallback;
    }
    if (haveFallback) return fallback;
    s.genMoves(buf);
    if (buf.empty()) return Move{};
    return buf[rng.below((int)buf.size())];
}

// result from player 1's perspective: 1 win, 0.5 draw, 0 loss
inline double playout(Pos s, Rng& rng, std::vector<Move>& buf) {
    while (!s.over()) s.play(playoutMove(s, rng, buf));
    int d = finalScore(s);
    return d > 0 ? 1.0 : d < 0 ? 0.0 : 0.5;
}

struct MctsNode {
    Move mv;
    int parent;
    int firstChild = -1, nChildren = 0;
    bool expanded = false;
    double w = 0;  // sum of results from the perspective of the player who made mv
    int n = 0;
};

struct MoveStat {
    Move mv;
    int n = 0;
    double w = 0;  // from side-to-move-at-root perspective
};

struct Mcts {
    std::vector<MctsNode> nodes;
    Rng rng;
    double C = 0.7;
    explicit Mcts(uint64_t seed) : rng(seed) {}

    void run(const Pos& root, double seconds, std::atomic<bool>* stop) {
        nodes.clear();
        nodes.reserve(1 << 20);
        nodes.push_back(MctsNode{Move{}, -1});
        std::vector<Move> buf;
        auto t0 = std::chrono::steady_clock::now();
        auto end = t0 + std::chrono::microseconds((long long)(seconds * 1e6));
        for (int it = 0;; it++) {
            if ((it & 63) == 0) {
                if (std::chrono::steady_clock::now() > end) break;
                if (stop && stop->load()) break;
                if (nodes.size() > 30000000) break;
            }
            Pos s = root;
            int cur = 0;
            // select
            while (nodes[cur].expanded && nodes[cur].nChildren > 0) {
                MctsNode& nd = nodes[cur];
                double logN = std::log((double)nd.n + 1);
                int best = -1;
                double bv = -1e9;
                for (int c = nd.firstChild; c < nd.firstChild + nd.nChildren; c++) {
                    const MctsNode& ch = nodes[c];
                    double v = ch.n == 0 ? 1e6 + rng.uni() : ch.w / ch.n + C * std::sqrt(logN / ch.n);
                    if (v > bv) { bv = v; best = c; }
                }
                cur = best;
                s.play(nodes[cur].mv);
            }
            // expand
            if (!nodes[cur].expanded && !s.over() && (cur == 0 || nodes[cur].n >= 3)) {
                s.genMoves(buf);
                if (buf.empty()) buf.push_back(Move{});
                if (cur == 0 && buf.size() > 1) {  // drop symmetric duplicates at the root
                    std::vector<uint64_t> seen;
                    std::vector<Move> uniq;
                    for (Move m : buf) {
                        Pos c = s; c.play(m);
                        int t; uint64_t k = c.key(t);
                        if (std::find(seen.begin(), seen.end(), k) == seen.end()) { seen.push_back(k); uniq.push_back(m); }
                    }
                    buf.swap(uniq);
                }
                int first = (int)nodes.size();
                for (Move m : buf) nodes.push_back(MctsNode{m, cur});
                nodes[cur].firstChild = first;
                nodes[cur].nChildren = (int)buf.size();
                nodes[cur].expanded = true;
                cur = first + rng.below((int)buf.size());
                s.play(nodes[cur].mv);
            }
            double r1 = playout(s, rng, buf);  // player-1 perspective
            // backprop; every move (incl. pass) flips the side to move, so the mover
            // of a node follows from its depth
            int depth = 0;
            for (int x = cur; x > 0; x = nodes[x].parent) depth++;
            int moverOfCur = (root.stm + depth - 1) & 1;
            for (int x = cur; x >= 0; x = nodes[x].parent) {
                MctsNode& nd = nodes[x];
                nd.n++;
                nd.w += moverOfCur == 0 ? r1 : 1.0 - r1;
                moverOfCur ^= 1;
            }
        }
    }
};

inline std::vector<MoveStat> mctsAnalyze(const Pos& root, double seconds, int threads,
                                         std::atomic<bool>* stop = nullptr) {
    std::vector<std::unique_ptr<Mcts>> trees;
    for (int i = 0; i < threads; i++) trees.emplace_back(new Mcts(0x1234 + i * 7919 + (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<std::thread> th;
    for (int i = 0; i < threads; i++) th.emplace_back([&, i] { trees[i]->run(root, seconds, stop); });
    for (auto& t : th) t.join();
    std::map<int, MoveStat> agg;
    for (auto& tr : trees) {
        auto& nd = tr->nodes;
        if (nd.empty() || !nd[0].expanded) continue;
        for (int c = nd[0].firstChild; c < nd[0].firstChild + nd[0].nChildren; c++) {
            auto& st = agg[nd[c].mv.gid];
            st.mv = nd[c].mv;
            st.n += nd[c].n;
            st.w += nd[c].w;
        }
    }
    std::vector<MoveStat> out;
    for (auto& [g, st] : agg) out.push_back(st);
    std::sort(out.begin(), out.end(), [](const MoveStat& a, const MoveStat& b) { return a.n > b.n; });
    return out;
}
