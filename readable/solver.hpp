// Exact solver: finds the game-theoretic value of a position (win / draw / loss for the side to
// move) with no heuristics in the result - every shortcut it takes is a proof.
//
// It is a negamax alpha-beta search over the values {-1, 0, +1}. What makes it fast:
//   - transposition table (transposition_table.hpp), keyed by the symmetry-canonical hash;
//   - snake-length bounds (bounds.hpp): a position is decided as soon as one player's sure
//     length beats the other's best possible length;
//   - the last pieces: once each player has at most one piece left, the value follows from the
//     placements without a search;
//   - tight bounds near the end: the exact one-player optimum (bounds.hpp) and the lengths both
//     players are sure to reach one and two plies ahead;
//   - threat zones: an opponent placement that would decide the game if the opponent could play
//     it right now has to be answered, so only our moves through its zone are searched;
//   - move ordering: table move, killer moves, simple shape heuristics and, while many pieces are
//     left, the learned evaluation of the child positions. With only three possible values a good
//     first move usually ends the search of a node immediately.
// There is no history heuristic: the scores it piled up over a long solve outweighed the shape
// heuristics (own snake first, blocks next) and cost nodes.
//
// solve() takes a window, so a yes/no question costs less than the exact value: (0, 1) asks
// "does the side to move win?" (result 1, or <= 0), (-1, 0) "does it avoid losing?" (>= 0, or -1).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include "evaluation.hpp"
#include "transposition_table.hpp"

// ---- the result of a finished game -----------------------------------------------------------

// SOLVER_RED_BONUS=b counts b extra cells for red at the end of the game (default 0: the real
// rules). With it, "blue still wins" means "blue wins by more than b", which finds the exact
// winning margin of a line.
inline const int RED_BONUS = envInt("SOLVER_RED_BONUS", 0);

// `player`'s share of the red bonus: added to its own length when it is compared with the other's.
inline int redBonusFor(int player) { return player == RED ? RED_BONUS : -RED_BONUS; }

// The result of a finished game for the side to move: +1 win, 0 draw, -1 loss.
inline int gameResult(const Position& pos) {
    int lead = redLead(pos) + RED_BONUS;
    int forRed = lead > 0 ? 1 : lead < 0 ? -1 : 0;
    return pos.sideToMove == RED ? forRed : -forRed;
}

// ---- the last pieces -------------------------------------------------------------------------

// Exact value of a position in which each player has at most one piece left, without a search:
// the side to move places its piece (or passes), then the opponent makes its longest snake with
// the placements that are still legal. A placement only ever takes opponent placements away (the
// cells it covers, and new snakes that would now touch it), never adds any. Returns false when a
// player has more pieces left.
inline bool lastPiecesValue(const Position& pos, int& value) {
    int me = pos.sideToMove, opp = me ^ 1;
    if (pieceCount(pos.piecesLeft[me]) > 1 || pieceCount(pos.piecesLeft[opp]) > 1 || pos.ply == 0) return false;
    int bonus = redBonusFor(me);
    auto result = [&](int myLength, int oppLength) {
        int lead = myLength + bonus - oppLength;
        return lead > 0 ? 1 : lead < 0 ? -1 : 0;
    };

    // the opponent's placements that would lengthen its longest snake, longest first
    struct Reply {
        Bitboard blockedBy;  // any of our cells here makes the reply illegal
        int length;
    };
    constexpr int MAX_REPLIES = 512;  // positions reachable in play have at most 468 (41+ cells filled)
    Reply replies[MAX_REPLIES];
    int replyCount = 0;
    bool overflow = false;
    if (pos.piecesLeft[opp]) {
        Position asOpponent = pos;
        asOpponent.sideToMove = opp;
        Bitboard occupied = pos.occupied();
        forEachLegalMove(asOpponent, [&](Move reply) {
            const Placement& p = reply.placement();
            int length = longestSnakeAfter(pos, opp, p);
            // a reply that grows an opponent snake is blocked only by covering it; a new snake
            // also by touching it
            if (length <= pos.longestSnake[opp]) return;
            if (replyCount == MAX_REPLIES) { overflow = true; return; }
            replies[replyCount++] = Reply{p.border & occupied ? p.cells : p.cells | p.border, length};
        });
        if (overflow) return false;  // more replies than the list holds: leave it to the search
        std::sort(replies, replies + replyCount, [](const Reply& a, const Reply& b) { return a.length > b.length; });
    }
    auto opponentLength = [&](Bitboard ourCells) {
        for (int i = 0; i < replyCount; i++)
            if (!(replies[i].blockedBy & ourCells)) return replies[i].length;
        return (int)pos.longestSnake[opp];
    };

    int best = -2;  // no placement tried yet
    if (pos.piecesLeft[me])
        forEachLegalMove(pos, [&](Move m) {
            if (best == 1) return;
            const Placement& p = m.placement();
            best = std::max(best, result(longestSnakeAfter(pos, me, p), opponentLength(p.cells)));
        });
    if (best == -2) best = result(pos.longestSnake[me], opponentLength(0));  // no piece or no legal placement: pass
    value = best;
    return true;
}

