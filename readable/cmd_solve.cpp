// Exact solving from the command line: solvepos, solvebench, benchset.
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include "commands.hpp"
#include "notation.hpp"
#include "parallel_solve.hpp"

static double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// lossTest: the value 0 means "does not lose" (a win or a draw)
static const char* resultWord(int value, bool lossTest) { return value > 0 ? "win" : value < 0 ? "loss" : lossTest ? "notloss" : "draw"; }

// Root moves already solved by an earlier run, read from a progress file
// (lines "<move> <win|draw|loss|notloss> <nodes> <seconds>"). A "notloss" line, written by a
// SOLVER_LOSS_TEST run, counts as 0 in a loss test but is no exact value: an exact run solves it again.
static std::vector<std::pair<Move, int>> readProgress(const char* path, bool lossTest) {
    std::vector<std::pair<Move, int>> solved;
    std::ifstream in(path);
    std::string line, moveText, valueText;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        Move m;
        if (!(fields >> moveText >> valueText) || !parseMove(moveText, m) || (valueText == "notloss" && !lossTest)) continue;
        solved.push_back({m, valueText == "win" ? 1 : valueText == "loss" ? -1 : 0});
    }
    return solved;
}

// snek solvepos <seconds> <threads> <move> <move> ...
// Parallel exact solve of the position after the given moves. With env PROGRESS=<file> every
// solved root move is appended to the file, and moves already recorded there are skipped (resume).
// With env SOLVER_LOSS_TEST=1 it only proves or refutes that the side to move loses, which is much
// cheaper: every root move is "loss" or "notloss", and it stops at the first move that does not lose.
int cmdSolvePos(const Args& args) {
    double seconds = args.arg(2, 0.0);
    int threads = args.arg(3, 1);
    Position pos = Position::initial();
    for (int i = 4; i < args.argc; i++)
        if (!playMoveText(pos, args.argv[i])) { printf("illegal move %s\n", args.argv[i]); return 1; }
    bool lossTest = envInt("SOLVER_LOSS_TEST", 0) != 0;
    SolveQuestion question = lossTest ? SolveQuestion::LOSS_TEST : SolveQuestion::EXACT;

    std::vector<std::pair<Move, int>> alreadySolved;
    FILE* progress = nullptr;
    if (const char* progressPath = getenv("PROGRESS")) {
        alreadySolved = readProgress(progressPath, lossTest);
        if (!alreadySolved.empty()) printf("resuming: %zu root moves already solved\n", alreadySolved.size());
        progress = fopen(progressPath, "a");
    }
    printf("root moves: %zu  ply %d stm %d\n", symmetryUniqueMoves(pos).size(), (int)pos.ply, (int)pos.sideToMove);
    fflush(stdout);

    auto start = std::chrono::steady_clock::now();
    size_t solvedCount = alreadySolved.size();
    auto onResult = [&](Move m, int value, uint64_t nodes, double moveSeconds) {
        solvedCount++;
        if (progress) {
            fprintf(progress, "%s %s %llu %.1f\n", moveToString(m).c_str(), resultWord(value, lossTest), (unsigned long long)nodes, moveSeconds);
            fflush(progress);
        }
        printf("[%6.0fs] %4zu solved  %-22s %-4s %6.1fs %12llu nodes\n", secondsSince(start), solvedCount,
               moveToString(m).c_str(), resultWord(value, lossTest), moveSeconds, (unsigned long long)nodes);
        fflush(stdout);
    };
    RootSolveResult result = parallelSolve(pos, seconds, threads, 22, alreadySolved, onResult, question);
    if (progress) fclose(progress);

    int known = 0;
    for (auto& [move, value] : result.perMove) known += value != Solver::UNKNOWN;
    // the loss test answers "loses" or "does not lose", never an exact value (ui/server.js reads "value")
    std::string verdict = result.value == Solver::UNKNOWN ? "value unknown"
                          : !lossTest                     ? "value " + std::to_string(result.value)
                          : result.value < 0              ? "loses"
                                                          : "does not lose";
    printf("ply %d stm %d: %s  (%d/%zu root moves solved, %llu nodes, %.1fs)\n", (int)pos.ply, (int)pos.sideToMove, verdict.c_str(),
           known, result.perMove.size(), (unsigned long long)result.nodes, result.seconds);
    for (auto& [move, value] : result.perMove)
        if (value != Solver::UNKNOWN) printf("  %-22s %s\n", moveToString(move).c_str(), resultWord(value, lossTest));
    return 0;
}

// A random position after `plies` random moves (game not over yet).
static Position randomPosition(int plies, Rng& rng) {
    for (;;) {
        Position pos = Position::initial();
        std::vector<Move> moves;
        for (int i = 0; i < plies && !pos.isOver(); i++) {
            generateMoves(pos, moves);
            pos.play(moves.empty() ? Move::pass() : moves[rng.below((int)moves.size())]);
        }
        if (!pos.isOver()) return pos;
    }
}

