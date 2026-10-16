// The command-line commands of `snek` (one function per command, see main.cpp for the list).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>

// Positional command-line arguments with defaults: arg(2, 10) is argv[2] as a number, or 10.
struct Args {
    int argc;
    char** argv;

    bool has(int i) const { return argc > i; }
    int arg(int i, int fallback) const { return has(i) ? std::atoi(argv[i]) : fallback; }
    double arg(int i, double fallback) const { return has(i) ? std::atof(argv[i]) : fallback; }
    uint64_t argU64(int i, uint64_t fallback) const { return has(i) ? (uint64_t)std::atoll(argv[i]) : fallback; }
    std::string arg(int i, const char* fallback) const { return has(i) ? argv[i] : fallback; }
};

// cmd_check.cpp
int cmdPerft(const Args& args);
int cmdVerify(const Args& args);
int cmdInfo(const Args& args);
int cmdSpeed(const Args& args);
// cmd_solve.cpp
int cmdSolveBench(const Args& args);
int cmdBenchSet(const Args& args);
int cmdSolvePos(const Args& args);
// cmd_refute.cpp
int cmdRefute(const Args& args);
int cmdProveLine(const Args& args);
int cmdUniqueMoves(const Args& args);
// cmd_data.cpp
int cmdGenData(const Args& args);
int cmdRefeature(const Args& args);
int cmdEvalExplain(const Args& args);
int cmdEvalTrace(const Args& args);
// cmd_play.cpp
int cmdArena(const Args& args);
int cmdSelfPlay(const Args& args);
// cmd_serve.cpp
int cmdServe(const Args& args);
// cmd_worker.cpp
int cmdWorker(const Args& args);