// ---- guaranteed growth -----------------------------------------------------------------------

// Lower bounds on both final snakes, one and two plies ahead. `legal`: every legal move of the
// side to move.
//  - The side to move reaches at least the longest snake of its best move now (myLength).
//  - The opponent reaches at least the length that none of our moves prevents (oppLength): some
//    placement of its that reaches it stays legal after any single move of ours (a grow placement
//    unless we cover it, a new snake unless we cover or touch it). Our move only takes opponent
//    placements away, never adds any; if we have to pass, every one stays.
inline void guaranteedGrowth(const Position& pos, const Move* legal, int legalCount, int& myLength, int& oppLength) {
    int me = pos.sideToMove, opp = me ^ 1;
    myLength = pos.longestSnake[me];
    for (int i = 0; i < legalCount; i++) myLength = std::max(myLength, longestSnakeAfter(pos, me, legal[i].placement()));
    oppLength = pos.longestSnake[opp];
    if (!pos.piecesLeft[opp]) return;

    // the opponent's placements that would lengthen its longest snake
    struct Extension {
        Bitboard blockedBy;  // any of our cells here makes it illegal
        int length;
    };
    constexpr int MAX_EXTENSIONS = 512;
    Extension extensions[MAX_EXTENSIONS];
    int count = 0;
    Position asOpponent = pos;
    asOpponent.sideToMove = opp;
    Bitboard occupied = pos.occupied();
    auto add = [&](Move m) {
        const Placement& p = m.placement();
        int length = longestSnakeAfter(pos, opp, p);
        if (length > pos.longestSnake[opp] && count < MAX_EXTENSIONS)
            extensions[count++] = Extension{p.border & occupied ? p.cells : p.cells | p.border, length};
    };
    // a new snake is longer only while the opponent's snakes are shorter than a piece
    if (pos.longestSnake[opp] >= PIECE_SIZE[PIECE_4C]) forEachGrowingMove(asOpponent, add);
    else forEachLegalMove(asOpponent, add);
    if (!count) return;
    std::sort(extensions, extensions + count, [](const Extension& a, const Extension& b) { return a.length > b.length; });

    oppLength = extensions[0].length;  // if we have to pass, every placement stays
    for (int i = 0; i < legalCount && oppLength > pos.longestSnake[opp]; i++) {
        Bitboard ours = legal[i].placement().cells;
        int best = pos.longestSnake[opp];  // the opponent's longest extension that our move leaves
        for (int j = 0; j < count && extensions[j].length > best; j++)
            if (!(extensions[j].blockedBy & ours)) { best = extensions[j].length; break; }
        oppLength = std::min(oppLength, best);
    }
}

// ---- threats ---------------------------------------------------------------------------------
//
// An opponent placement b is a threat when, if the opponent could play b right now as a free move,
// the snake bounds would already decide the game. Any move of ours that leaves b playable (does
// not cover b, and does not touch b when b starts a new snake) lets the opponent answer with b,
// and the bounds only tighten as cells fill up (our upper bound counts every cell we could still
// use, so it covers our own move too). Such moves are therefore worth at most the bounds' verdict,
// and only moves through the zone of every threat that is strong enough for the window need to be
// searched (the "must-play" region of Hex solvers): a move must meet every such zone.

struct Threat {
    Bitboard zone;  // our moves that cover none of these cells leave the threat playable
    int value;      // upper bound on our value after every such move
};

// The threats known at one node: from the killer moves one ply down (cheap, and often the very
// refutation) and, once the node's first move failed, from a scan of all opponent placements.
class ThreatList {
  public:
    static constexpr int NO_BOUND = 2;  // boundFor(): the threats do not bound the move

    int size() const { return count; }

    // Upper bound on our value for a move covering `cells`.
    int boundFor(Bitboard cells) const {
        int bound = NO_BOUND;
        for (int i = 0; i < count; i++)
            if (!(cells & threats[i].zone)) bound = std::min(bound, threats[i].value);
        return bound;
    }

    // Adds the opponent placement `placementId` (-1: none) if it is a threat. Each placement is
    // looked at only once per node. With `optimum`, our bound after the placement is the
    // one-player optimum, which is sound in the same way: whatever we play that leaves b playable,
    // every continuation is one that the relaxation after b allows.
    void tryPlacement(const Position& pos, int placementId, OnePlayerOptimum* optimum = nullptr) {
        if (placementId < 0 || count >= MAX_THREATS || triedCount >= MAX_TRIED) return;
        for (int i = 0; i < triedCount; i++)
            if (tried[i] == placementId) return;
        tried[triedCount++] = placementId;
        const Placement& b = pieceTables.placements[placementId];
        int value = threatValue(pos, b);
        if (optimum && (value == 0 || value == 1)) value = optimumThreatValue(pos, b, value, *optimum);
        if (value <= 0) threats[count++] = Threat{threatZone(pos, b), value};
    }

