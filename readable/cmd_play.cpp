// Engine games: arena (engine A against engine B) and selfplay.
#include "analysis.hpp"
#include "commands.hpp"
#include "notation.hpp"

// snek arena [pairs] [s/move] [threads] [engineA] [engineB] [seed] [randomPlies]
// Engine A against engine B from random openings (`randomPlies` random moves). Every opening is
// played twice with the colours swapped (once if both engines are the same).
int cmdArena(const Args& args) {
    int pairs = args.arg(2, 10);
    double seconds = args.arg(3, 1.0);
    int threads = args.arg(4, 8);
    std::string engineA = args.arg(5, "ab"), engineB = args.arg(6, "mcts");
    uint64_t seed = args.argU64(7, 2024);
    int randomPlies = args.arg(8, 2);
    Solver solver(22);
    Rng rng(seed);
    int winsA = 0, winsB = 0, draws = 0;
    std::vector<Move> moves;
    for (int pair = 0; pair < pairs; pair++) {
        Position opening = Position::initial();
        for (int k = 0; k < randomPlies; k++) {
            generateMoves(opening, moves);
            opening.play(moves[rng.below((int)moves.size())]);
        }
        int games = engineA == engineB ? 1 : 2;  // identical engines would play the same game twice
        for (int swapped = 0; swapped < games; swapped++) {
            std::string engine[2] = {swapped ? engineB : engineA, swapped ? engineA : engineB};  // red, blue
            Position pos = opening;
            std::string record;
            while (!pos.isOver()) {
                Move m = analyze(pos, seconds, threads, solver, engine[pos.sideToMove]).items[0].move;
                record += moveToString(m) + " ";
                pos.play(m);
            }
            int red = longestSnakeIn(pos.cells[RED]), blue = longestSnakeIn(pos.cells[BLUE]);
            const char* result = red > blue ? "P1" : red < blue ? "P2" : "draw";
            if (red == blue) draws++;
            else if ((red > blue) == (swapped == 0)) winsA++;
            else winsB++;
            printf("pair %d  P1=%s P2=%s  %d-%d  %s   | A %d  B %d  draws %d\n", pair, engine[0].c_str(), engine[1].c_str(), red,
                   blue, result, winsA, winsB, draws);
            printf("   %s\n", record.c_str());
            fflush(stdout);
        }
    }
    printf("FINAL %s %d : %s %d  (draws %d)\n", engineA.c_str(), winsA, engineB.c_str(), winsB, draws);
    return 0;
}

// snek selfplay [games] [s/move] [threads]: the default engine against itself.
int cmdSelfPlay(const Args& args) {
    int games = args.arg(2, 1);
    double seconds = args.arg(3, 1.0);
    int threads = args.arg(4, 8);
    Solver solver(22);
    for (int game = 0; game < games; game++) {
        Position pos = Position::initial();
        std::string record;
        while (!pos.isOver()) {
            Analysis a = analyze(pos, seconds, threads, solver, "ab");
            Move m = a.items[0].move;
            record += moveToString(m) + " ";
            printf("ply %d stm %d: %s  (%s %s)\n", (int)pos.ply, (int)pos.sideToMove, moveToString(m).c_str(),
                   a.exact ? "exact" : "mcts", a.exact ? std::to_string(a.value).c_str() : std::to_string(a.items[0].winProb).c_str());
            fflush(stdout);
            pos.play(m);
        }
        printf("game %d: P1 %d  P2 %d\nmoves: %s\n", game, longestSnakeIn(pos.cells[RED]), longestSnakeIn(pos.cells[BLUE]),
               record.c_str());
    }
    return 0;
}
