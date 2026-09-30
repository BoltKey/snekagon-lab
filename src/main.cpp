// snek - Snekagon engine CLI.
//   snek serve                 line protocol for the UI (see README)
//   snek perft <depth>         count move sequences (for cross-checking the rules)
//   snek solvebench <plies> <count> [seconds] [threads]
//                              solve random positions after <plies> random moves
//   snek selfplay <games> <seconds/move>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include "core.hpp"
#include "mcts.hpp"
#include "solver.hpp"

static uint64_t perft(const Pos& s, int d) {
    if (d == 0 || s.over()) return 1;
    std::vector<Move> mv;
    s.genMoves(mv);
    if (mv.empty()) { Pos c = s; c.play(Move{}); return perft(c, d - 1); }
    if (d == 1) return mv.size();
    uint64_t n = 0;
    for (Move m : mv) { Pos c = s; c.play(m); n += perft(c, d - 1); }
    return n;
}

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

static Pos randomPosition(int plies, Rng& rng) {
    for (;;) {
        Pos s = Pos::start();
        std::vector<Move> buf;
        for (int i = 0; i < plies && !s.over(); i++) {
            s.genMoves(buf);
            if (buf.empty()) s.play(Move{});
            else s.play(buf[rng.below((int)buf.size())]);
        }
        if (!s.over()) return s;
    }
}

// ---- analysis used by the UI ---------------------------------------------------
struct Analysis {
    bool exact = false;
    int value = 0;  // exact value for side to move
    struct Item { Move mv; int n; double w; int exact; };  // exact: -9 unknown, else value for mover
    std::vector<Item> items;
};

static Analysis analyze(const Pos& s, double seconds, int threads, Solver& solver) {
    Analysis a;
    std::vector<Move> mv;
    s.genMoves(mv);
    if (mv.empty()) mv.push_back(Move{});
    // 1) try to solve every root move exactly within ~half the budget
    auto t0 = std::chrono::steady_clock::now();
    double solveBudget = seconds * 0.5;
    std::vector<int> ex(mv.size(), -9);
    bool allSolved = true;
    if (mv.size() <= 400) {
        for (size_t i = 0; i < mv.size(); i++) {
            double left = solveBudget - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) { allSolved = false; break; }
            Pos c = s; c.play(mv[i]);
            int v = solver.solve(c, left);
            if (v == -9) { allSolved = false; break; }
            ex[i] = -v;
        }
    } else allSolved = false;
    if (allSolved) {
        a.exact = true;
        a.value = -2;
        for (size_t i = 0; i < mv.size(); i++) {
            a.items.push_back({mv[i], 0, 0, ex[i]});
            a.value = std::max(a.value, ex[i]);
        }
        std::stable_sort(a.items.begin(), a.items.end(), [](auto& x, auto& y) { return x.exact > y.exact; });
        return a;
    }
    double left = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto stats = mctsAnalyze(s, std::max(0.05, left), threads);
    for (auto& st : stats) {
        int e = -9;
        for (size_t i = 0; i < mv.size(); i++) if (mv[i] == st.mv) e = ex[i];
        double wr = st.n ? st.w / st.n : 0.5;
        a.items.push_back({st.mv, st.n, wr, e});
    }
    // proven wins first, proven losses last
    std::stable_sort(a.items.begin(), a.items.end(), [](auto& x, auto& y) {
        auto key = [](const Analysis::Item& it) { return it.exact == 1 ? 2 : it.exact == -1 ? 0 : 1; };
        return key(x) > key(y);
    });
    return a;
}