    // Adds the opponent placements that are threats for the window: our value after a move that
    // leaves them playable is at most alpha. `ours`: the clusters of the side to move, `bonus` its
    // share of the red bonus. Cheapest tests first: a placement lowers our bound only inside the
    // clusters it covers, so it has to cover every cluster whose bound is too high.
    void scan(const Position& pos, const SnakeClusters& ours, int alpha, int bonus) {
        int me = pos.sideToMove, opp = me ^ 1;
        int myLower = pos.longestSnake[me];
        int strict = alpha < 0 ? 1 : 0;  // a loss needs our bound strictly below the opponent's length
        // One placement joins at most two opponent snakes (a snake is a path and the piece one
        // segment of it), which caps the opponent's length after it; every cluster of ours with a
        // bound above what that allows has to be covered.
        int longest = 0, secondLongest = 0;
        for (int j = 0; j < pos.snakeCount[opp]; j++) {
            int length = countCells(pos.snakes[opp][j]);
            if (length > longest) {
                secondLongest = longest;
                longest = length;
            } else if (length > secondLongest) {
                secondLongest = length;
            }
        }
        int maxNeed = std::max<int>(pos.longestSnake[opp], 4 + longest + secondLongest) - bonus - strict;
        if (myLower > maxNeed) return;
        Bitboard mustCover = geometry.allCells;  // (no cluster above the cap: every placement)
        for (int i = 0; i < ours.count; i++)
            if (ours.upper[i] > maxNeed) { mustCover = ours.cells[i] & ~pos.cells[me]; break; }

        Threat found[MAX_FOUND];
        int foundCount = 0;
        Position asOpponent = pos;
        asOpponent.sideToMove = opp;
        forEachLegalMoveCovering(asOpponent, mustCover, [&](Move m) {
            if (foundCount >= MAX_FOUND) return;
            const Placement& b = m.placement();
            int oppLength = longestSnakeAfter(pos, opp, b);
            int need = oppLength - bonus - strict;  // our raw upper bound must not exceed this
            if (myLower > need) return;
            for (int i = 0; i < ours.count; i++)
                if (ours.upper[i] > need && !(ours.cells[i] & b.cells)) return;
            int myUpper = upperBoundAfterBlock(pos, me, ours, myLower, b.cells) + bonus;
            int value = myUpper < oppLength ? -1 : myUpper <= oppLength ? 0 : 1;
            if (value <= alpha) found[foundCount++] = Threat{threatZone(pos, b), value};
        });

        // smallest zones first: a zone that contains a kept one (with a value as low) prunes nothing more
        std::sort(found, found + foundCount, [](const Threat& a, const Threat& b) { return countCells(a.zone) < countCells(b.zone); });
        for (int f = 0; f < foundCount && count < MAX_THREATS; f++) {
            bool redundant = false;
            for (int i = 0; i < count; i++)
                if ((threats[i].zone & ~found[f].zone) == 0 && threats[i].value <= found[f].value) redundant = true;
            if (!redundant) threats[count++] = found[f];
        }
    }

  private:
    static constexpr int MAX_THREATS = 6;  // kept per node
    static constexpr int MAX_TRIED = 8;    // placements tried one by one per node
    static constexpr int MAX_FOUND = 64;   // threats one scan collects before keeping the best
    static constexpr int NOT_PLAYABLE = 2;
    Threat threats[MAX_THREATS];
    int count = 0;
    int tried[MAX_TRIED];
    int triedCount = 0;

    // The cells our move has to cover to make the opponent placement b illegal.
    static Bitboard threatZone(const Position& pos, const Placement& b) {
        bool growsSnake = b.border & pos.cells[pos.sideToMove ^ 1];
        return growsSnake ? b.cells : b.cells | b.border;
    }

    // threatValue() (0 or 1 there) with our one-player optimum after b instead of the bounds.
    static int optimumThreatValue(const Position& pos, const Placement& b, int value, OnePlayerOptimum& optimum) {
        int me = pos.sideToMove, opp = me ^ 1;
        int bonus = redBonusFor(me);
        Position after = pos;
        after.cells[opp] |= b.cells;
        SnakeClusters ours;
        snakeBounds(after, me, &ours);
        int oppLength = longestSnakeAfter(pos, opp, b);
        if (value == 1 && !optimum.mayReach(after, me, ours, oppLength - bonus + 1)) value = 0;
        if (value == 0 && !optimum.mayReach(after, me, ours, oppLength - bonus)) value = -1;
        return value;
    }

