// snek - Snekagon engine CLI.
//   snek serve                 line protocol for the UI (see README)
//   snek perft <depth>         count move sequences (for cross-checking the rules)
//   snek solvebench <plies> <count> [seconds] [threads]
//                              solve random positions after <plies> random moves
//   snek selfplay <games> <seconds/move>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include "core.hpp"
#include "eval.hpp"
#include "mcts.hpp"
#include "search.hpp"
#include "solver.hpp"
#include "serve.hpp"
#include "worker.hpp"

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

// Reference implementation of snakeBounds (floods over own cells + growable cells), used by
// `verify` to check the fast version.
static void snakeBoundsRef(const Pos& s, int c, int& lb, int& ub) {
    BB me = s.own[c];
    lb = longest(me);
    uint8_t av = s.avail[c];
    if (!av) { ub = lb; return; }
    Shape sh = shapeOf(me);
    BB G = s.empty() & ~expand(sh.full);
    BB U = me | G;
    ub = lb;
    while (U) {
        BB K = flood(bit(lsb(U)), U);
        U &= ~K;
        BB g = K & G;
        if (!g) continue;
        int cap = 0;
        while (g) { BB r = flood(bit(lsb(g)), g); g &= ~r; cap += T.fit[av][popc(r)]; }
        ub = std::max(ub, popc(K & me) + (int)T.fit[av][std::min(cap, NIDX)]);
    }
}

