// The first engine: Monte-Carlo tree search (UCT) with lightly guided random playouts.
// Root-parallel: every thread grows its own tree, the root statistics are added up.
// (Replaced by the alpha-beta engine, which is much stronger; kept for comparison: engine "mcts".)
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <thread>
#include "rules.hpp"

// Playout policy: random placements, preferring ones that grow an own snake.
inline Move playoutMove(const Position& pos, Rng& rng, std::vector<Move>& buffer) {
    PieceSet allowed = pos.piecesAllowedNow();
    if (!allowed) return Move::pass();
    int pieces[NUM_PIECES], pieceCount = 0;
    for (int p = 0; p < NUM_PIECES; p++)
        if (hasPiece(allowed, p)) pieces[pieceCount++] = p;
    Bitboard own = pos.ownCells();
    Move fallback = Move::pass();  // a legal move that starts a new snake
    bool haveFallback = false;
    for (int tries = 0; tries < 96; tries++) {
        PlacementRange candidates = placementsOf(pieces[rng.below(pieceCount)]);
        const Placement& p = candidates[rng.below((int)candidates.size())];
        if (!isLegalPlacement(pos, p)) continue;
        if (!own || (p.border & own)) return Move::place(p);
        if (!haveFallback) { fallback = Move::place(p); haveFallback = true; }
        if (tries > 40) return fallback;
    }
    if (haveFallback) return fallback;
    generateMoves(pos, buffer);  // random sampling found nothing: maybe there is no move at all
    if (buffer.empty()) return Move::pass();
    return buffer[rng.below((int)buffer.size())];
}

// Plays the game to the end; result for red: 1 win, 0.5 draw, 0 loss.
inline double playout(Position pos, Rng& rng, std::vector<Move>& buffer) {
    while (!pos.isOver()) pos.play(playoutMove(pos, rng, buffer));
    int lead = redLead(pos);
    return lead > 0 ? 1.0 : lead < 0 ? 0.0 : 0.5;
}

struct MctsNode {
    Move move;
    int parent;
    int firstChild = -1, childCount = 0;
    bool expanded = false;
    double wins = 0;  // sum of results for the player who made `move`
    int visits = 0;
};

struct MoveStats {
    Move move;
    int visits = 0;
    double wins = 0;  // for the side to move at the root
};

class MctsTree {
  public:
    std::vector<MctsNode> nodes;

    explicit MctsTree(uint64_t seed) : rng(seed) {}

    void run(const Position& root, double seconds, std::atomic<bool>* stop) {
        nodes.clear();
        nodes.reserve(1 << 20);
        nodes.push_back(MctsNode{Move::pass(), -1});
        std::vector<Move> buffer;
        auto end = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(seconds * 1e6));
        for (int iteration = 0;; iteration++) {
            if ((iteration & 63) == 0) {
                if (std::chrono::steady_clock::now() > end) break;
                if (stop && stop->load()) break;
                if (nodes.size() > 30000000) break;
            }
            Position pos = root;
            int leaf = select(pos);
            leaf = expand(leaf, pos, buffer);
            double redResult = playout(pos, rng, buffer);
            backPropagate(root, leaf, redResult);
        }
    }

  private:
    Rng rng;
    double exploration = 0.7;

    // Walk down by the UCB1 formula to a node that is not expanded yet.
    int select(Position& pos) {
        int current = 0;
        while (nodes[current].expanded && nodes[current].childCount > 0) {
            MctsNode& node = nodes[current];
            double logVisits = std::log((double)node.visits + 1);
            int best = -1;
            double bestValue = -1e9;
            for (int c = node.firstChild; c < node.firstChild + node.childCount; c++) {
                const MctsNode& child = nodes[c];
                double value = child.visits == 0 ? 1e6 + rng.uniform()
                                                 : child.wins / child.visits + exploration * std::sqrt(logVisits / child.visits);
                if (value > bestValue) { bestValue = value; best = c; }
            }
            current = best;
            pos.play(nodes[current].move);
        }
        return current;
    }

    // Add the children of a node visited often enough (the root right away) and step into one.
    int expand(int current, Position& pos, std::vector<Move>& buffer) {
        if (nodes[current].expanded || pos.isOver() || !(current == 0 || nodes[current].visits >= 3)) return current;
        if (current == 0) {
            buffer = symmetryUniqueMoves(pos);
        } else {
            generateMoves(pos, buffer);
            if (buffer.empty()) buffer.push_back(Move::pass());
        }
        int first = (int)nodes.size();
        for (Move m : buffer) nodes.push_back(MctsNode{m, current});
        nodes[current].firstChild = first;
        nodes[current].childCount = (int)buffer.size();
        nodes[current].expanded = true;
        int child = first + rng.below((int)buffer.size());
        pos.play(nodes[child].move);
        return child;
    }

    // Every move (a pass too) switches the side to move, so the mover of a node follows from
    // its depth.
    void backPropagate(const Position& root, int leaf, double redResult) {
        int depth = 0;
        for (int x = leaf; x > 0; x = nodes[x].parent) depth++;
        int mover = (root.sideToMove + depth - 1) & 1;
        for (int x = leaf; x >= 0; x = nodes[x].parent) {
            nodes[x].visits++;
            nodes[x].wins += mover == RED ? redResult : 1.0 - redResult;
            mover ^= 1;
        }
    }
};

inline std::vector<MoveStats> analyzeWithMcts(const Position& root, double seconds, int threads, std::atomic<bool>* stop = nullptr) {
    std::vector<std::unique_ptr<MctsTree>> trees;
    for (int i = 0; i < threads; i++)
        trees.emplace_back(new MctsTree(0x1234 + i * 7919 + (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; i++) pool.emplace_back([&, i] { trees[i]->run(root, seconds, stop); });
    for (auto& t : pool) t.join();

    std::map<int, MoveStats> total;  // by placement id
    for (auto& tree : trees) {
        auto& nodes = tree->nodes;
        if (nodes.empty() || !nodes[0].expanded) continue;
        for (int c = nodes[0].firstChild; c < nodes[0].firstChild + nodes[0].childCount; c++) {
            MoveStats& s = total[nodes[c].move.placementId];
            s.move = nodes[c].move;
            s.visits += nodes[c].visits;
            s.wins += nodes[c].wins;
        }
    }
    std::vector<MoveStats> result;
    for (auto& [id, s] : total) result.push_back(s);
    std::sort(result.begin(), result.end(), [](const MoveStats& a, const MoveStats& b) { return a.visits > b.visits; });
    return result;
}
