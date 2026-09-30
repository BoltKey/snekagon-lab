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
#include "eval.hpp"
#include "mcts.hpp"
#include "search.hpp"
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
        a.items.push_back({m.mv, m.exact ? 1 : 0, scoreToWinProb(m.score), ex, m.score});
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
        } else if (cmd == "engine") {
            std::string e;
            in >> e;
            if (e == "ab" || e == "mcts") g_engine = e;
            out = "{\"ok\":true,\"engine\":\"" + g_engine + "\"}";
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
    } else if (mode == "verify") {
        // fast generator == slow per-placement check (same set, same order);
        // snake bounds hold for the final result of every random game.
        int games = argc > 2 ? std::atoi(argv[2]) : 2000;
        Rng rng(99);
        long positions = 0, boundChecks = 0;
        std::vector<Move> fast, slow;
        for (int g = 0; g < games; g++) {
            Pos s = Pos::start();
            std::vector<Pos> hist;
            while (!s.over()) {
                s.genMoves(fast);
                slow.clear();
                uint8_t av = s.movable();
                for (int p = 0; p < NPIECE; p++)
                    if (av >> p & 1)
                        for (auto& pl : T.PL[p]) if (s.legal(pl)) slow.push_back(Move{(int16_t)pl.gid});
                if (fast.size() != slow.size() || !std::equal(fast.begin(), fast.end(), slow.begin())) {
                    printf("MOVEGEN MISMATCH game %d ply %d: fast %zu slow %zu\n", g, (int)s.ply, fast.size(), slow.size());
                    return 1;
                }
                if (s.hasMove() != !fast.empty()) { printf("HASMOVE MISMATCH\n"); return 1; }
                positions++;
                hist.push_back(s);
                // mix uniformly random and "grow own snake" moves so games get long snakes
                Move m{};
                if (!fast.empty()) {
                    std::vector<Move> grow;
                    for (Move x : fast) if (x.pl().nb & s.own[s.stm]) grow.push_back(x);
                    if (!grow.empty() && rng.below(3)) m = grow[rng.below((int)grow.size())];
                    else m = fast[rng.below((int)fast.size())];
                }
                s.play(m);
            }
            int fin[2] = {longest(s.own[0]), longest(s.own[1])};
            for (auto& h : hist)
                for (int c = 0; c < 2; c++) {
                    int lb, ub;
                    snakeBounds(h, c, lb, ub);
                    boundChecks++;
                    if (fin[c] < lb || fin[c] > ub) {
                        printf("BOUND VIOLATION game %d ply %d player %d: final %d, bounds [%d,%d]\n", g, (int)h.ply, c, fin[c], lb, ub);
                        return 1;
                    }
                }
        }
        printf("ok: %ld positions movegen identical, %ld bound checks hold\n", positions, boundChecks);
    } else if (mode == "gendata") {
        // snek gendata <games> <plyMin> <plyMax> <solveSecs> <threads> <seed>
        // Plays softmax-greedy games (1-ply eval, some random moves), solves the
        // positions in [plyMin, plyMax] exactly and prints features + result (CSV).
        int games = argc > 2 ? std::atoi(argv[2]) : 100;
        int plyMin = argc > 3 ? std::atoi(argv[3]) : 7;
        int plyMax = argc > 4 ? std::atoi(argv[4]) : 10;
        double secs = argc > 5 ? std::atof(argv[5]) : 5;
        int threads = argc > 6 ? std::atoi(argv[6]) : 8;
        uint64_t seed = argc > 7 ? std::atoll(argv[7]) : 7;
        bool goodPlay = argc > 8 && std::string(argv[8]) == "good";
        std::atomic<int> next{0};
        std::mutex mu;
        printf("game,ply,stm,value");
        const char* names[] = {"L", "rem", "pieces", "terr", "bestTerr", "bestOwn", "lb", "ub", "fresh", "ends", "liveEnds", "singleOwn", "singleTerr", "comps"};
        for (const char* side : {"s", "o"}) for (const char* n : names) printf(",%s_%s", n, side);
        printf(",eval\n");
        std::vector<std::thread> th;
        for (int w = 0; w < threads; w++)
            th.emplace_back([&, w] {
                Solver solver(21);
                std::vector<Move> mv;
                for (;;) {
                    int g = next++;
                    if (g >= games) break;
                    Rng rng(seed * 1000003 + g);
                    Pos s = Pos::start();
                    double temp = 40 + rng.below(160);  // centi-cells
                    double randP = 0.05 + 0.2 * rng.uni();
                    bool growPolicy = g % 2 == 1;
                    if (goodPlay) {  // random first two plies, then (almost) greedy 1-ply play
                        temp = 10 + rng.below(30);
                        randP = 0;
                        growPolicy = false;
                    }
                    while (!s.over() && s.ply <= plyMax) {
                        if (s.ply >= plyMin) {
                            solver.clear();
                            int v = solver.solve(s, secs);
                            if (v != -9) {
                                PlayerFeat f[2];
                                features(s, f);
                                int e = evaluateHand(s);
                                std::lock_guard<std::mutex> lk(mu);
                                printf("%d,%d,%d,%d", g, (int)s.ply, (int)s.stm, v);
                                for (int c : {(int)s.stm, (int)s.stm ^ 1}) {
                                    auto& x = f[c];
                                    printf(",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", x.L, x.rem, x.pieces, x.terr, x.bestTerr, x.bestOwn, x.lb, x.ub, x.fresh, x.ends, x.liveEnds, x.singleOwn, x.singleTerr, x.comps);
                                }
                                printf(",%d\n", e);
                                fflush(stdout);
                            }
                        }
                        s.genMoves(mv);
                        if (mv.empty()) { s.play(Move{}); continue; }
                        if (rng.uni() < randP || s.ply == 0 || (goodPlay && s.ply == 1)) { s.play(mv[rng.below((int)mv.size())]); continue; }
                        if (growPolicy) {
                            // prefer growing an own snake (mostly the longest one), sometimes block
                            std::vector<Move> grow;
                            BB me = s.own[s.stm];
                            for (Move m : mv) if (m.pl().nb & me) grow.push_back(m);
                            if (!grow.empty() && rng.uni() < 0.85) { s.play(grow[rng.below((int)grow.size())]); continue; }
                            s.play(mv[rng.below((int)mv.size())]);
                            continue;
                        }
                        std::vector<double> sc(mv.size());
                        double mx = -1e18;
                        for (size_t i = 0; i < mv.size(); i++) {
                            Pos c = s; c.play(mv[i]);
                            sc[i] = c.over() ? -terminalScore(c) : -evaluate(c);
                            mx = std::max(mx, sc[i]);
                        }
                        double tot = 0;
                        for (auto& x : sc) { x = std::exp((x - mx) / temp); tot += x; }
                        double r = rng.uni() * tot;
                        size_t pick = 0;
                        for (; pick + 1 < sc.size(); pick++) { r -= sc[pick]; if (r <= 0) break; }
                        s.play(mv[pick]);
                    }
                }
            });
        for (auto& t : th) t.join();
    } else if (mode == "solvepos") {
        // snek solvepos <seconds> <threads> <move> <move> ...   (parallel exact solve)
        double secs = std::atof(argv[2]);
        int threads = std::atoi(argv[3]);
        Pos s = Pos::start();
        for (int i = 4; i < argc; i++) {
            Move m;
            if (!parseMove(argv[i], m) || !s.legalMove(m)) { printf("illegal move %s\n", argv[i]); return 1; }
            s.play(m);
        }
        auto r = parallelSolve(s, secs, threads);
        int known = 0;
        for (auto& pm : r.perMove) known += pm.second != -9;
        printf("ply %d stm %d: value %s  (%d/%zu root moves solved, %llu nodes, %.1fs)\n", (int)s.ply, (int)s.stm,
               r.value == -9 ? "unknown" : std::to_string(r.value).c_str(), known, r.perMove.size(), (unsigned long long)r.nodes, r.seconds);
        for (auto& pm : r.perMove)
            if (pm.second != -9) printf("  %-22s %s\n", moveStr(pm.first).c_str(), pm.second > 0 ? "win" : pm.second < 0 ? "loss" : "draw");
    } else if (mode == "evaltrace") {
        // snek evaltrace <maxDepth> <seconds> <threads> <moves...>
        // For every position of the game: static eval and fixed-depth search results,
        // all as red's (player 1's) win probability.
        int maxD = std::atoi(argv[2]);
        double secs = std::atof(argv[3]);
        int threads = std::atoi(argv[4]);
        Pos s = Pos::start();
        std::vector<Move> moves;
        for (int i = 5; i < argc; i++) { Move m; if (!parseMove(argv[i], m)) { printf("bad move %s\n", argv[i]); return 1; } moves.push_back(m); }
        printf("ply stm  static");
        for (int d = 1; d <= maxD; d++) printf("   d%d  ", d);
        printf("  move played\n");
        for (size_t k = 0; k <= moves.size() && !s.over(); k++) {
            auto red = [&](int score) { double p = scoreToWinProb(score); return s.stm == 0 ? p : 1 - p; };
            printf("%3d  %s  %5.1f%%", (int)s.ply, s.stm == 0 ? "R" : "B", 100 * red(evaluate(s)));
            if (s.ply > 0)
                for (int d = 1; d <= maxD; d++) {
                    AbAnalysis a = abAnalyze(s, secs, threads, 1, d);
                    printf("  %5.1f%%", 100 * red(a.moves[0].score));
                    if (a.depth < d) printf("(%d)", a.depth);
                }
            printf("   %s\n", k < moves.size() ? moveStr(moves[k]).c_str() : "");
            fflush(stdout);
            if (k < moves.size()) s.play(moves[k]);
        }
    } else if (mode == "speed") {
        // micro-benchmarks of the hot functions on positions from random games
        Rng rng(5);
        std::vector<Pos> ps;
        std::vector<Move> buf;
        for (int g = 0; g < 300; g++) {
            Pos s = Pos::start();
            while (!s.over()) { ps.push_back(s); s.genMoves(buf); s.play(buf.empty() ? Move{} : buf[rng.below((int)buf.size())]); }
        }
        auto bench = [&](const char* name, auto&& f) {
            auto t0 = std::chrono::steady_clock::now();
            long sink = 0;
            int reps = 20;
            for (int r = 0; r < reps; r++) for (auto& p : ps) sink += f(p);
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("%-14s %8.3f us/call   (sink %ld)\n", name, el / (reps * ps.size()) * 1e6, sink);
        };
        bench("genMoves", [&](const Pos& p) { p.genMoves(buf); return (long)buf.size(); });
        bench("hasMove", [&](const Pos& p) { return (long)p.hasMove(); });
        bench("snakeBounds", [&](const Pos& p) { int a, b; snakeBounds(p, 0, a, b); return (long)b; });
        bench("evalHand", [&](const Pos& p) { return (long)evaluateHand(p); });
        bench("evalLearned", [&](const Pos& p) { return (long)evaluate(p); });
    } else if (mode == "arena") {
        // engine A vs engine B from random openings (random 1-hex + random reply),
        // every opening played twice with colours swapped.
        int pairs = argc > 2 ? std::atoi(argv[2]) : 10;
        double secs = argc > 3 ? std::atof(argv[3]) : 1;
        int threads = argc > 4 ? std::atoi(argv[4]) : 8;
        std::string A = argc > 5 ? argv[5] : "ab", B = argc > 6 ? argv[6] : "mcts";
        uint64_t seed = argc > 7 ? std::atoll(argv[7]) : 2024;
        Solver solver(22);
        Rng rng(seed);
        int winA = 0, winB = 0, draws = 0;
        std::vector<Move> buf;
        for (int p = 0; p < pairs; p++) {
            Pos open = Pos::start();
            for (int k = 0; k < 2; k++) { open.genMoves(buf); open.play(buf[rng.below((int)buf.size())]); }
            for (int sw = 0; sw < 2; sw++) {
                std::string e[2] = {sw ? B : A, sw ? A : B};
                Pos s = open;
                std::string rec = moveStr(Move{}) ;
                rec.clear();
                while (!s.over()) {
                    Analysis a = analyze(s, secs, threads, solver, e[s.stm]);
                    rec += moveStr(a.items[0].mv) + " ";
                    s.play(a.items[0].mv);
                }
                int l0 = longest(s.own[0]), l1 = longest(s.own[1]);
                const char* res = l0 > l1 ? "P1" : l0 < l1 ? "P2" : "draw";
                if (l0 == l1) draws++;
                else if ((l0 > l1) == (sw == 0)) winA++;
                else winB++;
                printf("pair %d  P1=%s P2=%s  %d-%d  %s   | A %d  B %d  draws %d\n", p, e[0].c_str(), e[1].c_str(), l0, l1, res, winA, winB, draws);
                printf("   %s\n", rec.c_str());
                fflush(stdout);
            }
        }
        printf("FINAL %s %d : %s %d  (draws %d)\n", A.c_str(), winA, B.c_str(), winB, draws);
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
