// Refutation sweeps from the command line (see refute.hpp): refute, proveline, uniquemoves.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "commands.hpp"
#include "notation.hpp"
#include "refute.hpp"

// The position after argv[firstMove...]; false (after printing which move) if a move is illegal.
static bool playMoves(const Args& args, int firstMove, Position& pos) {
    pos = Position::initial();
    for (int i = firstMove; i < args.argc; i++)
        if (!playMoveText(pos, args.argv[i])) {
            printf("illegal move %s\n", args.argv[i]);
            return false;
        }
    return true;
}

// The side to move's symmetry-unique moves, best first: the `top` best of a `searchSeconds` engine
// search (multi-PV), then the rest in the evaluation's order (0 seconds: the evaluation's order
// only). winProbability is the mover's, by the search (its top moves) or by the evaluation;
// *depth (if given) gets the depth the search completed (0 without a search).
struct RankedMove {
    Move move;
    double winProbability;
    bool searched;
};
static std::vector<RankedMove> movesBestFirst(const Position& pos, double searchSeconds, int top, int threads, int* depth = nullptr) {
    std::vector<std::pair<double, Move>> byEvaluation;
    for (Move m : symmetryUniqueMoves(pos)) {
        Position child = pos;
        child.play(m);
        byEvaluation.push_back({child.isOver() ? -gameResult(child) * 1000.0 : -learnedLogit(child), m});  // the mover's view
    }
    std::stable_sort(byEvaluation.begin(), byEvaluation.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<RankedMove> ranked;
    if (depth) *depth = 0;
    if (searchSeconds > 0 && top > 0 && byEvaluation.size() > 1) {
        EngineAnalysis analysis = analyzeWithEngine(pos, searchSeconds, threads, top);
        if (depth) *depth = analysis.depth;
        for (size_t i = 0; i < analysis.moves.size() && (int)i < top; i++)
            ranked.push_back({analysis.moves[i].move, scoreToWinProb(analysis.moves[i].score), true});
    }
    for (auto& [logit, m] : byEvaluation) {
        bool searched = std::any_of(ranked.begin(), ranked.end(), [&](const RankedMove& r) { return r.move == m; });
        if (!searched) ranked.push_back({m, 1.0 / (1.0 + std::exp(-logit)), false});
    }
    return ranked;
}

// ---- refute ----------------------------------------------------------------------------------

// First line of a refute log: the position, the question (REFUTE_LOSS) and SOLVER_RED_BONUS.
static std::string refuteLogHeader(const Position& pos) {
    return "# refute " + positionToString(pos) + " loss " + std::to_string(envInt("REFUTE_LOSS", 0) != 0) + " bonus " + std::to_string(RED_BONUS);
}

// snek refute <threads> <log> <move>...
// Does the side to move after the given moves win, or can each of its moves be refuted? With env
// REFUTE_LOSS=1: does it avoid losing, or can each move be refuted with a loss? REFUTE_FIRST=<file>
// lists moves (one per line) to settle before all others.
int cmdRefute(const Args& args) {
    if (!args.has(4)) { printf("usage: snek refute <threads> <log> <moves...>\n"); return 1; }
    int threads = args.arg(2, 1);
    Position pos;
    if (!playMoves(args, 4, pos)) return 1;
    std::vector<std::string> first;
    if (const char* path = getenv("REFUTE_FIRST")) {
        std::ifstream in(path);
        std::string move;
        while (in >> move) first.push_back(move);
    }
    bool mustLose = envInt("REFUTE_LOSS", 0) != 0;
    RefuteResult r = refuteMoves(pos, threads, args.arg(3, ""), refuteLogHeader(pos), first);
    if (!r.error.empty()) { printf("%s\n", r.error.c_str()); return 1; }
    if (!r.survivor.empty()) printf("the side to move %s with %s\n", mustLose ? "does not lose" : "WINS", r.survivor.c_str());
    else if (r.refuted == r.total) printf("the side to move %s: all %zu moves refuted\n", mustLose ? "LOSES" : "cannot win", r.total);
    else printf("%zu of %zu moves refuted, rest unknown\n", r.refuted, r.total);
    return 0;
}

// ---- proveline -------------------------------------------------------------------------------

// fopen(path, mode), or nullptr after printing "cannot write <path>".
static FILE* openForWriting(const std::string& path, const char* mode) {
    FILE* file = fopen(path.c_str(), mode);
    if (!file) printf("cannot write %s\n", path.c_str());
    return file;
}

// snek proveline <threads> <dir> <move>...
// Settles every reply of the side to move after the given moves (the defender) with a refutation
// sweep for the opponent (the attacker): an attacker answer that wins against all defender replies
// after it, or proof that the attacker cannot win after that reply. Answers that won against
// earlier replies are tried first (the same answer often wins again). The defender's best replies
// come first, so a reply that breaks the line shows up early; one such reply settles the line, and
// the command stops there unless PROVELINE_ALL=1. The order: the 8 best of a PROVELINE_SEARCH second
// engine search (multi-PV, as refute's REFUTE_SEARCH), then the evaluation's; it is kept in
// dir/order.txt so that a rerun takes the same order (PROVELINE_SEARCH=0, the default: the
// evaluation's order only).
//
// dir/summary.txt gets a line per settled reply, and a rerun continues after them:
//   <reply> wins <answer> <seconds>
//   <reply> holds <seconds>                      (the attacker cannot win after this reply)
// dir/<reply>.log (':' and ',' as '_') are the sweeps' resumable logs. Each sweep starts with a
// line "reply <rank>/<count> <reply>" on stdout.
int cmdProveLine(const Args& args) {
    if (!args.has(4)) { printf("usage: snek proveline <threads> <dir> <moves...>\n"); return 1; }
    if (envInt("REFUTE_LOSS", 0)) { printf("proveline proves wins; REFUTE_LOSS is for refute only\n"); return 1; }
    int threads = args.arg(2, 1);
    std::string dir = args.arg(3, "");
    Position pos;
    if (!playMoves(args, 4, pos)) return 1;
    std::error_code directoryError;  // a directory that cannot be made shows up as summary.txt not opening
    std::filesystem::create_directories(dir, directoryError);
    std::string summaryPath = dir + "/summary.txt";
    std::string header = "# proveline " + positionToString(pos) + " bonus " + std::to_string(RED_BONUS);
    std::string existing = firstLineOf(summaryPath);
    if (!existing.empty() && existing != header) {
        printf("%s starts with \"%s\", not \"%s\"\n", summaryPath.c_str(), existing.c_str(), header.c_str());
        return 1;
    }
    {  // summary.txt has to be writable; a new one starts with the header
        FILE* summary = openForWriting(summaryPath, "a");
        if (!summary) return 1;
        if (existing.empty()) fprintf(summary, "%s\n", header.c_str());
        fclose(summary);
    }

    // what earlier runs settled, and the answers that won (most recent first)
    std::map<std::string, std::string> settled;  // defender reply -> "wins" or "holds"
    std::vector<std::string> winningAnswers;
    {
        std::ifstream in(summaryPath);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string reply, state, answer;
            if (line[0] == '#' || !(fields >> reply >> state)) continue;  // the header
            settled[reply] = state;
            if (state == "wins" && fields >> answer && std::find(winningAnswers.begin(), winningAnswers.end(), answer) == winningAnswers.end())
                winningAnswers.insert(winningAnswers.begin(), answer);
        }
    }

    // the defender's replies, best first (an order kept by an earlier run comes first)
    int searchSeconds = envInt("PROVELINE_SEARCH", 0);
    std::string orderPath = dir + "/order.txt";
    std::vector<RankedMove> ranked = movesBestFirst(pos, 0, 0, threads);
    auto isReply = [&](Move m) { return std::any_of(ranked.begin(), ranked.end(), [&](const RankedMove& r) { return r.move == m; }); };
    std::vector<Move> replies;
    if (searchSeconds > 0) {
        std::ifstream in(orderPath);
        std::string text;
        Move m;
        while (in >> text)
            if (parseMove(text, m) && isReply(m) && std::find(replies.begin(), replies.end(), m) == replies.end()) replies.push_back(m);
        if (replies.empty()) {
            int depth;
            ranked = movesBestFirst(pos, searchSeconds, 8, threads, &depth);
            printf("order: %zu replies, the 8 best of a %d s search (depth %d) first\n", ranked.size(), searchSeconds, depth);
            FILE* order = openForWriting(orderPath, "w");
            if (!order) return 1;
            for (auto& r : ranked) fprintf(order, "%s\n", moveToString(r.move).c_str());
            fclose(order);
        }
    }
    for (auto& r : ranked)
        if (std::find(replies.begin(), replies.end(), r.move) == replies.end()) replies.push_back(r.move);

    size_t won = 0, held = 0, rank = 0;
    bool all = envInt("PROVELINE_ALL", 0) != 0;
    for (Move m : replies) {
        if (held && !all) break;  // a reply that holds settles the line
        rank++;
        std::string reply = moveToString(m);
        if (settled.count(reply)) {
            won += settled[reply] == "wins";
            held += settled[reply] == "holds";
            continue;
        }
        printf("reply %zu/%zu %s\n", rank, replies.size(), reply.c_str());
        fflush(stdout);
        Position afterReply = pos;
        afterReply.play(m);
        std::string logName = reply;
        std::replace(logName.begin(), logName.end(), ':', '_');
        std::replace(logName.begin(), logName.end(), ',', '_');
        auto start = std::chrono::steady_clock::now();
        RefuteResult r = refuteMoves(afterReply, threads, dir + "/" + logName + ".log", refuteLogHeader(afterReply), winningAnswers);
        if (!r.error.empty()) { printf("%s\n", r.error.c_str()); return 1; }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        bool attackerWins = !r.survivor.empty(), attackerCannotWin = r.refuted == r.total;
        FILE* summary = openForWriting(summaryPath, "a");
        if (!summary) return 1;
        if (attackerWins) {
            fprintf(summary, "%s wins %s %.0f\n", reply.c_str(), r.survivor.c_str(), seconds);
            winningAnswers.erase(std::remove(winningAnswers.begin(), winningAnswers.end(), r.survivor), winningAnswers.end());
            winningAnswers.insert(winningAnswers.begin(), r.survivor);
            won++;
        } else if (attackerCannotWin) {
            fprintf(summary, "%s holds %.0f\n", reply.c_str(), seconds);
            held++;
        }
        fclose(summary);
        std::string verdict = attackerWins ? "answer " + r.survivor : attackerCannotWin ? "the attacker cannot win" : "unknown";
        printf("[%zu/%zu] %s: %s (%.0f s; %zu won, %zu held)\n", won + held, replies.size(), reply.c_str(), verdict.c_str(), seconds, won, held);
        fflush(stdout);
    }
    printf("%zu replies: %zu with a winning answer, %zu where the attacker cannot win, %zu open\n", replies.size(), won, held,
           replies.size() - won - held);
    if (held) printf("the attacker does not win this line\n");
    else if (won == replies.size()) printf("the attacker WINS this line\n");
    return 0;
}

