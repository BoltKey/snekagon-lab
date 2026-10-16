// Playing engine: iterative-deepening alpha-beta (PVS) on the heuristic evaluation,
// exact snake-length bounds as hard cutoffs, the exact solver once few pieces are
// left, forward pruning (best-N by 1-ply eval) at inner nodes, parallel over root
// moves with multi-PV so the UI gets real scores for the top moves.
// Two engines: "ab", the default, and "nn", the same search using more of its move time, with exact
// verdicts on the root moves once at most 10 pieces are left, the network of nn.hpp in its evaluation
// and the network's move ranking (policy) choosing the moves it evaluates.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include "core.hpp"
#include "eval.hpp"
#include "nn.hpp"
#include "solver.hpp"

constexpr int WIN = 100000;
constexpr int INF = 1000000;

struct AbParams {
    int solveRem = 6;            // total pieces left (both players) at which the exact solver is tried
    uint64_t solveNodes = 20000; // node budget for that attempt
    int width = 24;              // moves searched at nodes with depth >= 2 (after 1-ply eval sort)
    double newIteration = 0.35;  // no iteration starts after this share of the move time
    bool partialIterations = false;  // an iteration cut off by the deadline still ranks the root moves it finished
    int exactRem = 0;            // exact root verdicts once at most this many pieces are left (0 = never)
    int exactFirstRem = 0;       // ... with a first exact phase in 1-ply order, before the heuristic search
    double exactFirstShare = 0.5;  // share of the move time for that first exact phase
    double heuristicShare = 0.35;  // share of the remaining time for the heuristic search; the exact phase gets the rest
    // With a policy (nn.hpp Policy, the "nn" engine): at depth >= 2 the 1-ply evaluation scores only the
    // policy's best policyCandidates moves and sorts them by its value plus policyWeight centi-logits per
    // nat of the policy's logit; the other moves follow in the policy's order (0: no policy).
    int policyCandidates = 0;
    int policyWeight = 100;
};
// "nn" ("ab" uses the defaults): iterations start until 60 % of the move time and still rank the root
// moves they finished when the deadline cuts them off; exact root verdicts from 10 pieces left, with a
// first exact phase from 9; with a policy, the 1-ply evaluation of only its best 48 moves per inner node.
inline AbParams abParamsNn() {
    AbParams p;
    p.newIteration = 0.6;
    p.partialIterations = true;
    p.exactRem = 10;
    p.exactFirstRem = 9;
    p.policyCandidates = 48;
    return p;
}

struct AbEntry {
    uint64_t key;
    int32_t score;
    int8_t depth;
    uint8_t flag;  // 1 lower, 2 upper, 3 exact
    int16_t best;
};

inline int terminalScore(const Pos& s) {
    int d = s.L[s.stm] - s.L[s.stm ^ 1];
    return d > 0 ? WIN + 100 * d : d < 0 ? -WIN + 100 * d : 0;
}

struct AbSearcher {
    const AbParams& P;
    std::vector<AbEntry> tt;
    uint64_t mask;
    Solver solver;
    std::vector<int32_t> hist[2];
    int16_t killer[32][2];
    uint64_t nodes = 0;
    std::atomic<bool>* stop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool aborted = false;
    // The network (the "nn" engine), which evaluates by the mean of its value and the logistic
    // evaluation (move ordering and leaves alike; leaves far below the window keep the network's value,
    // leafValue): the accumulators of the positions on the current search path, each updated from its
    // parent's.
    const nn::Net* net = nullptr;
    struct NnLevel { nn::Planes P; nn::Accumulator A; };
    std::vector<NnLevel> nnPath = std::vector<NnLevel>(64);
    int lvl = 0;
    const nn::Policy* policy = nullptr;  // move ranking from the same accumulators (with a network only)

    explicit AbSearcher(const AbParams& params, int log2 = 19) : P(params), solver(19) {
        tt.assign(size_t(1) << log2, AbEntry{0, 0, -1, 0, -1});
        mask = (uint64_t(1) << log2) - 1;
        for (auto& h : hist) h.assign(T.NGID, 0);
        std::memset(killer, -1, sizeof killer);
    }