static void serve(int threads) {
    Solver solver(22);
    Pos s = Pos::start();
    std::vector<Move> history;
    std::string line;
    auto stateJson = [&]() {
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
    };
    auto replay = [&](std::vector<Move> h) {
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
    };
    while (std::getline(std::cin, line)) {
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
                Analysis a = analyze(s, sec, threads, solver);
                std::ostringstream o;
                o << "{\"ok\":true,\"exact\":" << (a.exact ? "true" : "false") << ",\"value\":" << a.value
                  << ",\"stm\":" << (int)s.stm << ",\"moves\":[";
                size_t lim = std::min<size_t>(a.items.size(), 40);
                for (size_t i = 0; i < lim; i++) {
                    auto& it = a.items[i];
                    o << (i ? "," : "") << "{\"m\":" << moveJson(it.mv) << ",\"n\":" << it.n << ",\"wr\":" << it.w << ",\"exact\":" << it.exact << "}";
                }
                o << "]}";
                out = o.str();
                if (cmd == "go" && !a.items.empty()) {
                    history.push_back(a.items[0].mv);
                    replay(history);
                    out = "{\"ok\":true,\"played\":" + moveJson(a.items[0].mv) + ",\"analysis\":" + out + ",\"state\":" + stateJson() + "}";
                }
            }
        } else if (cmd == "quit") {
            break;
        } else {
            out = "{\"ok\":false,\"error\":\"unknown command\"}";
        }
        std::cout << out << "\n" << std::flush;
    }
}

int main(int argc, char** argv) {
    initTables();
    std::string mode = argc > 1 ? argv[1] : "serve";
    if (mode == "perft") {
        int d = argc > 2 ? std::atoi(argv[2]) : 3;
        Pos s = Pos::start();
        for (int i = 1; i <= d; i++) {
            auto t0 = std::chrono::steady_clock::now();
            uint64_t n = perft(s, i);
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("perft %d = %llu  (%.2fs)\n", i, (unsigned long long)n, el);
            fflush(stdout);
        }
    } else if (mode == "info") {
        for (int p = 0; p < NPIECE; p++) printf("%s: %zu placements\n", PNAME[p], T.PL[p].size());
        printf("total placements %d\n", T.NGID);
    } else if (mode == "solvebench") {
        int plies = argc > 2 ? std::atoi(argv[2]) : 10;
        int count = argc > 3 ? std::atoi(argv[3]) : 10;
        double secs = argc > 4 ? std::atof(argv[4]) : 60;
        int threads = argc > 5 ? std::atoi(argv[5]) : 1;
        std::atomic<int> next{0};
        std::mutex mu;
        std::vector<std::thread> th;
        for (int w = 0; w < threads; w++)
            th.emplace_back([&, w] {
                Solver solver(21);
                for (;;) {
                    int i = next++;
                    if (i >= count) break;
                    Rng rng(1000 + plies * 100000 + i);
                    Pos s = randomPosition(plies, rng);
                    solver.clear();
                    auto t0 = std::chrono::steady_clock::now();
                    int v = solver.solve(s, secs);
                    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::lock_guard<std::mutex> lk(mu);
                    printf("plies=%d pos=%d value(stm)=%s nodes=%llu time=%.3fs knps=%.0f\n", plies, i,
                           v == -9 ? "timeout" : std::to_string(v).c_str(), (unsigned long long)solver.nodes, el,
                           solver.nodes / el / 1000);
                    fflush(stdout);
                }
            });
        for (auto& t : th) t.join();
    } else if (mode == "selfplay") {
        int games = argc > 2 ? std::atoi(argv[2]) : 1;
        double secs = argc > 3 ? std::atof(argv[3]) : 1;
        int threads = argc > 4 ? std::atoi(argv[4]) : 8;
        Solver solver(22);
        for (int g = 0; g < games; g++) {
            Pos s = Pos::start();
            std::string rec;
            while (!s.over()) {
                Analysis a = analyze(s, secs, threads, solver);
                Move m = a.items[0].mv;
                rec += moveStr(m) + " ";
                printf("ply %d stm %d: %s  (%s %s)\n", (int)s.ply, (int)s.stm, moveStr(m).c_str(),
                       a.exact ? "exact" : "mcts",
                       a.exact ? std::to_string(a.value).c_str() : std::to_string(a.items[0].w).c_str());
                fflush(stdout);
                s.play(m);
            }
            printf("game %d: P1 %d  P2 %d\nmoves: %s\n", g, longest(s.own[0]), longest(s.own[1]), rec.c_str());
        }
    } else if (mode == "serve") {
        int threads = argc > 2 ? std::atoi(argv[2]) : std::max(1u, std::thread::hardware_concurrency() - 2);
        serve(threads);
    }
    return 0;
}
