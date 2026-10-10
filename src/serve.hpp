// The `snek serve` line protocol: one command in, one JSON line out. Shared by the CLI (`snek serve`,
// driven by ui/server.js) and the WebAssembly build (src/web.cpp, driven by ui/snek-worker.js).
#pragma once
#include <sstream>
#include <string>
#include <vector>
#include "core.hpp"
#include "eval.hpp"
#include "mcts.hpp"
#include "search.hpp"
#include "solver.hpp"

static std::string cellsJson(BB b) {
    std::string s = "[";
    bool first = true;
    while (b) { int i = lsb(b); b &= b - 1; if (!first) s += ','; s += std::to_string(i); first = false; }
    return s + "]";
}
static std::string moveJson(Move m) {
    if (m.isPass()) return "{\"p\":\"pass\",\"c\":[],\"s\":\"pass\"}";
    return std::string("{\"p\":\"") + PNAME[m.piece()] + "\",\"c\":" + cellsJson(m.pl().m) + ",\"s\":\"" + moveStr(m) + "\"}";
}
static std::string availJson(uint8_t av) {
    std::string s = "[";
    bool first = true;
    for (int p = 0; p < NPIECE; p++) if (av >> p & 1) { if (!first) s += ','; s += std::string("\"") + PNAME[p] + "\""; first = false; }
    return s + "]";
}

// ---- analysis used by the UI / arena ------------------------------------------
struct Analysis {
    bool exact = false;
    int value = 0;  // exact value for side to move (when exact)
    int depth = 0;
    std::string engine;
    // exact: -9 unknown, else value for mover; score: centi-cells (ab) ; w: win prob for mover
    struct Item { Move mv; int n; double w; int exact; int score; };
    std::vector<Item> items;
};

// Solve every root move exactly (only attempted when few pieces are left).
static bool solveAllRoot(const Pos& s, double budget, Solver& solver, Analysis& a) {
    int rem = pieceCount(s.avail[0]) + pieceCount(s.avail[1]);
    if (rem > 8) return false;
    std::vector<Move> mv = uniqueRootMoves(s);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<int> ex(mv.size(), -9);
    for (size_t i = 0; i < mv.size(); i++) {
        double left = budget - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (left <= 0) return false;
        Pos c = s; c.play(mv[i]);
        int v = solver.solve(c, left);
        if (v == -9) return false;
        ex[i] = -v;
    }
    a.exact = true;
    a.value = -2;
    for (size_t i = 0; i < mv.size(); i++) {
        a.items.push_back({mv[i], 0, ex[i] > 0 ? 1.0 : ex[i] < 0 ? 0.0 : 0.5, ex[i], ex[i] * WIN});
        a.value = std::max(a.value, ex[i]);
    }
    // among equal exact values prefer moves the evaluation likes
    std::vector<int> ev(a.items.size());
    for (size_t i = 0; i < a.items.size(); i++) { Pos c = s; c.play(a.items[i].mv); ev[i] = c.over() ? -terminalScore(c) : -evaluate(c); }
    std::vector<size_t> order(a.items.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (a.items[x].exact != a.items[y].exact) return a.items[x].exact > a.items[y].exact;
        return ev[x] > ev[y];
    });
    std::vector<Analysis::Item> sorted;
    for (size_t i : order) sorted.push_back(a.items[i]);
    a.items = sorted;
    a.engine = "exact";
    return true;
}

static Analysis analyzeMcts(const Pos& s, double seconds, int threads, Solver& solver) {
    Analysis a;
    auto t0 = std::chrono::steady_clock::now();
    if (solveAllRoot(s, seconds * 0.5, solver, a)) return a;
    double left = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto stats = mctsAnalyze(s, std::max(0.05, left), threads);
    for (auto& st : stats) {
        double wr = st.n ? st.w / st.n : 0.5;
        a.items.push_back({st.mv, st.n, wr, -9, 0});
    }
    a.engine = "mcts";
    return a;
}