// ---- training data rows ----------------------------------------------------------
static std::string hex128(BB b) {
    char buf[40];
    snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)(uint64_t)(b >> 64), (unsigned long long)(uint64_t)b);
    return buf;
}
static BB parseHex128(const std::string& h) {
    BB r = 0;
    for (char c : h) r = r * 16 + (BB)(c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10);
    return r;
}
// own0_own1_avail0_avail1_stm_ply_passes
static std::string posStr(const Pos& s) {
    return hex128(s.own[0]) + "_" + hex128(s.own[1]) + "_" + std::to_string(s.avail[0]) + "_" + std::to_string(s.avail[1]) +
           "_" + std::to_string(s.stm) + "_" + std::to_string(s.ply) + "_" + std::to_string(s.passes);
}
static bool parsePosStr(const std::string& str, Pos& s) {
    std::vector<std::string> f;
    size_t i = 0;
    while (i <= str.size()) {
        size_t j = str.find('_', i);
        if (j == std::string::npos) j = str.size();
        f.push_back(str.substr(i, j - i));
        i = j + 1;
    }
    if (f.size() != 7) return false;
    s = Pos{};
    s.own[0] = parseHex128(f[0]); s.own[1] = parseHex128(f[1]);
    s.avail[0] = (uint8_t)std::stoi(f[2]); s.avail[1] = (uint8_t)std::stoi(f[3]);
    s.stm = (uint8_t)std::stoi(f[4]); s.ply = (uint8_t)std::stoi(f[5]); s.passes = (uint8_t)std::stoi(f[6]);
    s.initHash();
    return true;
}
static void printDataHeader() {
    printf("game,ply,stm,value");
    const char* names[] = {"L", "rem", "pieces", "terr", "bestTerr", "bestOwn", "lb", "ub", "fresh", "ends", "liveEnds", "singleOwn", "singleTerr", "comps"};
    for (const char* side : {"s", "o"}) for (const char* n : names) printf(",%s_%s", n, side);
    printf(",eval,pos\n");
}
static std::string dataRow(int g, const Pos& s, int v, int head) {
    PlayerFeat f[2];
    features(s, f, head);
    char buf[512];
    std::string row;
    snprintf(buf, sizeof buf, "%d,%d,%d,%d", g, (int)s.ply, (int)s.stm, v);
    row += buf;
    for (int c : {(int)s.stm, (int)s.stm ^ 1}) {
        auto& x = f[c];
        snprintf(buf, sizeof buf, ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", x.L, x.rem, x.pieces, x.terr, x.bestTerr, x.bestOwn, x.lb, x.ub, x.fresh, x.ends, x.liveEnds, x.singleOwn, x.singleTerr, x.comps);
        row += buf;
    }
    snprintf(buf, sizeof buf, ",%d,", evaluateHand(s));
    row += buf;
    row += posStr(s) + "\n";
    return row;
}

static void serve(int threads) {
    ServeSession session(threads);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string out = session.handle(line);
        if (out.empty()) break;  // quit
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
                for (int c = 0; c < 2; c++) {
                    // incremental snakes == recomputed; fast bounds == reference (flood) bounds
                    if (s.L[c] != longest(s.own[c])) { printf("LONGEST MISMATCH\n"); return 1; }
                    BB u = 0;
                    for (int j = 0; j < s.ncomp[c]; j++) u |= s.comp[c][j];
                    if (u != s.own[c]) { printf("COMPONENT MISMATCH\n"); return 1; }
                    int lb, ub, rlb, rub;
                    snakeBounds(s, c, lb, ub);
                    snakeBoundsRef(s, c, rlb, rub);
                    if (lb != rlb || ub != rub) { printf("BOUNDS MISMATCH game %d ply %d: [%d,%d] vs ref [%d,%d]\n", g, (int)s.ply, lb, ub, rlb, rub); return 1; }
                }
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
        int featHead = argc > 9 ? std::atoi(argv[9]) : 0;  // Voronoi head start for the feature columns
        std::atomic<int> next{0};
        std::mutex mu;
        printDataHeader();
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
                                std::string row = dataRow(g, s, v, featHead);
                                std::lock_guard<std::mutex> lk(mu);
                                fputs(row.c_str(), stdout);
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
        // env PROGRESS=<file>: append every solved root move ("<move> <win|draw|loss> <nodes> <sec>")
        // and, if the file exists, skip the moves already recorded there (resume).
        std::vector<std::pair<Move, int>> prev;
        FILE* prog = nullptr;
        if (const char* pf = getenv("PROGRESS")) {
            std::ifstream in(pf);
            std::string ms, vs;
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line);
                Move m;
                if (ls >> ms >> vs && parseMove(ms, m)) prev.push_back({m, vs == "win" ? 1 : vs == "loss" ? -1 : 0});
            }
            if (!prev.empty()) printf("resuming: %zu root moves already solved\n", prev.size());
            prog = fopen(pf, "a");
        }
        printf("root moves: %zu  ply %d stm %d\n", uniqueRootMoves(s).size(), (int)s.ply, (int)s.stm);
        fflush(stdout);
        auto t0 = std::chrono::steady_clock::now();
        size_t done = prev.size();
        auto r = parallelSolve(s, secs, threads, 22, prev, [&](Move m, int v, uint64_t nodes, double sec) {
            done++;
            const char* vs = v > 0 ? "win" : v < 0 ? "loss" : "draw";
            if (prog) { fprintf(prog, "%s %s %llu %.1f\n", moveStr(m).c_str(), vs, (unsigned long long)nodes, sec); fflush(prog); }
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("[%6.0fs] %4zu solved  %-22s %-4s %6.1fs %12llu nodes\n", el, done, moveStr(m).c_str(), vs, sec, (unsigned long long)nodes);
            fflush(stdout);
        });
        if (prog) fclose(prog);
        int known = 0;
        for (auto& pm : r.perMove) known += pm.second != -9;
        printf("ply %d stm %d: value %s  (%d/%zu root moves solved, %llu nodes, %.1fs)\n", (int)s.ply, (int)s.stm,
               r.value == -9 ? "unknown" : std::to_string(r.value).c_str(), known, r.perMove.size(), (unsigned long long)r.nodes, r.seconds);
        for (auto& pm : r.perMove)
            if (pm.second != -9) printf("  %-22s %s\n", moveStr(pm.first).c_str(), pm.second > 0 ? "win" : pm.second < 0 ? "loss" : "draw");
    } else if (mode == "worker") {
        // snek worker <host[:port]> [threads] [name]
        if (argc < 3) { printf("usage: snek worker <coordinator-ip[:port]> [threads] [name]\n"); return 1; }
        int threads = argc > 3 ? std::atoi(argv[3]) : (int)std::max(1u, std::thread::hardware_concurrency());
        char hn[128] = "worker";
#ifdef _WIN32
        { DWORD n = sizeof hn; GetComputerNameA(hn, &n); }
#else
        gethostname(hn, sizeof hn);
#endif
        return runWorker(argv[2], threads, argc > 4 ? argv[4] : hn);
    } else if (mode == "benchset") {
        // snek benchset <file> <threads>: exact solve of every position in the file (one move
        // list per line). Threads take positions in order and keep their solver between
        // positions, like parallelSolve does. Prints per-position and total cost.
        std::ifstream in(argv[2]);
        int threads = argc > 3 ? std::atoi(argv[3]) : 8;
        std::vector<Pos> ps;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string tok;
            Pos s = Pos::start();
            bool ok = true, any = false;
            while (ls >> tok) { Move m; if (!parseMove(tok, m) || !s.legalMove(m)) { ok = false; break; } s.play(m); any = true; }
            if (ok && any) ps.push_back(s);
        }
        std::vector<int> val(ps.size());
        std::vector<uint64_t> nod(ps.size());
        std::vector<double> sec(ps.size());
        std::atomic<size_t> next{0};
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> th;
        std::mutex profMu;
        uint64_t profV[16] = {}, profT[16] = {}, profB[16] = {}, profE[16] = {};
        int sharedLog2 = envInt("SOLVER_SHARED", 0);
        std::shared_ptr<LTT> shared = sharedLog2 ? std::make_shared<LTT>(sharedLog2 - 1) : nullptr;
        for (int w = 0; w < threads; w++)
            th.emplace_back([&] {
                std::unique_ptr<Solver> SP(shared ? new Solver(shared) : new Solver(22));
                Solver& S = *SP;
                for (;;) {
                    size_t i = next++;
                    if (i >= ps.size()) break;
                    auto ts = std::chrono::steady_clock::now();
                    val[i] = S.solve(ps[i]);
                    nod[i] = S.nodes;
                    {
                        std::lock_guard<std::mutex> lk(profMu);
                        for (int p = 0; p < 16; p++) { profV[p] += S.pVisit[p]; profT[p] += S.pTT[p]; profB[p] += S.pBound[p]; profE[p] += S.pExp[p]; }
                        std::fill(S.pVisit, S.pVisit + 16, 0); std::fill(S.pTT, S.pTT + 16, 0);
                        std::fill(S.pBound, S.pBound + 16, 0); std::fill(S.pExp, S.pExp + 16, 0);
                    }
                    sec[i] = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
                }
            });
        for (auto& t : th) t.join();
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        uint64_t tn = 0;
        double ts = 0;
        for (size_t i = 0; i < ps.size(); i++) {
            printf("%3zu  value %2d  %12llu nodes  %7.1fs\n", i, val[i], (unsigned long long)nod[i], sec[i]);
            tn += nod[i]; ts += sec[i];
        }
        printf("TOTAL %zu positions: %llu nodes, %.1f thread-seconds, %.1f s wall (%d threads)\n", ps.size(), (unsigned long long)tn, ts, wall, threads);
        if (getenv("PROFILE")) {
            printf("ply     visited    tt-cut  bound-cut  expanded\n");
            for (int p = 0; p < 16; p++)
                if (profV[p]) printf("%3d %11llu %9.1f%% %9.1f%% %9.1f%%\n", p, (unsigned long long)profV[p], 100.0 * profT[p] / profV[p], 100.0 * profB[p] / profV[p], 100.0 * profE[p] / profV[p]);
        }
    } else if (mode == "evalexplain") {
        // snek evalexplain <moves...>: feature values and logit contributions for every position
        // (env WEIGHTS=<file> to use a weights file instead of the compiled-in set)
        if (getenv("WEIGHTS")) {
            if (!loadWeights(getenv("WEIGHTS"), 1)) return 1;
            for (int i = 0; i < NFEAT; i++) EW[0][i] = EW[1][i];
            EHEAD[0] = EHEAD[1];
        }
        Pos s = Pos::start();
        std::vector<Move> moves;
        for (int i = 2; i < argc; i++) { Move m; if (!parseMove(argv[i], m)) return 1; moves.push_back(m); }
        printf("%-12s", "feature");
        for (size_t k = 1; k <= moves.size(); k++) printf("  ply%-2zu(%s)", k, k % 2 ? "B" : "R");
        printf("\n");
        std::vector<std::array<double, NFEAT>> xs;
        std::vector<int> stms;
        for (size_t k = 0; k < moves.size(); k++) {
            s.play(moves[k]);
            double x[NFEAT];
            featureVector(s, x);
            std::array<double, NFEAT> a;
            for (int i = 0; i < NFEAT; i++) a[i] = x[i];
            xs.push_back(a);
            stms.push_back(s.stm);
        }
        for (int i = 0; i < NFEAT; i++) {
            printf("%-12s", FEAT_NAMES[i]);
            for (auto& a : xs) printf("  %4.0f %+5.2f", a[i], a[i] * EW[0][i]);
            printf("\n");
        }
        printf("%-12s", "logit(stm)");
        for (auto& a : xs) { double z = 0; for (int i = 0; i < NFEAT; i++) z += a[i] * EW[0][i]; printf("  %10.2f", z); }
        printf("\n%-12s", "red win %");
        for (size_t k = 0; k < xs.size(); k++) { double z = 0; for (int i = 0; i < NFEAT; i++) z += xs[k][i] * EW[0][i]; double p = 1 / (1 + std::exp(-z)); printf("  %9.1f%%", 100 * (stms[k] == 0 ? p : 1 - p)); }
        printf("\n");
    } else if (mode == "evaltrace") {
        // snek evaltrace <maxDepth> <seconds> <threads> <moves...>
        // For every position of the game: static eval and fixed-depth search results,
        // all as red's (player 1's) win probability.
        int maxD = std::atoi(argv[2]);
        double secs = std::atof(argv[3]);
        int threads = std::atoi(argv[4]);
        int exactFrom = getenv("EXACT_FROM") ? std::atoi(getenv("EXACT_FROM")) : 99;
        double exactSecs = getenv("EXACT_SECS") ? std::atof(getenv("EXACT_SECS")) : 30;
        Pos s = Pos::start();
        std::vector<Move> moves;
        for (int i = 5; i < argc; i++) { Move m; if (!parseMove(argv[i], m)) { printf("bad move %s\n", argv[i]); return 1; } moves.push_back(m); }
        std::vector<double> uiSecs;
        if (getenv("UI_SECS")) { std::stringstream ss(getenv("UI_SECS")); std::string t; while (std::getline(ss, t, ',')) uiSecs.push_back(std::atof(t.c_str())); }
        printf("ply stm  static");
        for (double us : uiSecs) printf("  ui%.0fs  ", us);
        for (int d = 1; d <= maxD; d++) printf("   d%d  ", d);
        printf("  exact   move played\n");
        for (size_t k = 0; k <= moves.size() && !s.over(); k++) {
            auto red = [&](int score) { double p = scoreToWinProb(score); return s.stm == 0 ? p : 1 - p; };
            printf("%3d  %s  %5.1f%%", (int)s.ply, s.stm == 0 ? "R" : "B", 100 * red(evaluate(s)));
            if (s.ply > 0 && uiSecs.size())
                for (double us : uiSecs) {  // what the UI shows: time-limited search, smoothed score
                    AbAnalysis a = abAnalyze(s, us, threads, 1);
                    const RootResult& b = a.moves[0];
                    printf("  %5.1f%%", 100 * red(b.exact ? b.display : b.score));
                }
            if (s.ply > 0)
                for (int d = 1; d <= maxD; d++) {
                    AbAnalysis a = abAnalyze(s, secs, threads, 1, d);
                    printf("  %5.1f%%", 100 * red(a.moves[0].score));
                    if (a.depth < d) printf("(%d)", a.depth);
                }
            if (s.ply >= exactFrom) {
                auto r = parallelSolve(s, exactSecs, threads, 21);
                int v = r.value;
                if (v == -9) printf("     ?  ");
                else { int red = s.stm == 0 ? v : -v; printf("  %5s ", red > 0 ? "R win" : red < 0 ? "B win" : "draw"); }
            } else printf("        ");
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
    } else if (mode == "refeature") {
        // snek refeature <in.csv> <head>: recompute feature columns from the stored positions
        std::ifstream in(argv[2]);
        int head = argc > 3 ? std::atoi(argv[3]) : 0;
        std::string line;
        std::getline(in, line);
        std::vector<std::string> cols;
        { std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) cols.push_back(c); }
        int ig = -1, iv = -1, ip = -1;
        for (int i = 0; i < (int)cols.size(); i++) { if (cols[i] == "game") ig = i; if (cols[i] == "value") iv = i; if (cols[i] == "pos") ip = i; }
        if (ip < 0) { fprintf(stderr, "no pos column\n"); return 1; }
        printDataHeader();
        while (std::getline(in, line)) {
            std::vector<std::string> f;
            std::stringstream ss(line); std::string c;
            while (std::getline(ss, c, ',')) f.push_back(c);
            if ((int)f.size() != (int)cols.size()) continue;
            Pos s;
            if (!parsePosStr(f[ip], s)) continue;
            fputs(dataRow(ig >= 0 ? std::stoi(f[ig]) : 0, s, std::stoi(f[iv]), head).c_str(), stdout);
        }
    } else if (mode == "arena") {
        // engine A vs engine B from random openings (random 1-hex + random reply),
        // every opening played twice with colours swapped.
        int pairs = argc > 2 ? std::atoi(argv[2]) : 10;
        double secs = argc > 3 ? std::atof(argv[3]) : 1;
        int threads = argc > 4 ? std::atoi(argv[4]) : 8;
        std::string A = argc > 5 ? argv[5] : "ab", B = argc > 6 ? argv[6] : "mcts";
        uint64_t seed = argc > 7 ? std::atoll(argv[7]) : 2024;
        int randomPlies = argc > 8 ? std::atoi(argv[8]) : 2;  // random moves at the start of each opening
        Solver solver(22);
        Rng rng(seed);
        int winA = 0, winB = 0, draws = 0;
        std::vector<Move> buf;
        for (int p = 0; p < pairs; p++) {
            Pos open = Pos::start();
            for (int k = 0; k < randomPlies; k++) { open.genMoves(buf); open.play(buf[rng.below((int)buf.size())]); }
            for (int sw = 0; sw < (A == B ? 1 : 2); sw++) {  // identical engines: the swapped game would be the same
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