// snek solvebench [plies] [count] [seconds] [threads]: exact solve of `count` random positions
// after `plies` random moves (the same positions on every run).
int cmdSolveBench(const Args& args) {
    int plies = args.arg(2, 10);
    int count = args.arg(3, 10);
    double seconds = args.arg(4, 60.0);
    int threads = args.arg(5, 1);
    std::atomic<int> next{0};
    std::mutex printLock;
    auto worker = [&] {
        Solver solver(21);
        for (;;) {
            int i = next++;
            if (i >= count) break;
            Rng rng(1000 + plies * 100000 + i);
            Position pos = randomPosition(plies, rng);
            solver.clear();
            auto start = std::chrono::steady_clock::now();
            int value = solver.solve(pos, seconds);
            double elapsed = secondsSince(start);
            std::lock_guard<std::mutex> lock(printLock);
            printf("plies=%d pos=%d value(stm)=%s nodes=%llu time=%.3fs knps=%.0f\n", plies, i,
                   value == Solver::UNKNOWN ? "timeout" : std::to_string(value).c_str(), (unsigned long long)solver.nodes, elapsed,
                   solver.nodes / elapsed / 1000);
            fflush(stdout);
        }
    };
    std::vector<std::thread> pool;
    for (int w = 0; w < threads; w++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    return 0;
}

// Per ply: nodes visited and how they ended - table cutoff, last pieces, bound cutoff, tight-bound
// cutoff, expanded -, the share of the moves that threat zones skipped, threat scans and how often
// they found a threat; then the one-player optimum's questions.
static void printProfile(const Solver::Profile& p, const OnePlayerOptimum::Statistics& onePlayer) {
    auto percent = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * part / whole : 0.0; };
    printf("ply     visited    tt-cut  last-pcs  bound-cut  tight-cut  expanded  threat-skip     scans  found\n");
    for (int ply = 0; ply < 16; ply++)
        if (p.visited[ply])
            printf("%3d %11llu %9.1f%% %8.1f%% %9.1f%% %9.1f%% %9.1f%% %9.1f%% %9llu %5.1f%%\n", ply, (unsigned long long)p.visited[ply],
                   percent(p.tableCut[ply], p.visited[ply]), percent(p.lastPieces[ply], p.visited[ply]),
                   percent(p.boundCut[ply], p.visited[ply]), percent(p.tightCut[ply], p.visited[ply]), percent(p.expanded[ply], p.visited[ply]),
                   percent(p.threatSkipped[ply], p.threatSkipped[ply] + p.movesListed[ply]), (unsigned long long)p.threatScans[ply],
                   percent(p.threatScansFound[ply], p.threatScans[ply]));
    printf("one-player queries %llu, cannot reach %.1f%%, %.1f search nodes per query, %llu out of budget\n",
           (unsigned long long)onePlayer.questions, percent(onePlayer.unreachable, onePlayer.questions),
           onePlayer.questions ? (double)onePlayer.searchNodes / onePlayer.questions : 0.0, (unsigned long long)onePlayer.outOfBudget);
}

// snek benchset <file> [threads]: exact solve of every position in the file (one move list per
// line). Threads take positions in order and keep their solver between positions, like
// parallelSolve does. Prints the cost per position and in total. Environment:
//   PROFILE=1        per-ply statistics of the solver
//   BENCH_FRESH=1    every position starts from an empty table (node counts independent of the
//                    order and the number of threads)
//   BENCH_WINTEST=1  only whether the side to move wins (the window (0, 1))
int cmdBenchSet(const Args& args) {
    std::ifstream in(args.arg(2, ""));
    int threads = args.arg(3, 8);
    std::vector<Position> positions;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string word;
        Position pos = Position::initial();
        bool ok = true, any = false;
        while (words >> word) {
            if (!playMoveText(pos, word)) { ok = false; break; }
            any = true;
        }
        if (ok && any) positions.push_back(pos);
    }
    bool fresh = envInt("BENCH_FRESH", 0) != 0;
    bool winTestOnly = envInt("BENCH_WINTEST", 0) != 0;

    std::vector<int> value(positions.size());
    std::vector<uint64_t> nodes(positions.size());
    std::vector<double> seconds(positions.size());
    std::atomic<size_t> next{0};
    std::mutex profileLock;
    Solver::Profile total;
    OnePlayerOptimum::Statistics totalOnePlayer;
    int sharedLog2 = envInt("SOLVER_SHARED", 0);
    std::shared_ptr<TranspositionTable> sharedTable = sharedLog2 ? std::make_shared<TranspositionTable>(sharedLog2 - 1) : nullptr;
    auto start = std::chrono::steady_clock::now();
    auto worker = [&] {
        std::unique_ptr<Solver> solver(sharedTable ? new Solver(sharedTable) : new Solver(22));
        for (;;) {
            size_t i = next++;
            if (i >= positions.size()) break;
            if (fresh) solver->clear();
            auto positionStart = std::chrono::steady_clock::now();
            value[i] = winTestOnly ? solver->solve(positions[i], 0, UINT64_MAX, 0, 1) : solver->solve(positions[i]);
            nodes[i] = solver->nodes;
            {
                std::lock_guard<std::mutex> lock(profileLock);
                total.add(solver->profile);
                solver->profile = Solver::Profile{};
                totalOnePlayer.add(solver->onePlayerStatistics());
                solver->resetOnePlayerStatistics();
            }
            seconds[i] = secondsSince(positionStart);
        }
    };
    std::vector<std::thread> pool;
    for (int w = 0; w < threads; w++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    double wall = secondsSince(start);

    uint64_t totalNodes = 0;
    double threadSeconds = 0;
    for (size_t i = 0; i < positions.size(); i++) {
        printf("%3zu  value %2d  %12llu nodes  %7.1fs\n", i, value[i], (unsigned long long)nodes[i], seconds[i]);
        totalNodes += nodes[i];
        threadSeconds += seconds[i];
    }
    printf("TOTAL %zu positions: %llu nodes, %.1f thread-seconds, %.1f s wall (%d threads)\n", positions.size(),
           (unsigned long long)totalNodes, threadSeconds, wall, threads);
    if (getenv("PROFILE")) printProfile(total, totalOnePlayer);
    return 0;
}
