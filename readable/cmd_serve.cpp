// snek serve [threads]: the line protocol between the engine and the web UI (ui/server.js).
// One command per line on stdin, one JSON object per line on stdout:
//
//   new                     start a new game                          -> state
//   set <move>...           replace the game by these moves            -> state
//   play <move>...          append moves                               -> state
//   undo                    take back the last move                    -> state
//   state                                                              -> state
//   analyze [seconds]       analyse, up to 8 moves with exact scores   -> analysis
//   go [seconds]            analyse and play the best move             -> {played, analysis, state}
//   solve [seconds]         parallel exact solve                       -> value per root move
//   children <move>...      symmetry-unique moves of that position, best first by the learned
//                           evaluation (used by the distributed-solve coordinator)
//   engine ab|mcts          choose the engine for analyze/go
//   quit
#include <iostream>
#include <sstream>
#include <thread>
#include "analysis.hpp"
#include "commands.hpp"
#include "notation.hpp"
#include "parallel_solve.hpp"

static const char* jsonBool(bool b) { return b ? "true" : "false"; }

static std::string cellsJson(Bitboard cells) {
    std::string json = "[";
    forEachCell(cells, [&](int i) {
        if (json.size() > 1) json += ',';
        json += std::to_string(i);
    });
    return json + "]";
}

static std::string moveJson(Move m) {
    if (m.isPass()) return "{\"p\":\"pass\",\"c\":[],\"s\":\"pass\"}";
    return std::string("{\"p\":\"") + PIECE_NAME[m.piece()] + "\",\"c\":" + cellsJson(m.placement().cells) + ",\"s\":\"" +
           moveToString(m) + "\"}";
}

static std::string piecesJson(PieceSet pieces) {
    std::string json = "[";
    for (int p = 0; p < NUM_PIECES; p++)
        if (hasPiece(pieces, p)) {
            if (json.size() > 1) json += ',';
            json += std::string("\"") + PIECE_NAME[p] + "\"";
        }
    return json + "]";
}

static const std::string ILLEGAL_MOVE = "{\"ok\":false,\"error\":\"illegal move\"}";
static const std::string GAME_OVER = "{\"ok\":false,\"error\":\"game over\"}";

// The game being shown in the UI.
class ServedGame {
  public:
    explicit ServedGame(int threads) : threads(threads), solver(22) {}

    std::string handle(const std::string& command, std::istringstream& in) {
        if (command == "new") { replay({}); return stateJson(); }
        if (command == "set" || command == "play") return setMoves(command == "play", in);
        if (command == "undo") {
            std::vector<Move> moves = history;
            if (!moves.empty()) moves.pop_back();
            replay(moves);
            return stateJson();
        }
        if (command == "state") return stateJson();
        if (command == "analyze" || command == "go") return analyzeCommand(command == "go", in);
        if (command == "solve") return solveCommand(in);
        if (command == "children") return childrenCommand(in);
        if (command == "engine") {
            std::string name;
            in >> name;
            if (name == "ab" || name == "mcts") engine = name;
            return "{\"ok\":true,\"engine\":\"" + engine + "\"}";
        }
        return "{\"ok\":false,\"error\":\"unknown command\"}";
    }

  private:
    int threads;
    Solver solver;
    std::string engine = "ab";
    Position pos = Position::initial();
    std::vector<Move> history;

    // Replays the moves from the start; stops at the first illegal one and returns false.
    bool replay(std::vector<Move> moves) {
        pos = Position::initial();
        history.clear();
        for (Move m : moves) {
            if (pos.isOver() || !isLegalMove(pos, m)) return false;
            pos.play(m);
            history.push_back(m);
        }
        return true;
    }

    std::string setMoves(bool append, std::istringstream& in) {
        std::vector<Move> moves = append ? history : std::vector<Move>{};
        std::string text;
        while (in >> text) {
            Move m;
            if (!parseMove(text, m)) { replay(history); return ILLEGAL_MOVE; }
            moves.push_back(m);
        }
        std::vector<Move> previous = history;
        if (!replay(moves)) { replay(previous); return ILLEGAL_MOVE; }
        return stateJson();
    }

