// snek - Snekagon engine, exact solver and tools. Usage: snek <command> [arguments]
//
// Readable version of src/ (same behaviour, same results); see README.md for a reading guide.
#include <cstdio>
#include <map>
#include "commands.hpp"
#include "position.hpp"

struct Command {
    int (*run)(const Args&);
    const char* usage;
};

static const std::map<std::string, Command> COMMANDS = {
    // playing / UI
    {"serve", {cmdServe, "serve [threads]                       line protocol for the UI (ui/server.js)"}},
    {"arena", {cmdArena, "arena [pairs] [s/move] [threads] [engineA] [engineB] [seed] [randomPlies]"}},
    {"selfplay", {cmdSelfPlay, "selfplay [games] [s/move] [threads]"}},
    // exact solving
    {"solvepos", {cmdSolvePos, "solvepos <seconds> <threads> <move>...    (env PROGRESS=<file> to log/resume)"}},
    {"worker", {cmdWorker, "worker <coordinator[:port]> [threads] [name]   distributed solving"}},
    {"solvebench", {cmdSolveBench, "solvebench [plies] [count] [seconds] [threads]"}},
    {"benchset", {cmdBenchSet, "benchset <file> [threads]             (env PROFILE=1 for per-ply statistics)"}},
    // checks
    {"perft", {cmdPerft, "perft [depth]                         count move sequences"}},
    {"verify", {cmdVerify, "verify [games]                        fast generator/bounds vs reference versions"}},
    {"info", {cmdInfo, "info                                  number of placements per piece"}},
    {"speed", {cmdSpeed, "speed                                 micro-benchmarks"}},
    // evaluation and training data
    {"gendata", {cmdGenData, "gendata [games] [plyMin] [plyMax] [solveSecs] [threads] [seed] [good] [head]"}},
    {"refeature", {cmdRefeature, "refeature <in.csv> [head]"}},
    {"evalexplain", {cmdEvalExplain, "evalexplain <move>...                (env WEIGHTS=<file>)"}},
    {"evaltrace", {cmdEvalTrace, "evaltrace <maxDepth> <seconds> <threads> <move>...   (env EXACT_FROM, EXACT_SECS, UI_SECS)"}},
};

int main(int argc, char** argv) {
    initTables();
    std::string name = argc > 1 ? argv[1] : "serve";
    auto command = COMMANDS.find(name);
    if (command == COMMANDS.end()) {
        fprintf(stderr, "unknown command '%s'. Commands:\n", name.c_str());
        for (auto& [_, c] : COMMANDS) fprintf(stderr, "  snek %s\n", c.usage);
        return 1;
    }
    return command->second.run(Args{argc, argv});
}