    // What the bounds tell about our value if the opponent played b now: -1 (a loss), 0 (at most
    // a draw) or 1 (nothing); NOT_PLAYABLE when the opponent cannot play b.
    static int threatValue(const Position& pos, const Placement& b) {
        int me = pos.sideToMove, opp = me ^ 1;
        if (!hasPiece(pos.piecesLeft[opp], b.piece) || !isLegalPlacementFor(pos, opp, b)) return NOT_PLAYABLE;
        Position after = pos;
        after.cells[opp] |= b.cells;  // enough for our bounds (the opponent's snakes are not needed)
        int myUpper = snakeBounds(after, me).upper + redBonusFor(me);
        int oppLength = longestSnakeAfter(pos, opp, b);
        return myUpper < oppLength ? -1 : myUpper <= oppLength ? 0 : 1;
    }
};

// ---- the solver ------------------------------------------------------------------------------

class Solver {
  public:
    static constexpr int UNKNOWN = -9;  // result when a time or node limit stopped the search

    // Move-ordering settings (environment variables override them for experiments).
    int evalOrderFromPieces = envInt("SOLVER_EVAL_REM", 7);  // order by evaluation with >= this many pieces left (both players)
    // Below that, order by evaluation only where the side to move does not look lost: the losing
    // side has to try every move anyway, so ordering its moves is wasted work.
    int evalOrderIfHopefulFromPieces = envInt("SOLVER_EVAL_WIN_REM", 7);
    double hopefulLogit = envInt("SOLVER_EVAL_WIN_THR", -100) / 100.0;
    int etcFromPieces = envInt("SOLVER_ETC_REM", 99);  // enhanced transposition cutoffs (off by default)
    // The tight bounds (the one-player optimum and guaranteed growth) cost more than they save
    // higher up: they are used at every node once at most TIGHT_BOUNDS_MAX_PIECES pieces are left in
    // total, and with up to ALL_NODE_TIGHT_BOUNDS_MAX_PIECES only once a node's first move failed,
    // where every remaining move would have to be searched.
    static constexpr int TIGHT_BOUNDS_MAX_PIECES = 5, ALL_NODE_TIGHT_BOUNDS_MAX_PIECES = 6;

    uint64_t nodes = 0;                        // nodes searched by the last solve()
    std::atomic<bool>* stopFlag = nullptr;     // set from outside to abort the search

    // Per-ply statistics (benchset with PROFILE=1).
    struct Profile {
        uint64_t visited[16] = {}, tableCut[16] = {}, lastPieces[16] = {}, boundCut[16] = {}, tightCut[16] = {}, expanded[16] = {};
        uint64_t movesListed[16] = {}, threatSkipped[16] = {};  // moves listed for the search / skipped by threat zones
        uint64_t threatScans[16] = {}, threatScansFound[16] = {};  // threat scans / scans that found a new threat
        void add(const Profile& other) {
            for (int ply = 0; ply < 16; ply++) {
                visited[ply] += other.visited[ply];
                tableCut[ply] += other.tableCut[ply];
                lastPieces[ply] += other.lastPieces[ply];
                boundCut[ply] += other.boundCut[ply];
                tightCut[ply] += other.tightCut[ply];
                expanded[ply] += other.expanded[ply];
                movesListed[ply] += other.movesListed[ply];
                threatSkipped[ply] += other.threatSkipped[ply];
                threatScans[ply] += other.threatScans[ply];
                threatScansFound[ply] += other.threatScansFound[ply];
            }
        }
    } profile;

    // log2Entries: table size (16 bytes per entry)
    explicit Solver(int log2Entries = 22) : table(std::make_shared<TranspositionTable>(log2Entries - 1)) { init(); }
    explicit Solver(std::shared_ptr<TranspositionTable> shared) : table(std::move(shared)) { init(); }

    // Forget what belongs to the last position solved and keep the table: before each position of a
    // run. Killer moves carried over from another position are tried first and cost nodes;
    // the one-player memo's answers depend on the order of its queries, so a fresh one keeps the node
    // counts of a position independent of the positions before it (on its own table).
    void newPosition() {
        onePlayer.clear();
        std::memset(killers, -1, sizeof killers);
    }
    // Forget everything learned (table, killers, one-player answers).
    void clear() {
        table->clear();
        newPosition();
    }

    const OnePlayerOptimum::Statistics& onePlayerStatistics() const { return onePlayer.stats; }
    void resetOnePlayerStatistics() { onePlayer.stats = OnePlayerOptimum::Statistics{}; }