static Analysis analyzeAb(const Pos& s, double seconds, int threads, Solver& solver, int multipv) {
    Analysis a;
    auto t0 = std::chrono::steady_clock::now();
    if (solveAllRoot(s, seconds * 0.3, solver, a)) return a;
    double left = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    AbAnalysis r = abAnalyze(s, std::max(0.05, left), threads, multipv);
    for (auto& m : r.moves) {
        int ex = -9;
        if (m.exact && m.score >= WIN / 2) ex = 1;
        else if (m.score <= -WIN / 2) ex = -1;
        a.items.push_back({m.mv, m.exact ? 1 : 0, scoreToWinProb(m.exact ? m.display : m.score), ex, m.score});
    }
    a.depth = r.depth;
    a.engine = "ab";
    return a;
}

static std::string g_engine = "ab";
static Analysis analyze(const Pos& s, double seconds, int threads, Solver& solver, const std::string& engine = g_engine, int multipv = 1) {
    if (engine == "mcts") return analyzeMcts(s, seconds, threads, solver);
    g_handEval = engine == "abhand";
    g_evalSet = 0;
    if (engine.rfind("ab:", 0) == 0) {  // "ab:<weights file>" uses runtime-loaded weights
        static std::string loaded;
        if (loaded != engine) {
            if (!loadWeights(engine.substr(3), 1)) { fprintf(stderr, "cannot load weights %s\n", engine.c_str() + 3); exit(1); }
            loaded = engine;
        }
        g_evalSet = 1;
    }
    Analysis a = analyzeAb(s, seconds, threads, solver, multipv);
    g_handEval = false;
    g_evalSet = 0;
    return a;
}

class ServeSession {
public:
    explicit ServeSession(int threads) : threads(threads), solver(22), s(Pos::start()) {}

    // The answer to one command line, or "" for "quit".
    std::string handle(std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        std::string out;
        if (cmd == "new") {
            replay({});
            out = stateJson();
        } else if (cmd == "set" || cmd == "play") {
            std::vector<Move> h = cmd == "play" ? history : std::vector<Move>{};
            std::string tok;
            bool ok = true;
            while (in >> tok) { Move m; if (!parseMove(tok, m)) { ok = false; break; } h.push_back(m); }
            auto backup = history;
            if (!ok || !replay(h)) { replay(backup); out = "{\"ok\":false,\"error\":\"illegal move\"}"; }
            else out = stateJson();
        } else if (cmd == "undo") {
            auto h = history;
            if (!h.empty()) h.pop_back();
            replay(h);
            out = stateJson();
        } else if (cmd == "state") {
            out = stateJson();
        } else if (cmd == "analyze" || cmd == "go") {
            double sec = 2;
            in >> sec;
            if (s.over()) { out = "{\"ok\":false,\"error\":\"game over\"}"; }
            else {
                Analysis a = analyze(s, sec, threads, solver, g_engine, cmd == "go" ? 1 : 8);
                std::ostringstream o;
                o << "{\"ok\":true,\"exact\":" << (a.exact ? "true" : "false") << ",\"value\":" << a.value
                  << ",\"stm\":" << (int)s.stm << ",\"engine\":\"" << a.engine << "\",\"depth\":" << a.depth << ",\"moves\":[";
                size_t lim = std::min<size_t>(a.items.size(), 40);
                for (size_t i = 0; i < lim; i++) {
                    auto& it = a.items[i];
                    o << (i ? "," : "") << "{\"m\":" << moveJson(it.mv) << ",\"n\":" << it.n << ",\"wr\":" << it.w << ",\"exact\":" << it.exact << ",\"score\":" << it.score << "}";
                }
                o << "]}";
                out = o.str();
                if (cmd == "go" && !a.items.empty()) {
                    history.push_back(a.items[0].mv);
                    replay(history);
                    out = "{\"ok\":true,\"played\":" + moveJson(a.items[0].mv) + ",\"analysis\":" + out + ",\"state\":" + stateJson() + "}";
                }
            }
        } else if (cmd == "solve") {
            double sec = 10;
            in >> sec;
            if (s.over()) { out = "{\"ok\":false,\"error\":\"game over\"}"; }
            else {
                auto r = parallelSolve(s, sec, threads, 21);
                std::ostringstream o;
                o << "{\"ok\":true,\"stm\":" << (int)s.stm << ",\"value\":" << r.value << ",\"nodes\":" << r.nodes
                  << ",\"seconds\":" << r.seconds << ",\"moves\":[";
                std::stable_sort(r.perMove.begin(), r.perMove.end(), [](auto& a, auto& b) {
                    auto key = [](int v) { return v == -9 ? -0.5 : (double)v; };
                    return key(a.second) > key(b.second);
                });
                size_t lim = std::min<size_t>(r.perMove.size(), 60);
                for (size_t i = 0; i < lim; i++)
                    o << (i ? "," : "") << "{\"m\":" << moveJson(r.perMove[i].first) << ",\"v\":" << r.perMove[i].second << "}";
                o << "]}";
                out = o.str();
            }
        } else if (cmd == "children") {
            // children <moves...>: symmetry-unique legal moves of that position, best first by the
            // learned evaluation (from the mover's point of view); used by the coordinator
            Pos p = Pos::start();
            std::string tok;
            bool ok = true;
            while (in >> tok) { Move m; if (!parseMove(tok, m) || !p.legalMove(m)) { ok = false; break; } p.play(m); }
            if (!ok) out = "{\"ok\":false,\"error\":\"illegal move\"}";
            else {
                std::vector<Move> mv = uniqueRootMoves(p);
                std::vector<std::pair<double, Move>> sc;
                for (Move m : mv) {
                    Pos c = p; c.play(m);
                    double e = c.over() ? -Solver::terminalValue(c) * 100.0 : -learnedLogit(c);
                    sc.push_back({e, m});
                }
                std::stable_sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.first > b.first; });
                std::ostringstream o;
                o << "{\"ok\":true,\"ply\":" << (int)p.ply << ",\"stm\":" << (int)p.stm << ",\"over\":" << (p.over() ? "true" : "false") << ",\"moves\":[";
                for (size_t i = 0; i < sc.size(); i++) o << (i ? "," : "") << "{\"s\":\"" << moveStr(sc[i].second) << "\",\"e\":" << sc[i].first << "}";
                o << "]}";
                out = o.str();
            }
        } else if (cmd == "engine") {
            std::string e;
            in >> e;
            if (e == "ab" || e == "mcts") g_engine = e;
            out = "{\"ok\":true,\"engine\":\"" + g_engine + "\"}";
        } else if (cmd == "quit") {
            return "";
        } else {
            out = "{\"ok\":false,\"error\":\"unknown command\"}";
        }
        return out;
    }