    bool checkAbort() {
        if (aborted) return true;
        if ((nodes & 63) == 0) {  // inner nodes are expensive (1-ply eval of all children): check often
            if ((stop && stop->load(std::memory_order_relaxed)) || std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        return aborted;
    }

    // Search from a root position (resets the network's path to it).
    int searchRoot(const Pos& s, int depth, int alpha, int beta) {
        if (net) {
            lvl = 0;
            nn::planesOf(s, nnPath[0].P);
            net->refresh(nnPath[0].P, nnPath[0].A);
        }
        return ab(s, depth, alpha, beta);
    }
    // The evaluation of s, the position at the current level of the path.
    int staticEval(const Pos& s) {
        if (!net) return evaluate(s);
        return (net->evaluate(nnPath[lvl].A, s.stm, s.ply) + evaluate(s)) / 2;
    }
    // The network's value of c = s after m (legal in s, the current level) for c's side to move,
    // from s's accumulator.
    int netChild(const Pos& s, Move m, const Pos& c, const int* ub = nullptr) {
        const NnLevel& L = nnPath[lvl];
        nn::Planes Q;
        nn::planesAfter(s, L.P, m.pl(), m.piece(), c, Q, ub);
        nn::Delta d;
        nn::deltaOf(L.P, Q, d);
        nn::Accumulator A;
        net->apply(L.A, d, A);
        return net->evaluate(A, c.stm, c.ply);
    }
    // The value of c = s after m (legal in s, the current level) for c's side to move: the terminal
    // score or the evaluation.
    int childValue(const Pos& s, Move m) {
        Pos c = s; c.play(m);
        if (c.over()) return terminalScore(c);
        if (!net) return evaluate(c);
        int ub[2];  // the logistic evaluation's bounds are inputs of the network too
        int lv = evaluateBounds(c, ub);
        return (netChild(s, m, c, ub) + lv) / 2;
    }
    // A leaf's value, lazily: when the network alone puts the child more than LAZY_MARGIN below the
    // parent's alpha, the mean can only reach alpha if the logistic evaluation disagrees with it by
    // more than twice that, so the network's value stands and the logistic evaluation is skipped.
    static constexpr int LAZY_MARGIN = 100;
    int leafValue(const Pos& s, Move m, int alpha) {
        if (!net) return childValue(s, m);
        Pos c = s; c.play(m);
        if (c.over()) return terminalScore(c);
        int vn = netChild(s, m, c);
        if (-vn <= alpha - LAZY_MARGIN) return vn;
        return (vn + evaluate(c)) / 2;
    }
    // The evaluation of a position from scratch (the root moves).
    int rootEval(const Pos& c) {
        if (!net) return evaluate(c);
        return (net->evaluate(c) + evaluate(c)) / 2;
    }
    // ab() of the child c = s after m, with the network path extended to it.
    int abChild(const Pos& s, Move m, const Pos& c, int depth, int alpha, int beta) {
        if (net) {
            NnLevel& L = nnPath[lvl], &N = nnPath[lvl + 1];
            if (m.isPass()) N.P = L.P, N.A = L.A;
            else {
                nn::planesAfter(s, L.P, m.pl(), m.piece(), c, N.P);
                nn::Delta d;
                nn::deltaOf(L.P, N.P, d);
                net->apply(L.A, d, N.A);
            }
        }
        lvl++;
        int v = ab(c, depth, alpha, beta);
        lvl--;
        return v;
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
        if (rem <= P.solveRem) {
            int r = solver.solve(s, 0, P.solveNodes);
            if (r != -9) { v = r > 0 ? WIN : r < 0 ? -WIN : 0; return true; }
        }
        return false;
    }

    int ab(const Pos& s, int depth, int alpha, int beta) {
        nodes++;
        if (checkAbort()) return 0;
        if (s.over()) return terminalScore(s);
        if (depth <= 0) return staticEval(s);

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
            return -abChild(s, Move{}, c, depth, -beta, -alpha);  // a pass does not use up depth
        }
        int n = (int)mv.size();
        std::vector<int> sc(n);
        int ply = std::min<int>(s.ply, 31);
        auto& H = hist[s.stm];
        if (depth >= 2) {
            // order by 1-ply evaluation, keep the best `width`; with the policy, only its best candidates are
            // evaluated (the table move always), sorted with the policy's logit added; the rest follow in its order
            std::vector<int32_t> logit;
            std::vector<char> evaluated(n, 1);
            bool ranked = policy && P.policyCandidates > 0;
            if (ranked) {
                logit.resize(n);
                policy->logits(nnPath[lvl].A, nnPath[lvl].P, s.stm, mv.data(), n, logit.data());
                if (n > P.policyCandidates) {
                    std::vector<int> o(n);
                    for (int i = 0; i < n; i++) o[i] = i;
                    std::nth_element(o.begin(), o.begin() + P.policyCandidates, o.end(), [&](int a, int b) { return logit[a] > logit[b]; });
                    for (int k = P.policyCandidates; k < n; k++) evaluated[o[k]] = 0;
                }
            }
            for (int i = 0; i < n; i++) {
                int bonus = ranked ? (int)((int64_t)logit[i] * P.policyWeight / (nn::QA * nn::QP)) : 0;
                if (!evaluated[i] && mv[i].gid != ttMove) { sc[i] = -10 * WIN + bonus; continue; }
                if ((++nodes & 63) == 0 && checkAbort()) return 0;
                if (net) sc[i] = -childValue(s, mv[i]);
                else { Pos c = s; c.play(mv[i]); sc[i] = c.over() ? -terminalScore(c) : -evaluate(c); }
                sc[i] += bonus;
                if (mv[i].gid == ttMove) sc[i] += 10 * WIN;
                else if (mv[i].gid == killer[ply][0] || mv[i].gid == killer[ply][1]) sc[i] += 200;
            }
        } else {
            BB me = s.own[s.stm];
            BB oppFront = expand(shapeOf(s.own[s.stm ^ 1]).ends) & s.empty();
            for (int i = 0; i < n; i++) {
                const Placement& pl = mv[i].pl();
                int x = H[mv[i].gid] + PIECE_CELLS[mv[i].piece()] * 64;
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
        // forward pruning: only the best `width` moves at depth >= 2 - unless every move
        // tried so far is a proven loss (otherwise a pruned move could still save the game
        // and we would report a false "forced win" for the opponent)
        int limit = depth >= 2 ? std::min(n, P.width) : n;

        int a0 = alpha, best = -INF, bestGid = -1;
        for (int k = 0; k < n; k++) {
            if (k >= limit && best > -WIN / 2) break;
            Move m = mv[idx[k]];
            int v;
            if (net && depth == 1) {
                // the children are leaves: the network's values come straight from the parent's accumulator
                nodes++;
                if (checkAbort()) return 0;
                v = -leafValue(s, m, alpha);
            } else {
                Pos c = s; c.play(m);
                if (k == 0) v = -abChild(s, m, c, depth - 1, -beta, -alpha);
                else {
                    v = -abChild(s, m, c, depth - 1, -alpha - 1, -alpha);
                    if (v > alpha && v < beta) v = -abChild(s, m, c, depth - 1, -beta, -alpha);
                }
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

// Exact verdict on a root move, for the side to move. NotWin: proven not to win (a draw or a loss).
enum class Verdict : int8_t { Unknown, Win, NotWin, Draw, Loss };

struct RootResult {
    Move mv;
    int score = 0;
    bool exact = false;  // score is exact (not just an upper bound)
    int depth = 0;
    int display = 0;     // score averaged over the last two iterations (damps odd/even swings)
    Verdict verdict = Verdict::Unknown;
    bool exactTried = false;  // the exact phase started on this move
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

// One engine configuration with the state it keeps between moves: a searcher per thread and the
// exact solvers of the root verdicts, which share one table (later positions of a game are
// subtrees of earlier ones, so proofs carry over).
struct AbEngine {
    AbParams P;
    const nn::Net* net = nullptr;  // the network ("nn" engine), else the logistic evaluation
    const nn::Policy* policy = nullptr;
    std::vector<std::unique_ptr<AbSearcher>> pool;
    std::shared_ptr<LTT> exactTT;
    std::vector<std::unique_ptr<Solver>> exactPool;
    explicit AbEngine(const AbParams& p) : P(p) {}
};
// The default engine "ab".
inline AbEngine& abEngine() {
    static AbEngine ab{AbParams{}};
    return ab;
}

// The network file named by an engine name: "nn" (the compiled-in network) or "nn:<file>".
inline bool isNnEngine(const std::string& name) { return name == "nn" || name.rfind("nn:", 0) == 0; }
inline std::string nnNetFile(const std::string& name) { return name.size() > 3 ? name.substr(3) : ""; }
// A new "nn" engine instance with its network's policy (nn::policyFor: the compiled-in network comes with
// one, a network loaded from a file evaluates without).
inline std::unique_ptr<AbEngine> newNnEngine(const std::string& name) {
    auto e = std::make_unique<AbEngine>(abParamsNn());
    std::string netFile = nnNetFile(name);
    e->net = nn::loadNet(netFile);
    e->policy = nn::policyFor(netFile);
    return e;
}
// The "nn" engine; one instance (state kept between moves) per network.
inline AbEngine& nnEngine(const std::string& name) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<AbEngine>> engines;
    std::lock_guard<std::mutex> lk(mu);
    auto& e = engines[name];
    if (!e) e = newNnEngine(name);
    return *e;
}

// Value of s for the side to move from the exact solver with window (alpha, beta), or -9 if the
// deadline or the stop flag came first.
inline int solveWindow(Solver& S, const Pos& s, int alpha, int beta, std::chrono::steady_clock::time_point deadline,
                       std::atomic<bool>* stop) {
    S.nodes = 0;
    if (s.over()) return Solver::terminalValue(s);
    S.stop = stop;
    S.useDeadline = true;
    S.deadline = deadline;
    S.nodeLimit = UINT64_MAX;
    S.aborted = false;
    int v = S.search(s, alpha, beta);
    return S.aborted ? -9 : v;
}

// The best root move is proven: a win, a draw once no move can win, or every move loses.
inline bool verdictsSettle(const std::vector<RootResult>& moves) {
    bool allLost = !moves.empty(), open = false, draw = false;
    for (auto& r : moves) {
        if (r.verdict == Verdict::Win) return true;
        if (r.verdict != Verdict::Loss) allLost = false;
        if (r.verdict == Verdict::Unknown) open = true;
        if (r.verdict == Verdict::Draw) draw = true;
    }
    return allLost || (!open && draw);
}

// Exact verdicts on the root moves, in their current order (most promising first), until the
// deadline: first "does it win" for every move (null window below a draw), then, once no move
// wins, "does it draw" for the others. Stops at the first win or draw: nothing better exists then.
inline uint64_t exactVerdicts(const Pos& root, std::vector<RootResult>& moves, std::chrono::steady_clock::time_point deadline,
                              int threads, AbEngine& E) {
    if (!E.exactTT) E.exactTT = std::make_shared<LTT>(22);  // 2^23 entries, 128 MB
    while ((int)E.exactPool.size() < threads) E.exactPool.emplace_back(new Solver(E.exactTT));
    std::atomic<uint64_t> nodes{0};
    for (int phase = 0; phase < 2; phase++) {
        if (verdictsSettle(moves)) break;
        Verdict todo = phase == 0 ? Verdict::Unknown : Verdict::NotWin;
        // "does it draw" only once every move is known not to win
        if (phase == 1)
            for (auto& r : moves) if (r.verdict == Verdict::Unknown) return nodes;
        std::atomic<size_t> next{0};
        std::atomic<bool> stop{false};
        auto work = [&](int w) {
            Solver& S = *E.exactPool[w];
            for (;;) {
                size_t k = next++;
                if (k >= moves.size() || stop) break;
                RootResult& r = moves[k];
                if (r.verdict != todo) continue;
                r.exactTried = true;
                Pos c = root; c.play(r.mv);
                // the opponent's value: below a draw (phase 0), at most a draw (phase 1)?
                int v = phase == 0 ? solveWindow(S, c, -1, 0, deadline, &stop) : solveWindow(S, c, 0, 1, deadline, &stop);
                nodes += S.nodes;
                if (v == -9) break;
                if (phase == 0) r.verdict = v < 0 ? Verdict::Win : Verdict::NotWin;
                else r.verdict = v <= 0 ? Verdict::Draw : Verdict::Loss;
                if (r.verdict == Verdict::Win || r.verdict == Verdict::Draw) stop = true;
            }
        };
        if (threads == 1) work(0);  // inline, so it also runs where there are no threads (WebAssembly)
        else {
            std::vector<std::thread> th;
            for (int w = 0; w < threads; w++) th.emplace_back(work, w);
            for (auto& t : th) t.join();
        }
    }
    return nodes;
}

// Reorders moves as moves[0], the first of `other` not yet taken, moves[1], ... (each move once).
inline void interleave(std::vector<RootResult>& moves, const std::vector<Move>& other) {
    std::vector<int> at(T.NGID + 1, -1);  // gid + 1 -> index in moves (a pass has gid -1)
    for (size_t i = 0; i < moves.size(); i++) at[moves[i].mv.gid + 1] = (int)i;
    std::vector<char> taken(moves.size(), 0);
    std::vector<RootResult> out;
    out.reserve(moves.size());
    size_t j = 0;
    for (size_t i = 0; i < moves.size(); i++) {
        if (!taken[i]) { taken[i] = 1; out.push_back(moves[i]); }
        for (; j < other.size(); j++) {
            int k = at[other[j].gid + 1];
            if (k >= 0 && !taken[k]) { taken[k] = 1; out.push_back(moves[k]); j++; break; }
        }
    }
    moves = out;
}

// Moves proven lost go last (stable), so the heuristic search never starts from one.
inline void lostMovesLast(std::vector<RootResult>& moves) {
    std::stable_partition(moves.begin(), moves.end(), [](const RootResult& r) { return r.verdict != Verdict::Loss; });
}

// Share of the opponent's replies to m that the exact table already proves lost for the opponent:
// how far a running "does m win" proof got.
inline double proofProgress(const Pos& root, Move m, const LTT& tt) {
    Pos c = root; c.play(m);
    int proven = 0, total = 0;
    c.forEachLegal([&](Move x) {
        Pos g = c; g.play(x);
        int t, lo, hi, best;
        total++;
        if (g.over() ? Solver::terminalValue(g) > 0 : tt.probe(g.key(t), g.keyCheck(), lo, hi, best) && lo >= 1) proven++;
    });
    return total ? (double)proven / total : 0;
}

// Final ranking once verdicts are known: a proven win; the moves whose "does it win" proof was
// still running at the deadline, the furthest proof first; a proven draw; the remaining moves
// (proven not to win or never tried) in candidate order; proven losses. Scores are capped at what
// the verdict allows.
enum RankClass { ProvenWin, ProofRunning, ProvenDraw, NoWinOrUntried, ProvenLoss };
inline void rankByVerdict(const Pos& root, std::vector<RootResult>& moves, const AbEngine& E) {
    std::vector<double> progress(moves.size(), 0);
    for (size_t i = 0; i < moves.size(); i++) {
        RootResult& r = moves[i];
        if (r.verdict == Verdict::Win) { r.score = r.display = WIN; r.exact = true; }
        else if (r.verdict == Verdict::Draw) { r.score = r.display = 0; r.exact = true; }
        else if (r.verdict == Verdict::NotWin) { r.score = std::min(r.score, 0); r.display = std::min(r.display, 0); }
        else if (r.verdict == Verdict::Unknown && r.exactTried && E.exactTT) progress[i] = proofProgress(root, r.mv, *E.exactTT);
    }
    auto rankClass = [](const RootResult& r) {
        switch (r.verdict) {
            case Verdict::Win: return ProvenWin;
            case Verdict::Unknown: return r.exactTried ? ProofRunning : NoWinOrUntried;
            case Verdict::Draw: return ProvenDraw;
            case Verdict::NotWin: return NoWinOrUntried;
            default: return ProvenLoss;
        }
    };
    std::vector<size_t> order(moves.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        RankClass ca = rankClass(moves[a]), cb = rankClass(moves[b]);
        if (ca != cb) return ca < cb;
        return ca == ProofRunning && progress[a] > progress[b];
    });
    std::vector<RootResult> out;
    for (size_t i : order) out.push_back(moves[i]);
    moves = out;
}

// Depth 1: the symmetry-unique root moves scored exactly where known, otherwise by the evaluation, best first.
inline std::vector<RootResult> rootMovesByEval(const Pos& root, AbSearcher& S) {
    std::vector<Move> mv = uniqueRootMoves(root);
    int n = (int)mv.size();
    std::vector<RootResult> cur(n);
    for (int i = 0; i < n; i++) {
        Pos c = root; c.play(mv[i]);
        int v;
        if (c.over()) v = -terminalScore(c);
        else if (S.exactKnown(c, v)) v = -v;
        else v = -S.rootEval(c);
        cur[i].mv = mv[i]; cur[i].score = v; cur[i].exact = true; cur[i].depth = 1; cur[i].display = v;
    }
    std::stable_sort(cur.begin(), cur.end(), [](auto& a, auto& b) { return a.score > b.score; });
    return cur;
}

inline AbAnalysis abAnalyze(const Pos& root, double seconds, int threads, int multipv = 1, int maxDepth = 64,
                            AbEngine& E = abEngine()) {
    using clk = std::chrono::steady_clock;
    const AbParams& P = E.P;
    auto t0 = clk::now();
    auto deadline = t0 + std::chrono::microseconds((long long)(seconds * 1e6));
    std::atomic<bool> stop{false};
    auto& pool = E.pool;  // reused between calls (TT warm)
    while ((int)pool.size() < threads) pool.emplace_back(new AbSearcher(P));
    for (auto& S : pool) S->net = E.net, S->policy = E.policy;

    AbAnalysis res;
    res.moves = rootMovesByEval(root, *pool[0]);
    res.depth = 1;
    int n = (int)res.moves.size();
    if (n == 1) return res;
    std::vector<Move> evalOrder(n);
    for (int i = 0; i < n; i++) evalOrder[i] = res.moves[i].mv;

    // exact root verdicts after the heuristic search; when the solver is fast, also a first exact
    // phase in 1-ply order before it
    int rem = pieceCount(root.avail[0]) + pieceCount(root.avail[1]);
    bool exactOn = P.exactRem && rem <= P.exactRem;
    bool exactFirst = exactOn && rem <= P.exactFirstRem;
    if (exactFirst) {
        res.nodes += exactVerdicts(root, res.moves, t0 + std::chrono::microseconds((long long)(seconds * P.exactFirstShare * 1e6)), threads, E);
        lostMovesLast(res.moves);
    }
    // the heuristic search: all the time, or its share of what is left when an exact phase follows
    auto heurStart = exactOn ? clk::now() : t0;
    double left = seconds - std::chrono::duration<double>(heurStart - t0).count();
    double heurSeconds = exactOn ? left * P.heuristicShare : seconds;
    auto heurDeadline = heurStart + std::chrono::microseconds((long long)(heurSeconds * 1e6));
    for (int i = 0; i < threads; i++) { pool[i]->stop = &stop; pool[i]->deadline = heurDeadline; pool[i]->aborted = false; pool[i]->nodes = 0; }

    for (int depth = 2; depth <= maxDepth && !verdictsSettle(res.moves); depth++) {
        double el = std::chrono::duration<double>(clk::now() - heurStart).count();
        if (el > heurSeconds * P.newIteration) break;
        std::vector<RootResult> next = res.moves;
        for (auto& r : next) if (r.verdict != Verdict::Loss) { r.exact = false; r.depth = depth; }
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
            if (next[i].verdict == Verdict::Loss) { done[i] = 1; return; }  // proven: keeps its score
            Pos c = root; c.play(next[i].mv);
            int a = threshold();
            int v;
            if (a == -INF) v = -S.searchRoot(c, depth - 1, -INF, INF);
            else {
                v = -S.searchRoot(c, depth - 1, -a - 1, -a);
                if (!S.aborted && v > a) v = -S.searchRoot(c, depth - 1, -INF, -a);
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
        if (!complete && !P.partialIterations) break;
        // a cut-off iteration ranks the moves it finished (the previous best is always among them)
        // before the others, which keep the previous iteration's results
        for (int i = 0; i < n; i++) if (!done[i]) next[i] = res.moves[i];
        std::stable_sort(next.begin(), next.end(), [](auto& a, auto& b) {
            if (a.depth != b.depth) return a.depth > b.depth;
            if (a.exact != b.exact) return a.exact;
            return a.score > b.score;
        });
        // odd/even smoothing for display: average with the previous iteration where both are exact
        for (auto& r : next) {
            if (r.depth != depth) continue;
            r.display = r.score;
            if (!r.exact || std::abs(r.score) >= WIN / 2) continue;
            for (auto& p : res.moves)
                if (p.mv == r.mv) {
                    if (p.exact && std::abs(p.score) < WIN / 2) r.display = (r.score + p.score) / 2;
                    break;
                }
        }
        // the value of the position (best move) is smoothed with the previous iteration's
        // best value even when the best move changed
        {
            const RootResult& pb = res.moves[0];
            RootResult& nb = next[0];
            if (nb.exact && pb.exact && std::abs(nb.score) < WIN / 2 && std::abs(pb.score) < WIN / 2)
                nb.display = (nb.score + pb.score) / 2;
        }
        res.moves = next;
        res.depth = depth;
        if (!complete) break;
        if (next[0].exact && next[0].score >= WIN / 2) break;  // proven win found
        bool allLost = true;
        for (auto& r : next) if (r.score > -WIN / 2) allLost = false;
        if (allLost) break;
    }
    for (int i = 0; i < threads; i++) res.nodes += pool[i]->nodes;
    if (exactOn) {
        // candidates for the exact phase alternate between the search's and the 1-ply evaluation's
        // favourites
        interleave(res.moves, evalOrder);
        lostMovesLast(res.moves);
        if (!verdictsSettle(res.moves) && clk::now() < deadline) res.nodes += exactVerdicts(root, res.moves, deadline, threads, E);
        rankByVerdict(root, res.moves, E);
    }
    return res;
}
