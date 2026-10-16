// Evaluation tooling: training data generation (gendata, refeature) and inspection of the
// evaluation (evalexplain, evaltrace).
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include "commands.hpp"
#include "engine.hpp"
#include "parallel_solve.hpp"
#include "training_data.hpp"

// Fields of a line separated by `separator` (a trailing separator adds no empty field).
static std::vector<std::string> splitFields(const std::string& line, char separator) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, separator)) fields.push_back(field);
    return fields;
}

// ---- gendata ---------------------------------------------------------------------------------

struct GameStyle {
    double temperature;   // softmax temperature over 1-ply evaluations (centi-logits)
    double randomChance;  // probability of a uniformly random move
    bool growPolicy;      // instead of the evaluation: mostly grow an own snake
};

// Picks the next move of a data-generation game.
static Move chooseDataMove(const Position& pos, const std::vector<Move>& moves, const GameStyle& style, bool goodPlay, Rng& rng) {
    if (rng.uniform() < style.randomChance || pos.ply == 0 || (goodPlay && pos.ply == 1))
        return moves[rng.below((int)moves.size())];
    if (style.growPolicy) {
        // prefer growing an own snake (mostly the longest one), sometimes block
        std::vector<Move> growing;
        for (Move m : moves)
            if (m.placement().border & pos.ownCells()) growing.push_back(m);
        if (!growing.empty() && rng.uniform() < 0.85) return growing[rng.below((int)growing.size())];
        return moves[rng.below((int)moves.size())];
    }
    // softmax over the 1-ply evaluation
    std::vector<double> weight(moves.size());
    double best = -1e18;
    for (size_t i = 0; i < moves.size(); i++) {
        Position child = pos;
        child.play(moves[i]);
        weight[i] = child.isOver() ? -finishedGameScore(child) : -evaluate(child);
        best = std::max(best, weight[i]);
    }
    double sum = 0;
    for (auto& w : weight) {
        w = std::exp((w - best) / style.temperature);
        sum += w;
    }
    double r = rng.uniform() * sum;
    size_t pick = 0;
    for (; pick + 1 < weight.size(); pick++) {
        r -= weight[pick];
        if (r <= 0) break;
    }
    return moves[pick];
}