private:
    int threads;
    Solver solver;
    Pos s;
    std::vector<Move> history;

    std::string stateJson() {
        std::ostringstream o;
        std::vector<Move> mv;
        if (!s.over()) s.genMoves(mv);
        int l1 = longest(s.own[0]), l2 = longest(s.own[1]);
        o << "{\"ok\":true,\"ply\":" << (int)s.ply << ",\"stm\":" << (int)s.stm << ",\"over\":" << (s.over() ? "true" : "false")
          << ",\"own\":[" << cellsJson(s.own[0]) << "," << cellsJson(s.own[1]) << "]"
          << ",\"avail\":[" << availJson(s.avail[0]) << "," << availJson(s.avail[1]) << "]"
          << ",\"longest\":[" << l1 << "," << l2 << "]";
        int lb0, ub0, lb1, ub1;
        snakeBounds(s, 0, lb0, ub0); snakeBounds(s, 1, lb1, ub1);
        o << ",\"bounds\":[[" << lb0 << "," << ub0 << "],[" << lb1 << "," << ub1 << "]]";
        o << ",\"history\":[";
        for (size_t i = 0; i < history.size(); i++) o << (i ? "," : "") << moveJson(history[i]);
        o << "],\"legal\":[";
        for (size_t i = 0; i < mv.size(); i++) o << (i ? "," : "") << moveJson(mv[i]);
        o << "]}";
        return o.str();
    }
    bool replay(std::vector<Move> h) {
        s = Pos::start();
        history.clear();
        for (Move m : h) {
            if (s.over()) return false;
            if (!m.isPass() && !s.legal(m.pl())) return false;
            if (!m.isPass() && s.ply == 0 && m.piece() != P1) return false;
            if (!m.isPass() && !(s.avail[s.stm] >> m.piece() & 1)) return false;
            if (m.isPass() && s.hasMove()) return false;
            s.play(m);
            history.push_back(m);
        }
        return true;
    }
};
