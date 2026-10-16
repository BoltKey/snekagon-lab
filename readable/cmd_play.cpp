// Engine games: arena (engine A against engine B) and selfplay.
#include <fstream>
#include <sstream>
#include "analysis.hpp"
#include "commands.hpp"
#include "notation.hpp"

// An arena opening: the position and its moves as text, each followed by a space.
struct Opening {
    Position pos = Position::initial();
    std::string record;
};

// The openings of a book (snek book): one move list per line, anything after a # is a comment; empty
// lines and lines with a move that does not parse or is illegal are skipped.
static std::vector<Opening> readOpeningBook(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        fprintf(stderr, "cannot open book %s\n", path.c_str());
        exit(1);
    }
    std::vector<Opening> book;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line.substr(0, line.find('#')));
        Opening opening;
        std::string word;
        bool ok = true;
        while (ok && words >> word) {
            Move m;
            ok = parseMove(word, m) && isLegalMove(opening.pos, m);
            if (!ok) break;
            opening.record += moveToString(m) + " ";
            opening.pos.play(m);
        }
        if (ok && !opening.record.empty()) book.push_back(opening);
    }
    return book;
}

// snek arena [pairs] [s/move] [threads] [engineA] [engineB] [seed] [randomPlies]
// Engine A against engine B from random openings (`randomPlies` random moves). Every opening is
// played twice with the colours swapped (once if both engines are the same). The printed game
// records start with the opening, so they replay from the start. ARENA_BOOK=<file>: the openings of
// that book instead, pair p taking line ARENA_BOOK_START + p (cyclically).
int cmdArena(const Args& args) {
    int pairs = args.arg(2, 10);
    double seconds = args.arg(3, 1.0);
    int threads = args.arg(4, 8);
    std::string engineA = args.arg(5, "ab"), engineB = args.arg(6, "mcts");
    uint64_t seed = args.argU64(7, 2024);
    int randomPlies = args.arg(8, 2);
    std::vector<Opening> book;
    if (const char* path = std::getenv("ARENA_BOOK")) book = readOpeningBook(path);
    int bookStart = envInt("ARENA_BOOK_START", 0);
    Solver solver(22);
    Rng rng(seed);
    int winsA = 0, winsB = 0, draws = 0;
    std::vector<Move> moves;
    for (int pair = 0; pair < pairs; pair++) {
        Opening opening;
        if (!book.empty()) {
            opening = book[(bookStart + pair) % book.size()];
        } else {
            for (int k = 0; k < randomPlies; k++) {
                generateMoves(opening.pos, moves);
                Move m = moves[rng.below((int)moves.size())];
                opening.record += moveToString(m) + " ";
                opening.pos.play(m);
            }
        }
        int games = engineA == engineB ? 1 : 2;  // identical engines would play the same game twice
        for (int swapped = 0; swapped < games; swapped++) {
            std::string engine[2] = {swapped ? engineB : engineA, swapped ? engineA : engineB};  // red, blue
            Position pos = opening.pos;
            std::string record = opening.record;
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
