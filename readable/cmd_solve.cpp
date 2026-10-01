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

static const char* resultWord(int value) { return value > 0 ? "win" : value < 0 ? "loss" : "draw"; }

// Root moves already solved by an earlier run, read from a progress file
// (lines "<move> <win|draw|loss> <nodes> <seconds>").
static std::vector<std::pair<Move, int>> readProgress(const char* path) {
    std::vector<std::pair<Move, int>> solved;
    std::ifstream in(path);
    std::string line, moveText, valueText;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        Move m;
        if (fields >> moveText >> valueText && parseMove(moveText, m))
            solved.push_back({m, valueText == "win" ? 1 : valueText == "loss" ? -1 : 0});
    }
    return solved;
}

// snek solvepos <seconds> <threads> <move> <move> ...
// Parallel exact solve of the position after the given moves. With env PROGRESS=<file> every
// solved root move is appended to the file, and moves already recorded there are skipped (resume).
int cmdSolvePos(const Args& args) {
    double seconds = args.arg(2, 0.0);
    int threads = args.arg(3, 1);
    Position pos = Position::initial();
    for (int i = 4; i < args.argc; i++)
        if (!playMoveText(pos, args.argv[i])) { printf("illegal move %s\n", args.argv[i]); return 1; }

    std::vector<std::pair<Move, int>> alreadySolved;
    FILE* progress = nullptr;
    if (const char* progressPath = getenv("PROGRESS")) {
        alreadySolved = readProgress(progressPath);
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
            fprintf(progress, "%s %s %llu %.1f\n", moveToString(m).c_str(), resultWord(value), (unsigned long long)nodes, moveSeconds);
            fflush(progress);
        }
        printf("[%6.0fs] %4zu solved  %-22s %-4s %6.1fs %12llu nodes\n", secondsSince(start), solvedCount,
               moveToString(m).c_str(), resultWord(value), moveSeconds, (unsigned long long)nodes);
        fflush(stdout);
    };
    RootSolveResult result = parallelSolve(pos, seconds, threads, 22, alreadySolved, onResult);
    if (progress) fclose(progress);

    int known = 0;
    for (auto& [move, value] : result.perMove) known += value != Solver::UNKNOWN;
    printf("ply %d stm %d: value %s  (%d/%zu root moves solved, %llu nodes, %.1fs)\n", (int)pos.ply, (int)pos.sideToMove,
           result.value == Solver::UNKNOWN ? "unknown" : std::to_string(result.value).c_str(), known, result.perMove.size(),
           (unsigned long long)result.nodes, result.seconds);
    for (auto& [move, value] : result.perMove)
        if (value != Solver::UNKNOWN) printf("  %-22s %s\n", moveToString(move).c_str(), resultWord(value));
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

// snek benchset <file> [threads]: exact solve of every position in the file (one move list per
// line). Threads take positions in order and keep their solver between positions, like
// parallelSolve does. Prints the cost per position and in total; env PROFILE=1 adds per-ply
// statistics of the solver.
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

    std::vector<int> value(positions.size());
    std::vector<uint64_t> nodes(positions.size());
    std::vector<double> seconds(positions.size());
    std::atomic<size_t> next{0};
    std::mutex profileLock;
    Solver::Profile total;
    int sharedLog2 = envInt("SOLVER_SHARED", 0);
    std::shared_ptr<TranspositionTable> sharedTable = sharedLog2 ? std::make_shared<TranspositionTable>(sharedLog2 - 1) : nullptr;
    auto start = std::chrono::steady_clock::now();
    auto worker = [&] {
        std::unique_ptr<Solver> solver(sharedTable ? new Solver(sharedTable) : new Solver(22));
        for (;;) {
            size_t i = next++;
            if (i >= positions.size()) break;
            auto positionStart = std::chrono::steady_clock::now();
            value[i] = solver->solve(positions[i]);
            nodes[i] = solver->nodes;
            {
                std::lock_guard<std::mutex> lock(profileLock);
                for (int p = 0; p < 16; p++) {
                    total.visited[p] += solver->profile.visited[p];
                    total.tableCut[p] += solver->profile.tableCut[p];
                    total.boundCut[p] += solver->profile.boundCut[p];
                    total.expanded[p] += solver->profile.expanded[p];
                }
                solver->profile = Solver::Profile{};
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
    if (getenv("PROFILE")) {
        printf("ply     visited    tt-cut  bound-cut  expanded\n");
        for (int p = 0; p < 16; p++)
            if (total.visited[p])
                printf("%3d %11llu %9.1f%% %9.1f%% %9.1f%%\n", p, (unsigned long long)total.visited[p],
                       100.0 * total.tableCut[p] / total.visited[p], 100.0 * total.boundCut[p] / total.visited[p],
                       100.0 * total.expanded[p] / total.visited[p]);
    }
    return 0;
}