    // Value for the side to move within the window (alpha, beta) - exact inside it, a bound
    // outside - or UNKNOWN when the limits stopped the search. seconds = 0: no time limit.
    int solve(const Position& pos, double seconds = 0, uint64_t maxNodes = UINT64_MAX, int alpha = -1, int beta = 1) {
        aborted = false;
        nodes = 0;
        nodeLimit = maxNodes;
        hasDeadline = seconds > 0;
        if (hasDeadline)
            deadline = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(seconds * 1e6));
        int value = search(pos, alpha, beta);
        return aborted ? UNKNOWN : value;
    }

  private:
    static constexpr int MAX_PLY = 32;
    static constexpr int MAX_MOVES = 3000;  // more than the legal moves of any position
    // Ordering scores of the table move and the killer moves: above every other score.
    static constexpr int TABLE_MOVE_SCORE = 1 << 30, KILLER1_SCORE = 1 << 29, KILLER2_SCORE = 1 << 28;
    // The first moves are picked one at a time (most nodes are decided by their first move); once
    // this many have failed, every move may be needed and the rest is sorted once.
    static constexpr int FULL_SORT_AFTER = 3;

    std::shared_ptr<TranspositionTable> table;
    int16_t killers[MAX_PLY][2];            // per ply: the last two moves that caused a cutoff
    std::vector<Move> moveBuffer[MAX_PLY];  // per-ply move lists, allocated once
    std::vector<Move> legalBuffer[MAX_PLY]; // per ply: every legal move, where the tight bounds list them
    OnePlayerOptimum onePlayer;
    std::vector<int> scoreBuffer[MAX_PLY];
    std::vector<uint64_t> sortKeyBuffer[MAX_PLY];

    std::chrono::steady_clock::time_point deadline;
    bool hasDeadline = false;
    bool aborted = false;
    uint64_t nodeLimit = UINT64_MAX;

    void init() {
        for (int i = 0; i < MAX_PLY; i++) {
            moveBuffer[i].resize(MAX_MOVES);
            legalBuffer[i].resize(MAX_MOVES);
            scoreBuffer[i].resize(MAX_MOVES);
            sortKeyBuffer[i].resize(MAX_MOVES);
        }
        std::memset(killers, -1, sizeof killers);
    }

    bool shouldAbort() {
        if (aborted) return true;
        if ((nodes & 4095) == 0) {  // checking the clock is not free
            if (stopFlag && stopFlag->load(std::memory_order_relaxed)) aborted = true;
            else if (hasDeadline && std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        if (nodes > nodeLimit) aborted = true;
        return aborted;
    }

    // ---- the search ----

    int search(const Position& pos, int alpha, int beta) {
        nodes++;
        if (shouldAbort()) return 0;
        if (pos.isOver()) return gameResult(pos);
        uint64_t nodesAtStart = nodes;
        int profilePly = std::min<int>(pos.ply, 15);
        profile.visited[profilePly]++;

        // 1. What is already known: the value lies in [lower, upper].
        int lower = -1, upper = 1;
        int symmetry;
        uint64_t key = pos.canonicalKey(symmetry);
        uint32_t keyCheck = pos.keyCheck();
        TTEntry stored{-1, 1, -1};
        int tableMove = -1;  // the stored best move, in this position's orientation
        if (table->probe(key, keyCheck, stored)) {
            lower = stored.lower;
            upper = stored.upper;
            if (lower >= upper || lower >= beta) { profile.tableCut[profilePly]++; return lower; }
            if (upper <= alpha) { profile.tableCut[profilePly]++; return upper; }
            if (stored.bestMove >= 0) tableMove = pieceTables.symmetricPlacement[stored.bestMove][geometry.inverseSymmetry[symmetry]];
        }

        // 2. The last pieces: the exact value without a search.
        int lastValue;
        if (lastPiecesValue(pos, lastValue)) {
            profile.lastPieces[profilePly]++;
            table->store(key, keyCheck, lastValue, lastValue, stored.bestMove, 0);
            return lastValue;
        }

        // 3. The snake-length bounds.
        int piecesLeft = pieceCount(pos.piecesLeft[RED]) + pieceCount(pos.piecesLeft[BLUE]);
        bool tight = piecesLeft <= TIGHT_BOUNDS_MAX_PIECES;
        SnakeClusters myClusters, oppClusters;  // kept for the threat scan and the tight bounds
        SnakeBounds mine, theirs;
        narrowByBounds(pos, lower, upper, &myClusters, tight ? &oppClusters : nullptr, mine, theirs);
        if (lower >= upper || lower >= beta) { profile.boundCut[profilePly]++; return lower; }
        if (upper <= alpha) { profile.boundCut[profilePly]++; return upper; }

        // 3b. Near the end, the tight bounds. They need every legal move of ours, which step 5 then reuses.
        Move* legal = tight ? legalBuffer[pos.ply & 31].data() : nullptr;
        int legalCount = 0;
        if (tight) {
            forEachLegalMove(pos, [&](Move m) { legal[legalCount++] = m; });
            narrowByTightBounds(pos, legal, legalCount, lower, upper, alpha, beta, mine, theirs, myClusters, oppClusters);
            if (lower >= upper || lower >= beta || upper <= alpha) {
                profile.tightCut[profilePly]++;
                table->store(key, keyCheck, lower, upper, stored.bestMove, 0);
                return lower >= upper || lower >= beta ? lower : upper;
            }
        }
        profile.expanded[profilePly]++;
        alpha = std::max(alpha, lower);
        beta = std::min(beta, upper);
        int alphaAtStart = alpha, betaAtStart = beta;

        // 4. Threats: the opponent's refutations one ply down are the natural candidates. A move
        //    that leaves a threat playable is worth at most the threat's value, and is skipped when
        //    that cannot raise alpha.
        int ply = std::min<int>(pos.ply, MAX_PLY - 1);
        int bonus = redBonusFor(pos.sideToMove);
        bool threatsOn = pos.piecesLeft[pos.sideToMove ^ 1] && ply < MAX_PLY - 1;
        ThreatList threats;
        if (threatsOn) {
            threats.tryPlacement(pos, killers[ply + 1][0], tight ? &onePlayer : nullptr);
            threats.tryPlacement(pos, killers[ply + 1][1], tight ? &onePlayer : nullptr);
        }
        int skipped = -2;  // the best bound of the moves skipped because of threats
        auto skipByThreats = [&](Move m) {
            if (threats.size() == 0) return false;
            int bound = threats.boundFor(m.placement().cells);
            if (bound > alpha) return false;
            skipped = std::max(skipped, bound);
            profile.threatSkipped[profilePly]++;
            return true;
        };

        // 5. Generate and score the moves.
        Move* moves = moveBuffer[pos.ply & 31].data();
        int* scores = scoreBuffer[pos.ply & 31].data();
        int count = scoreMoves(pos, tableMove, legal, legalCount, moves, scores, skipByThreats);
        bool orderByEval = count > 1 && (piecesLeft >= evalOrderFromPieces ||
                                         (piecesLeft >= evalOrderIfHopefulFromPieces && learnedLogit(pos) > hopefulLogit));
        bool tryEtc = count > 0 && piecesLeft >= etcFromPieces;
        // Moves scored at least `ahead` (the table move and the killers) stay in front of the
        // evaluation ordering. The evaluation is expensive: when such a move exists it is searched
        // first, and the others are evaluated only if it fails.
        constexpr int ahead = KILLER2_SCORE;
        bool evaluateLater = false;
        if (orderByEval && !tryEtc)
            for (int i = 0; i < count && !evaluateLater; i++) evaluateLater = scores[i] >= ahead;
        if (evaluateLater) orderByEval = false;
        if (tryEtc) {
            int cutoffValue;
            if (transpositionCutoff(pos, moves, count, beta, key, symmetry, lower, upper, cutoffValue)) return cutoffValue;
        }
        if (orderByEval) scoreByEvaluation(pos, moves, scores, 0, count, ahead);
        profile.movesListed[profilePly] += count;

        // 6. Search the moves, best-scored first.
        bool scanned = !threatsOn;
        bool allNodeChecked = tight || piecesLeft > ALL_NODE_TIGHT_BOUNDS_MAX_PIECES;
        int best = -2, bestMove = -1;
        if (count == 0 && skipped == -2) {
            Position child = pos;
            child.play(Move::pass());
            best = -search(child, -beta, -alpha);
        } else {
            for (int i = 0; i < count; i++) {
                if (evaluateLater && i == 1) {
                    scoreByEvaluation(pos, moves, scores, 1, count, ahead);
                    evaluateLater = false;
                }
                bringNextToFront(moves, scores, i, count, pos.ply);
                if (skipByThreats(moves[i])) continue;
                Position child = pos;
                child.play(moves[i]);
                int value = -search(child, -beta, -alpha);
                if (aborted) return 0;
                if (value > best) { best = value; bestMove = moves[i].placementId; }
                if (value > alpha) alpha = value;
                if (alpha >= beta) {
                    rememberKiller(ply, moves[i]);
                    break;
                }
                // The first move failed: above the tight bounds' gate, the tight bounds over the
                // moves left may settle them all at once.
                if (!allNodeChecked && i + 2 < count) {
                    allNodeChecked = true;
                    int restLower = -1, restUpper = 1;
                    snakeBounds(pos, pos.sideToMove ^ 1, &oppClusters);
                    narrowByTightBounds(pos, moves + i + 1, count - i - 1, restLower, restUpper, alpha, beta, mine, theirs, myClusters,
                                        oppClusters);
                    if (restUpper <= alpha) {  // no move left can raise alpha
                        profile.tightCut[profilePly]++;
                        best = std::max(best, restUpper);
                        break;
                    }
                    if (restLower >= beta) {  // one of them reaches beta
                        profile.tightCut[profilePly]++;
                        best = restLower;
                        break;
                    }
                }
                // The refutation just found may be a threat against the remaining moves. And once
                // the first move failed, this is probably a node where every move must be refuted,
                // so look for all of the opponent's threats.
                if (threatsOn && i + 2 < count) {
                    threats.tryPlacement(pos, killers[ply + 1][0], tight ? &onePlayer : nullptr);
                    if (!scanned) {
                        scanned = true;
                        int before = threats.size();
                        threats.scan(pos, myClusters, alpha, bonus);
                        profile.threatScans[profilePly]++;
                        if (threats.size() > before) profile.threatScansFound[profilePly]++;
                    }
                }
            }
            best = std::max(best, skipped);
        }
        if (aborted) return 0;

        // 7. Store what we learned.
        int newLower = lower, newUpper = upper;
        if (best <= alphaAtStart) newUpper = std::min(newUpper, best);      // failed low: an upper bound
        else if (best >= betaAtStart) newLower = std::max(newLower, best);  // failed high: a lower bound
        else newLower = newUpper = best;                                    // exact
        uint64_t work = nodes - nodesAtStart + 1;
        int workLog2 = 63 - __builtin_clzll(work);
        int canonicalBest = bestMove >= 0 ? pieceTables.symmetricPlacement[bestMove][symmetry] : stored.bestMove;
        table->store(key, keyCheck, newLower, newUpper, canonicalBest, workLog2);
        return best;
    }

    // Snake-length bounds: if my sure length beats the opponent's best possible one I have won,
    // and so on. The players' clusters are kept in `myClusters` and, if given, `oppClusters`; `me`
    // (with our share of the red bonus) and `opp` get the bounds.
    static void narrowByBounds(const Position& pos, int& lower, int& upper, SnakeClusters* myClusters, SnakeClusters* oppClusters,
                               SnakeBounds& me, SnakeBounds& opp) {
        me = snakeBounds(pos, pos.sideToMove, myClusters);
        opp = snakeBounds(pos, pos.sideToMove ^ 1, oppClusters);
        int bonus = redBonusFor(pos.sideToMove);
        me.lower += bonus;
        me.upper += bonus;
        int boundLower = me.lower > opp.upper ? 1 : me.lower >= opp.upper ? 0 : -1;
        int boundUpper = me.upper < opp.lower ? -1 : me.upper <= opp.lower ? 0 : 1;
        lower = std::max(lower, boundLower);
        upper = std::min(upper, boundUpper);
    }

    // The tight bounds: guaranteed growth raises the sure lengths, then the one-player optimum is
    // asked only the questions whose answer would end the node in the window (alpha, beta).
    // `me` includes our share of the red bonus. `legal`: every legal move of ours, or only some of
    // them: then [lower, upper] bounds the best of those (the growth bounds hold move by move).
    void narrowByTightBounds(const Position& pos, const Move* legal, int legalCount, int& lower, int& upper, int alpha, int beta,
                             SnakeBounds me, SnakeBounds opp, const SnakeClusters& myClusters, const SnakeClusters& oppClusters) {
        int mover = pos.sideToMove, other = mover ^ 1;
        int bonus = redBonusFor(mover);
        int myLength, oppLength;
        guaranteedGrowth(pos, legal, legalCount, myLength, oppLength);
        me.lower = std::max(me.lower, myLength + bonus);
        opp.lower = std::max(opp.lower, oppLength);
        lower = std::max(lower, me.lower > opp.upper ? 1 : me.lower >= opp.upper ? 0 : -1);
        upper = std::min(upper, me.upper < opp.lower ? -1 : me.upper <= opp.lower ? 0 : 1);
        auto decided = [&] { return lower >= upper || lower >= beta || upper <= alpha; };
        if (decided()) return;
        // our optimum against the opponent's sure length lowers the upper bound: to 0 if we cannot
        // pass it (a cutoff only when alpha >= 0 or lower >= 0), to -1 if we cannot reach it
        if (upper == 1 && (alpha >= 0 || lower >= 0) && !onePlayer.mayReach(pos, mover, myClusters, opp.lower - bonus + 1)) upper = 0;
        if (decided()) return;
        if ((upper == 0 || alpha == -1) && !onePlayer.mayReach(pos, mover, myClusters, opp.lower - bonus)) upper = -1;
        if (decided()) return;
        // the opponent's optimum against our sure length raises the lower bound the same way
        if (lower == -1 && (beta <= 0 || upper <= 0) && !onePlayer.mayReach(pos, other, oppClusters, me.lower + 1)) lower = 0;
        if (decided()) return;
        if ((lower == 0 || beta == 1) && !onePlayer.mayReach(pos, other, oppClusters, me.lower)) lower = 1;
    }

    // The moves that `skip` lets through, with cheap ordering scores; returns their number. From
    // `legal` if the legal moves are listed there already, else generated.
    template <class Skip>
    int scoreMoves(const Position& pos, int tableMove, const Move* legal, int legalCount, Move* moves, int* scores, Skip&& skip) {
        Bitboard own = pos.ownCells(), opp = pos.opponentCells();
        Bitboard opponentFront = neighboursOf(snakeShapeOf(opp).ends) & pos.emptyCells();
        int ply = std::min<int>(pos.ply, MAX_PLY - 1);
        int killer1 = killers[ply][0], killer2 = killers[ply][1];
        int count = 0;
        auto add = [&](Move m) {
            if (skip(m)) return;
            const Placement& p = m.placement();
            int score = PIECE_SIZE[p.piece] * 64;      // big pieces first
            if (p.border & own) score += 256;           // grows an own snake
            if (p.cells & opponentFront) score += 128;  // blocks an opponent snake end
            else if (p.border & opp) score += 32;       // at least touches the opponent
            if (m.placementId == tableMove) score = TABLE_MOVE_SCORE;
            else if (m.placementId == killer1) score = KILLER1_SCORE;
            else if (m.placementId == killer2) score = KILLER2_SCORE;
            moves[count] = m;
            scores[count] = score;
            count++;
        };
        if (legal)
            for (int i = 0; i < legalCount; i++) add(legal[i]);
        else
            forEachLegalMove(pos, add);
        return count;
    }

    // Enhanced transposition cutoff: a child already known (game over, or in the table) to be good
    // enough for a cutoff decides the node without a search. Returns true with cutoffValue set then.
    bool transpositionCutoff(const Position& pos, const Move* moves, int count, int beta, uint64_t key, int symmetry, int lower,
                             int upper, int& cutoffValue) {
        for (int i = 0; i < count; i++) {
            Position child = pos;
            child.play(moves[i]);
            int value = UNKNOWN;
            if (child.isOver()) {
                value = -gameResult(child);
            } else {
                int childSymmetry;
                TTEntry e;
                if (table->probe(child.canonicalKey(childSymmetry), child.keyCheck(), e)) value = -e.upper;  // our value >= -upper
            }
            if (value >= beta) {
                table->store(key, pos.keyCheck(), std::max(lower, value), upper, pieceTables.symmetricPlacement[moves[i].placementId][symmetry], 0);
                cutoffValue = value;
                return true;
            }
        }
        return false;
    }

    // Re-scores moves[from..count) by the learned evaluation of the child position, except the
    // moves scored at least `ahead`: they keep their place in front.
    static void scoreByEvaluation(const Position& pos, const Move* moves, int* scores, int from, int count, int ahead) {
        for (int i = from; i < count; i++)
            if (scores[i] < ahead) {
                Position child = pos;
                child.play(moves[i]);
                scores[i] = child.isOver() ? -gameResult(child) * 100000 : -(int)(1000 * learnedLogit(child));
            }
    }

    // Puts the move to search next at position i: the best-scored of the rest, picked one at a
    // time for the first FULL_SORT_AFTER moves, then from a sort of the rest at once.
    void bringNextToFront(Move* moves, int* scores, int i, int count, int ply) {
        if (i < FULL_SORT_AFTER) bringBestToFront(moves, scores, i, count);
        else if (i == FULL_SORT_AFTER) sortByScore(moves, scores, i, count, sortKeyBuffer[ply & 31].data());
    }

    // Selection sort step: moves the best-scored of moves[i..count) to position i (the earlier
    // move on ties).
    static void bringBestToFront(Move* moves, int* scores, int i, int count) {
        int best = i;
        for (int j = i + 1; j < count; j++)
            if (scores[j] > scores[best]) best = j;
        std::swap(moves[i], moves[best]);
        std::swap(scores[i], scores[best]);
    }

    // Sorts moves[from..count) by score, best first, and equal scores by placement id, highest
    // first. Score and id are packed into one integer (the score with its sign bit flipped, so
    // that unsigned order is signed order), which makes the order independent of the sort.
    static void sortByScore(Move* moves, int* scores, int from, int count, uint64_t* keys) {
        for (int j = from; j < count; j++) keys[j] = uint64_t(uint32_t(scores[j]) ^ 0x80000000u) << 16 | uint16_t(moves[j].placementId);
        std::sort(keys + from, keys + count, std::greater<uint64_t>());
        for (int j = from; j < count; j++) {
            moves[j].placementId = int16_t(keys[j] & 0xFFFF);
            scores[j] = int(uint32_t(keys[j] >> 16) ^ 0x80000000u);
        }
    }

    void rememberKiller(int ply, Move m) {
        if (killers[ply][0] != m.placementId) {
            killers[ply][1] = killers[ply][0];
            killers[ply][0] = m.placementId;
        }
    }
};