// ---- uniquemoves -----------------------------------------------------------------------------

// snek uniquemoves <seconds> <top> <threads> <move>... [-- <move>...]
// The side to move's symmetry-unique moves, best first (the <top> best of a <seconds> engine search,
// then the evaluation's order), one per line "<move> <the mover's win probability> search|eval",
// after a line "# position <position> moves <count> depth <search depth>". Moves listed after "--"
// are mapped to their representative among them: "map <move> <representative>" or
// "map <move> illegal".
int cmdUniqueMoves(const Args& args) {
    if (!args.has(5)) { printf("usage: snek uniquemoves <seconds> <top> <threads> <moves...> [-- <move>...]\n"); return 1; }
    double seconds = args.arg(2, 0.0);
    int top = args.arg(3, 0), threads = args.arg(4, 1);
    Position pos = Position::initial();
    int i = 5;
    for (; i < args.argc && std::string(args.argv[i]) != "--"; i++)
        if (!playMoveText(pos, args.argv[i])) {
            printf("illegal move %s\n", args.argv[i]);
            return 1;
        }
    int depth;
    std::vector<RankedMove> ranked = movesBestFirst(pos, seconds, top, threads, &depth);
    printf("# position %s moves %zu depth %d\n", positionToString(pos).c_str(), ranked.size(), depth);
    for (auto& r : ranked) printf("%s %.4f %s\n", moveToString(r.move).c_str(), r.winProbability, r.searched ? "search" : "eval");

    std::map<uint64_t, Move> representative;  // the position after a move, up to symmetry -> its move here
    for (auto& r : ranked) {
        Position child = pos;
        child.play(r.move);
        int symmetry;
        representative[child.canonicalKey(symmetry)] = r.move;
    }
    for (i++; i < args.argc; i++) {
        Position child = pos;
        std::string mapped = "illegal";
        if (playMoveText(child, args.argv[i])) {
            int symmetry;
            auto it = representative.find(child.canonicalKey(symmetry));
            if (it != representative.end()) mapped = moveToString(it->second);
        }
        printf("map %s %s\n", args.argv[i], mapped.c_str());
    }
    return 0;
}