    std::string stateJson() {
        std::vector<Move> legal;
        if (!pos.isOver()) generateMoves(pos, legal);
        SnakeBounds red = snakeBounds(pos, RED), blue = snakeBounds(pos, BLUE);
        std::ostringstream o;
        o << "{\"ok\":true,\"ply\":" << (int)pos.ply << ",\"stm\":" << (int)pos.sideToMove << ",\"over\":" << jsonBool(pos.isOver())
          << ",\"own\":[" << cellsJson(pos.cells[RED]) << "," << cellsJson(pos.cells[BLUE]) << "]"
          << ",\"avail\":[" << piecesJson(pos.piecesLeft[RED]) << "," << piecesJson(pos.piecesLeft[BLUE]) << "]"
          << ",\"longest\":[" << longestSnakeIn(pos.cells[RED]) << "," << longestSnakeIn(pos.cells[BLUE]) << "]"
          << ",\"bounds\":[[" << red.lower << "," << red.upper << "],[" << blue.lower << "," << blue.upper << "]]";
        o << ",\"history\":[";
        for (size_t i = 0; i < history.size(); i++) o << (i ? "," : "") << moveJson(history[i]);
        o << "],\"legal\":[";
        for (size_t i = 0; i < legal.size(); i++) o << (i ? "," : "") << moveJson(legal[i]);
        o << "]}";
        return o.str();
    }

    std::string analyzeCommand(bool playBest, std::istringstream& in) {
        double seconds = 2;
        in >> seconds;
        if (pos.isOver()) return GAME_OVER;
        Analysis a = analyze(pos, seconds, threads, solver, engine, playBest ? 1 : 8);
        std::ostringstream o;
        o << "{\"ok\":true,\"exact\":" << jsonBool(a.exact) << ",\"value\":" << a.value << ",\"stm\":" << (int)pos.sideToMove
          << ",\"engine\":\"" << a.engine << "\",\"depth\":" << a.depth << ",\"moves\":[";
        size_t shown = std::min<size_t>(a.items.size(), 40);
        for (size_t i = 0; i < shown; i++) {
            const Analysis::Item& item = a.items[i];
            o << (i ? "," : "") << "{\"m\":" << moveJson(item.move) << ",\"n\":" << item.visits << ",\"wr\":" << item.winProb
              << ",\"exact\":" << item.exact << ",\"score\":" << item.score << "}";
        }
        o << "]}";
        if (!playBest || a.items.empty()) return o.str();
        history.push_back(a.items[0].move);
        replay(history);
        return "{\"ok\":true,\"played\":" + moveJson(a.items[0].move) + ",\"analysis\":" + o.str() + ",\"state\":" + stateJson() + "}";
    }

    std::string solveCommand(std::istringstream& in) {
        double seconds = 10;
        in >> seconds;
        if (pos.isOver()) return GAME_OVER;
        RootSolveResult r = parallelSolve(pos, seconds, threads, 21);
        // wins first, then draws, unknown, losses
        auto rank = [](int v) { return v == Solver::UNKNOWN ? -0.5 : (double)v; };
        std::stable_sort(r.perMove.begin(), r.perMove.end(), [&](auto& a, auto& b) { return rank(a.second) > rank(b.second); });
        std::ostringstream o;
        o << "{\"ok\":true,\"stm\":" << (int)pos.sideToMove << ",\"value\":" << r.value << ",\"nodes\":" << r.nodes
          << ",\"seconds\":" << r.seconds << ",\"moves\":[";
        size_t shown = std::min<size_t>(r.perMove.size(), 60);
        for (size_t i = 0; i < shown; i++)
            o << (i ? "," : "") << "{\"m\":" << moveJson(r.perMove[i].first) << ",\"v\":" << r.perMove[i].second << "}";
        o << "]}";
        return o.str();
    }

    std::string childrenCommand(std::istringstream& in) {
        Position p = Position::initial();
        std::string text;
        while (in >> text)
            if (!playMoveText(p, text)) return ILLEGAL_MOVE;
        std::vector<std::pair<double, Move>> scored;
        for (Move m : symmetryUniqueMoves(p)) {
            Position child = p;
            child.play(m);
            scored.push_back({child.isOver() ? -gameResult(child) * 100.0 : -learnedLogit(child), m});
        }
        std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::ostringstream o;
        o << "{\"ok\":true,\"ply\":" << (int)p.ply << ",\"stm\":" << (int)p.sideToMove << ",\"over\":" << jsonBool(p.isOver()) << ",\"moves\":[";
        for (size_t i = 0; i < scored.size(); i++)
            o << (i ? "," : "") << "{\"s\":\"" << moveToString(scored[i].second) << "\",\"e\":" << scored[i].first << "}";
        o << "]}";
        return o.str();
    }
};

int cmdServe(const Args& args) {
    int threads = args.arg(2, (int)std::max(1u, std::thread::hardware_concurrency() - 2));
    ServedGame game(threads);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream in(line);
        std::string command;
        in >> command;
        if (command == "quit") break;
        std::cout << game.handle(command, in) << "\n" << std::flush;
    }
    return 0;
}