// snek gendata [games] [plyMin] [plyMax] [solveSecs] [threads] [seed] [good] [head]
// Plays games with a randomised policy, solves the positions with plyMin <= ply <= plyMax
// exactly and prints their features and exact value as CSV (see training_data.hpp).
//   good: random first two moves, then nearly greedy play by the evaluation
//   head: territory head start for the feature columns
int cmdGenData(const Args& args) {
    int games = args.arg(2, 100);
    int plyMin = args.arg(3, 7);
    int plyMax = args.arg(4, 10);
    double solveSeconds = args.arg(5, 5.0);
    int threads = args.arg(6, 8);
    uint64_t seed = args.argU64(7, 7);
    bool goodPlay = args.arg(8, "") == std::string("good");
    int headStart = args.arg(9, 0);
    std::atomic<int> nextGame{0};
    std::mutex outputLock;
    printDataHeader();
    auto worker = [&] {
        Solver solver(21);
        std::vector<Move> moves;
        for (;;) {
            int game = nextGame++;
            if (game >= games) break;
            Rng rng(seed * 1000003 + game);
            GameStyle style;
            style.temperature = 40 + rng.below(160);
            style.randomChance = 0.05 + 0.2 * rng.uniform();
            style.growPolicy = game % 2 == 1;
            if (goodPlay) style = {double(10 + rng.below(30)), 0, false};
            Position pos = Position::initial();
            while (!pos.isOver() && pos.ply <= plyMax) {
                if (pos.ply >= plyMin) {
                    solver.clear();
                    int value = solver.solve(pos, solveSeconds);
                    if (value != Solver::UNKNOWN) {
                        std::string row = dataRow(game, pos, value, headStart);
                        std::lock_guard<std::mutex> lock(outputLock);
                        fputs(row.c_str(), stdout);
                        fflush(stdout);
                    }
                }
                generateMoves(pos, moves);
                pos.play(moves.empty() ? Move::pass() : chooseDataMove(pos, moves, style, goodPlay, rng));
            }
        }
    };
    std::vector<std::thread> pool;
    for (int w = 0; w < threads; w++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    return 0;
}

// ---- refeature -------------------------------------------------------------------------------

// snek refeature <in.csv> [head]: recomputes the feature columns from the stored positions
// (after changing the features), keeping game and value.
int cmdRefeature(const Args& args) {
    std::ifstream in(args.arg(2, ""));
    int headStart = args.arg(3, 0);
    std::string line;
    std::getline(in, line);
    std::vector<std::string> columns = splitFields(line, ',');
    int gameColumn = -1, valueColumn = -1, positionColumn = -1;
    for (int i = 0; i < (int)columns.size(); i++) {
        if (columns[i] == "game") gameColumn = i;
        if (columns[i] == "value") valueColumn = i;
        if (columns[i] == "pos") positionColumn = i;
    }
    if (positionColumn < 0) { fprintf(stderr, "no pos column\n"); return 1; }
    printDataHeader();
    while (std::getline(in, line)) {
        std::vector<std::string> field = splitFields(line, ',');
        if (field.size() != columns.size()) continue;
        Position pos;
        if (!parsePosition(field[positionColumn], pos)) continue;
        int game = gameColumn >= 0 ? std::stoi(field[gameColumn]) : 0;
        fputs(dataRow(game, pos, std::stoi(field[valueColumn]), headStart).c_str(), stdout);
    }
    return 0;
}

// ---- evalexplain -----------------------------------------------------------------------------

// The logit of a feature vector with weight set 0 (without the early-game calibration), summed in
// feature order. Kept out of line like learnedLogit: inlined and unrolled, its multiply-adds are
// fused (FMA), and a printed logit can then round differently from src/'s.
[[gnu::noinline]] static double explainedLogit(const std::array<double, NUM_FEATURES>& x) {
    double z = 0;
    for (int i = 0; i < NUM_FEATURES; i++) z += x[i] * featureWeights[0][i];
    return z;
}

// snek evalexplain <move>...: feature values and their contributions to the logit after every
// move (env WEIGHTS=<file> to explain a weights file instead of the compiled-in weights).
int cmdEvalExplain(const Args& args) {
    if (getenv("WEIGHTS")) {
        if (!loadWeights(getenv("WEIGHTS"), 1)) return 1;
        for (int i = 0; i < NUM_FEATURES; i++) featureWeights[0][i] = featureWeights[1][i];
        featureHeadStart[0] = featureHeadStart[1];
    }
    std::vector<Move> moves;
    for (int i = 2; i < args.argc; i++) {
        Move m;
        if (!parseMove(args.argv[i], m)) return 1;
        moves.push_back(m);
    }
    printf("%-12s", "feature");
    for (size_t k = 1; k <= moves.size(); k++) printf("  ply%-2zu(%s)", k, k % 2 ? "B" : "R");
    printf("\n");

    std::vector<std::array<double, NUM_FEATURES>> features;
    std::vector<int> sideToMove;
    Position pos = Position::initial();
    for (Move m : moves) {
        pos.play(m);
        std::array<double, NUM_FEATURES> x;
        featureVector(pos, x.data());
        features.push_back(x);
        sideToMove.push_back(pos.sideToMove);
    }
    for (int i = 0; i < NUM_FEATURES; i++) {
        printf("%-12s", FEATURE_NAMES[i]);
        for (auto& x : features) printf("  %4.0f %+5.2f", x[i], x[i] * featureWeights[0][i]);
        printf("\n");
    }
    printf("%-12s", "logit(stm)");
    for (auto& x : features) printf("  %10.2f", explainedLogit(x));
    printf("\n%-12s", "red win %");
    for (size_t k = 0; k < features.size(); k++) {
        double p = 1 / (1 + std::exp(-explainedLogit(features[k])));
        printf("  %9.1f%%", 100 * (sideToMove[k] == RED ? p : 1 - p));
    }
    printf("\n");
    return 0;
}

// ---- evaltrace -------------------------------------------------------------------------------

// snek evaltrace <maxDepth> <seconds> <threads> <move>...
// For every position of a game: the static evaluation and fixed-depth search results, all as
// red's win probability. Env UI_SECS=<s,s,...>: also what the UI shows after that much time;
// EXACT_FROM=<ply> (with EXACT_SECS): exact result from that ply on.
int cmdEvalTrace(const Args& args) {
    int maxDepth = args.arg(2, 4);
    double seconds = args.arg(3, 10.0);
    int threads = args.arg(4, 1);
    int exactFrom = envInt("EXACT_FROM", 99);
    double exactSeconds = getenv("EXACT_SECS") ? std::atof(getenv("EXACT_SECS")) : 30;
    std::vector<Move> moves;
    for (int i = 5; i < args.argc; i++) {
        Move m;
        if (!parseMove(args.argv[i], m)) { printf("bad move %s\n", args.argv[i]); return 1; }
        moves.push_back(m);
    }
    std::vector<double> uiSeconds;
    if (getenv("UI_SECS"))
        for (const std::string& s : splitFields(getenv("UI_SECS"), ',')) uiSeconds.push_back(std::atof(s.c_str()));

    printf("ply stm  static");
    for (double s : uiSeconds) printf("  ui%.0fs  ", s);
    for (int d = 1; d <= maxDepth; d++) printf("   d%d  ", d);
    printf("  exact   move played\n");

    Position pos = Position::initial();
    for (size_t k = 0; k <= moves.size() && !pos.isOver(); k++) {
        auto redWinProb = [&](int score) {
            double p = scoreToWinProb(score);
            return pos.sideToMove == RED ? p : 1 - p;
        };
        printf("%3d  %s  %5.1f%%", (int)pos.ply, pos.sideToMove == RED ? "R" : "B", 100 * redWinProb(evaluate(pos)));
        if (pos.ply > 0)
            for (double s : uiSeconds) {  // what the UI shows: time-limited search, smoothed score
                EngineAnalysis a = analyzeWithEngine(pos, s, threads, 1);
                const RootMoveResult& best = a.moves[0];
                printf("  %5.1f%%", 100 * redWinProb(best.exact ? best.display : best.score));
            }
        if (pos.ply > 0)
            for (int d = 1; d <= maxDepth; d++) {
                EngineAnalysis a = analyzeWithEngine(pos, seconds, threads, 1, d);
                printf("  %5.1f%%", 100 * redWinProb(a.moves[0].score));
                if (a.depth < d) printf("(%d)", a.depth);
            }
        if (pos.ply >= exactFrom) {
            int value = parallelSolve(pos, exactSeconds, threads, 21).value;
            if (value == Solver::UNKNOWN) {
                printf("     ?  ");
            } else {
                int forRed = pos.sideToMove == RED ? value : -value;
                printf("  %5s ", forRed > 0 ? "R win" : forRed < 0 ? "B win" : "draw");
            }
        } else {
            printf("        ");
        }
        printf("   %s\n", k < moves.size() ? moveToString(moves[k]).c_str() : "");
        fflush(stdout);
        if (k < moves.size()) pos.play(moves[k]);
    }
    return 0;
}
