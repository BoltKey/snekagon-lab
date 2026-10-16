// Exact solver: negamax alpha-beta over {-1, 0, +1} (loss/draw/win for the side
// to move), transposition table keyed by the symmetry-canonical hash, snake-length
// bounds for early cutoffs, threat zones, an exact closed form for the last pieces,
// TT-move + killer + static + learned-eval move ordering, enhanced transposition cutoffs
// (ETC). The TT is lockless and can be shared by threads.
// There is no history heuristic: the scores it accumulated over a long solve outweighed
// the static ordering (own snake first, blocks next) and cost nodes.
#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include "core.hpp"
#include "eval.hpp"

// Lockless 2-way transposition table. Each slot stores key^data and data, so an entry
// torn by concurrent writes fails the key check instead of returning garbage.
// Slot 0 of a bucket keeps the entry with the most work behind it, slot 1 is always replaced.
// The key bits that pick the bucket verify nothing, so an entry also carries the position's 32-bit
// keyCheck(): without it a 2^25-entry table would take a different position for the probed one about
// once in 2^39 probes (40 bits checked, two slots), which a proof running 10^12 nodes cannot afford.
struct LTT {
    struct Slot { std::atomic<uint64_t> k{0}, d{0}; };
    std::unique_ptr<Slot[]> s;
    size_t nb;
    uint64_t mask;
    explicit LTT(int log2buckets) : s(new Slot[size_t(2) << log2buckets]), nb(size_t(1) << log2buckets), mask(nb - 1) {}
    void clear() {
        for (size_t i = 0; i < 2 * nb; i++) { s[i].k.store(0, std::memory_order_relaxed); s[i].d.store(0, std::memory_order_relaxed); }
    }
    // data: bits 0-2 lo + 2, 3-5 hi + 2, 6-21 best + 1, 22-27 work, 32-63 the key check
    static uint64_t pack(int lo, int hi, int best, int work, uint32_t check) {
        return uint64_t(lo + 2) | uint64_t(hi + 2) << 3 | uint64_t(best + 1) << 6 | uint64_t(work) << 22 | uint64_t(check) << 32;
    }
    static bool holds(uint64_t k, uint64_t d, uint64_t key, uint32_t check) { return (k ^ d) == key && uint32_t(d >> 32) == check; }
    void prefetch(uint64_t key) const { __builtin_prefetch(&s[(key & mask) * 2]); }
    bool probe(uint64_t key, uint32_t check, int& lo, int& hi, int& best) const {
        size_t b = (key & mask) * 2;
        for (int i = 0; i < 2; i++) {
            uint64_t k = s[b + i].k.load(std::memory_order_relaxed), d = s[b + i].d.load(std::memory_order_relaxed);
            if (d && holds(k, d, key, check)) {
                lo = int(d & 7) - 2; hi = int(d >> 3 & 7) - 2; best = int(d >> 6 & 0xFFFF) - 1;
                return true;
            }
        }
        return false;
    }
    void store(uint64_t key, uint32_t check, int lo, int hi, int best, int work) {
        size_t b = (key & mask) * 2;
        uint64_t d = pack(lo, hi, best, work, check);
        uint64_t k0 = s[b].k.load(std::memory_order_relaxed), d0 = s[b].d.load(std::memory_order_relaxed);
        int i = (holds(k0, d0, key, check) || work >= int(d0 >> 22 & 63)) ? 0 : 1;
        s[b + i].d.store(d, std::memory_order_relaxed);
        s[b + i].k.store(key ^ d, std::memory_order_relaxed);
    }
};

inline int envInt(const char* name, int def) { const char* v = getenv(name); return v ? atoi(v) : def; }

// The table of a run over many positions (a refutation sweep, a distributed worker): one table for the
// whole run, shared by its threads, so that sibling positions reuse each other's subtrees.
// SOLVER_SWEEP_TT=<log2 entries> of 16 bytes: the default 2^25 entries is 512 MB per process.
constexpr int SWEEP_TT_LOG2 = 25;
inline std::shared_ptr<LTT> makeSweepTable() { return std::make_shared<LTT>(envInt("SOLVER_SWEEP_TT", SWEEP_TT_LOG2) - 1); }
// SOLVER_RED_BONUS=b counts b extra cells for red at the end of the game (default 0 = the real rules).
// With it, "blue still wins" means blue wins by more than b: used to find the exact margin of a line.
inline const int RED_BONUS = envInt("SOLVER_RED_BONUS", 0);

// The tight bounds (guaranteed growth and the exact one-player optimum, Solver::tightenBounds) cost
// more than they save higher up: they are used at every node once at most TIGHT_BOUNDS_MAX_PIECES
// pieces are left in total, and with up to ALL_NODE_TIGHT_BOUNDS_MAX_PIECES only once a node's first
// move failed, where every remaining move would have to be searched.
constexpr int TIGHT_BOUNDS_MAX_PIECES = 5, ALL_NODE_TIGHT_BOUNDS_MAX_PIECES = 6;

struct Solver {
    std::shared_ptr<LTT> tt;
    int16_t killer[32][2];
    uint64_t nodes = 0;
    std::atomic<bool>* stop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool useDeadline = false;
    bool aborted = false;
    uint64_t nodeLimit = UINT64_MAX;
    // profiling counters per ply: visited, cut by TT, cut by bounds, expanded
    uint64_t pVisit[16] = {}, pTT[16] = {}, pBound[16] = {}, pExp[16] = {};
    // ... nodes solved by lastPieces and cut by the tight bounds; moves skipped by threat zones and
    // moves listed; threat scans and the scans that found a threat
    uint64_t pLast[16] = {}, pTight[16] = {}, pPruned[16] = {}, pMoves[16] = {}, pScans[16] = {}, pScanHits[16] = {};
    int evalOrderRem = envInt("SOLVER_EVAL_REM", 7);  // order children by the learned evaluation when >= this many pieces are left
    // deeper down, eval-order only where the side to move does not look lost: the losing side
    // has to try every move anyway, so ordering its moves is wasted work
    int evalOrderRemWin = envInt("SOLVER_EVAL_WIN_REM", 7);
    double evalWinThreshold = envInt("SOLVER_EVAL_WIN_THR", -100) / 100.0;
    int etcRem = envInt("SOLVER_ETC_REM", 99);        // enhanced transposition cutoffs when >= this many pieces are left
    OnePlayer onePlayer;

    // log2size: log2 of the number of entries (16 bytes each)
    explicit Solver(int log2size = 22) : tt(std::make_shared<LTT>(log2size - 1)) { init(); }
    explicit Solver(std::shared_ptr<LTT> shared) : tt(std::move(shared)) { init(); }
    std::vector<Move> mvBuf[32], legalBuf[32];
    std::vector<int> scBuf[32];
    std::vector<uint64_t> sortBuf[32];
    static constexpr int FULL_SORT_AFTER = 3;
    void init() {
        for (int i = 0; i < 32; i++) { mvBuf[i].resize(3000); legalBuf[i].resize(3000); scBuf[i].resize(3000); sortBuf[i].resize(3000); }
        std::memset(killer, -1, sizeof killer);
    }
    // Forgets what belongs to the last position solved and keeps the table: before each position of a
    // run. Killer moves carried over from another position are tried first and cost nodes;
    // the one-player memo's answers depend on the order of its queries, so a fresh one keeps the node
    // counts of a position independent of the positions before it (on its own table).
    void newPosition() {
        onePlayer.clear();
        std::memset(killer, -1, sizeof killer);
    }
    void clear() {
        tt->clear();
        newPosition();
    }

    static int terminalValue(const Pos& s) {
        int d = finalScore(s) + RED_BONUS;
        int v = d > 0 ? 1 : d < 0 ? -1 : 0;
        return s.stm == 0 ? v : -v;
    }

    // Threats. An opponent placement b is a threat when, if the opponent could play b right
    // now as a free move, the snake bounds would already decide the game. Any move of ours
    // that leaves b playable (does not cover b, and does not touch b when b starts a new
    // snake) lets the opponent answer with b, and the bounds only tighten as cells fill up
    // (our upper bound counts every cell we could still use, so it covers our move too).
    // Such moves are therefore worth at most the bound value, and only moves through the
    // zone of every threat strong enough for the current window need to be searched.
    struct Threat {
        BB zone;
        int value;  // upper bound on our value for every move that leaves the zone free
    };
    static constexpr int MAX_THREATS = 6, MAX_TRIED = 8, MAX_FOUND = 64;

    // Our value bound after the opponent plays b, or 2 if b is not a legal opponent placement.
    static int threatValue(const Pos& s, const Placement& b, int piece) {
        int me = s.stm, opp = me ^ 1;
        if (!(s.avail[opp] >> piece & 1) || !s.legalFor(opp, b)) return 2;
        Pos after = s;
        after.own[opp] |= b.m;
        int lbMe, ubMe;
        snakeBounds(after, me, lbMe, ubMe);
        ubMe += me == 0 ? RED_BONUS : -RED_BONUS;
        int lbOpp = lengthAfter(s, opp, b);
        return ubMe < lbOpp ? -1 : ubMe <= lbOpp ? 0 : 1;
    }
    static BB threatZone(const Pos& s, const Placement& b) {
        bool grows = b.nb & s.own[s.stm ^ 1];
        return grows ? b.m : b.m | b.nb;
    }
    // Adds the opponent placement gid as a threat if it is one (value <= 0). With `exact`, our
    // bound after it is the one-player optimum, which is sound in the same way: whatever we play
    // that leaves b playable, every continuation is one that the relaxation after b allows.
    void addThreat(const Pos& s, int gid, Threat* threats, int& n, int* tried, int& nTried, bool exact) {
        if (gid < 0 || n >= MAX_THREATS || nTried >= MAX_TRIED) return;
        for (int i = 0; i < nTried; i++) if (tried[i] == gid) return;
        tried[nTried++] = gid;
        const Placement& b = T.placements[gid];
        int v = threatValue(s, b, b.piece);
        if (exact && (v == 0 || v == 1)) {
            int me = s.stm, opp = me ^ 1;
            int bonus = me == 0 ? RED_BONUS : -RED_BONUS;
            Pos after = s;
            after.own[opp] |= b.m;
            Clusters cl;
            int lb, ub;
            snakeClusters(after, me, lb, ub, cl);
            int lenOpp = lengthAfter(s, opp, b);
            if (v == 1 && !mayReach(after, me, cl, lenOpp - bonus + 1)) v = 0;
            if (v == 0 && !mayReach(after, me, cl, lenOpp - bonus)) v = -1;
        }
        if (v <= 0) threats[n++] = Threat{threatZone(s, b), v};
    }
    // Every opponent placement that is a threat for the current window, cheapest test first:
    // a placement only lowers our bound inside the clusters it covers, so every cluster whose
    // bound exceeds the opponent's length after the placement must be covered by it.
    // Keeps the smallest zones (a zone containing a kept one prunes nothing more).
    // cl: our clusters (snakeClusters of the side to move)
    static void scanThreats(const Pos& s, const Clusters& cl, int alpha, int bonus, Threat* threats, int& n) {
        int me = s.stm, opp = me ^ 1;
        int lbMe = s.L[me];
        int strict = alpha < 0 ? 1 : 0;  // a loss needs our bound strictly below the opponent
        // a placement joins at most two snakes (it is one segment of a path), which caps what
        // the opponent can reach, and every cluster above that cap must be covered
        int top[2] = {0, 0};
        for (int j = 0; j < s.ncomp[opp]; j++) {
            int len = popc(s.comp[opp][j]);
            if (len > top[0]) { top[1] = top[0]; top[0] = len; }
            else if (len > top[1]) top[1] = len;
        }
        int maxNeed = std::max<int>(s.L[opp], 4 + top[0] + top[1]) - bonus - strict;
        if (lbMe > maxNeed) return;
        BB mustCover = ~(BB)0;
        for (int i = 0; i < cl.n; i++)
            if (cl.ub[i] > maxNeed) { mustCover = cl.cells[i] & ~s.own[me]; break; }
        Threat found[MAX_FOUND];
        int nf = 0;
        Pos asOpp = s;
        asOpp.stm = (uint8_t)opp;
        asOpp.forEachLegalTouching(mustCover, [&](Move b) {
            if (nf >= MAX_FOUND) return;
            const Placement& pl = b.pl();
            int lenOpp = lengthAfter(s, opp, pl);
            int need = lenOpp - bonus - strict;  // our raw upper bound must not exceed this
            if (lbMe > need) return;
            for (int i = 0; i < cl.n; i++)
                if (cl.ub[i] > need && !(cl.cells[i] & pl.m)) return;
            int ub = boundAfterBlock(s, me, cl, lbMe, pl.m) + bonus;
            int v = ub < lenOpp ? -1 : ub <= lenOpp ? 0 : 1;
            if (v <= alpha) found[nf++] = Threat{threatZone(s, pl), v};
        });
        std::sort(found, found + nf, [](const Threat& a, const Threat& b) { return popc(a.zone) < popc(b.zone); });
        for (int f = 0; f < nf && n < MAX_THREATS; f++) {
            bool redundant = false;
            for (int i = 0; i < n; i++)
                if ((threats[i].zone & ~found[f].zone) == 0 && threats[i].value <= found[f].value) redundant = true;
            if (!redundant) threats[n++] = found[f];
        }
    }
    // Upper bound on our value for a move covering cells m (2 = no bound from the threats).
    static int threatBound(BB m, const Threat* threats, int n) {
        int v = 2;
        for (int i = 0; i < n; i++)
            if (!(m & threats[i].zone)) v = std::min(v, threats[i].value);
        return v;
    }

    // Exact value when each player has at most one piece left (-2 otherwise), without a search:
    // the side to move places its piece, then the opponent makes its longest snake with the
    // placements that are still legal. A placement only ever removes opponent placements
    // (covered cells, or new snakes that would now touch it), never adds any.
    static int lastPieces(const Pos& s) {
        int me = s.stm, opp = me ^ 1;
        int nMe = pieceCount(s.avail[me]), nOpp = pieceCount(s.avail[opp]);
        if (nMe > 1 || nOpp > 1 || s.ply == 0) return -2;
        int bonus = me == 0 ? RED_BONUS : -RED_BONUS;
        auto result = [&](int lenMe, int lenOpp) { int d = lenMe + bonus - lenOpp; return d > 0 ? 1 : d < 0 ? -1 : 0; };
        // the opponent's placements that would lengthen its longest snake, longest first
        struct Reply { BB block; int len; };  // block: our cells that make the reply illegal
        constexpr int MAX_REPLIES = 512;  // reachable positions have at most 468 (41+ cells filled)
        Reply replies[MAX_REPLIES];
        int nr = 0;
        bool overflow = false;
        if (nOpp) {
            Pos asOpp = s;
            asOpp.stm = (uint8_t)opp;
            BB occ = s.occ();
            asOpp.forEachLegal([&](Move q) {
                const Placement& pl = q.pl();
                int len = lengthAfter(s, opp, pl);
                if (len <= s.L[opp]) return;
                if (nr == MAX_REPLIES) { overflow = true; return; }
                replies[nr++] = Reply{pl.nb & occ ? pl.m : pl.m | pl.nb, len};
            });
            if (overflow) return -2;  // let the search handle it
            std::sort(replies, replies + nr, [](const Reply& a, const Reply& b) { return a.len > b.len; });
        }
        auto oppBest = [&](BB ours) {
            for (int i = 0; i < nr; i++) if (!(replies[i].block & ours)) return replies[i].len;
            return (int)s.L[opp];
        };
        int best = -2;
        if (nMe) {
            s.forEachLegal([&](Move b) {
                if (best == 1) return;
                const Placement& pl = b.pl();
                best = std::max(best, result(lengthAfter(s, me, pl), oppBest(pl.m)));
            });
        }
        if (best == -2) best = result(s.L[me], oppBest(0));  // no piece or no legal placement: pass
        return best;
    }

    // Guaranteed growth: lower bounds on both final snakes, one and two plies ahead.
    //  - The side to move reaches at least the longest snake of its best move now (lenMe).
    //  - The opponent reaches at least the length that none of our moves prevents (lenOpp): some
    //    placement of its that reaches it stays legal after any single move of ours (a grow
    //    placement unless we cover it, a new snake unless we cover or touch it). Our move only
    //    removes opponent placements, never adds any; if we have to pass, every one stays.
    // legal: every legal move of the side to move.
    static void growthBounds(const Pos& s, const Move* legal, int nLegal, int& lenMe, int& lenOpp) {
        int me = s.stm, opp = me ^ 1;
        lenMe = s.L[me];
        for (int i = 0; i < nLegal; i++) lenMe = std::max(lenMe, lengthAfter(s, me, legal[i].pl()));
        lenOpp = s.L[opp];
        if (!s.avail[opp]) return;
        struct Extension { BB block; int len; };  // block: our cells that make it illegal
        constexpr int MAX_EXTENSIONS = 512;
        Extension ext[MAX_EXTENSIONS];
        int ne = 0;
        Pos asOpp = s;
        asOpp.stm = (uint8_t)opp;
        BB occ = s.occ();
        auto add = [&](Move q) {
            const Placement& pl = q.pl();
            int len = lengthAfter(s, opp, pl);
            if (len > s.L[opp] && ne < MAX_EXTENSIONS) ext[ne++] = Extension{pl.nb & occ ? pl.m : pl.m | pl.nb, len};
        };
        // a new snake is longer only while the opponent's snakes are shorter than a piece
        if (s.L[opp] >= PIECE_CELLS[P4C]) asOpp.forEachLegalGrow(add);
        else asOpp.forEachLegal(add);
        if (!ne) return;
        std::sort(ext, ext + ne, [](const Extension& a, const Extension& b) { return a.len > b.len; });
        lenOpp = ext[0].len;  // if we have to pass, every placement stays
        for (int i = 0; i < nLegal && lenOpp > s.L[opp]; i++) {
            BB m = legal[i].pl().m;
            int best = s.L[opp];
            for (int j = 0; j < ne && ext[j].len > best; j++)
                if (!(ext[j].block & m)) { best = ext[j].len; break; }
            lenOpp = std::min(lenOpp, best);
        }
    }

    // Narrows [lo, hi] with the growth bounds and the exact one-player optimum, asking only the
    // questions whose answer would end the node in the window (alpha, beta). lbMe/ubMe include
    // our share of RED_BONUS (bonus); legal: every legal move of ours; myCl/oppCl: the clusters.
    void tightenBounds(const Pos& s, const Move* legal, int nLegal, int& lo, int& hi, int alpha, int beta,
                       int lbMe, int ubMe, int lbOp, int ubOp, int bonus, const Clusters& myCl, const Clusters& oppCl) {
        int me = s.stm, opp = me ^ 1;
        int lenMe, lenOpp;
        growthBounds(s, legal, nLegal, lenMe, lenOpp);
        lbMe = std::max(lbMe, lenMe + bonus);
        lbOp = std::max(lbOp, lenOpp);
        lo = std::max(lo, lbMe > ubOp ? 1 : lbMe >= ubOp ? 0 : -1);
        hi = std::min(hi, ubMe < lbOp ? -1 : ubMe <= lbOp ? 0 : 1);
        auto decided = [&] { return lo >= hi || lo >= beta || hi <= alpha; };
        if (decided()) return;
        // our optimum against the opponent's guaranteed length lowers hi: to 0 if we cannot pass
        // it (a cutoff only when alpha >= 0 or lo >= 0), to -1 if we cannot reach it
        if (hi == 1 && (alpha >= 0 || lo >= 0) && !mayReach(s, me, myCl, lbOp - bonus + 1)) hi = 0;
        if (decided()) return;
        if ((hi == 0 || alpha == -1) && !mayReach(s, me, myCl, lbOp - bonus)) hi = -1;
        if (decided()) return;
        // the opponent's optimum against our guaranteed length raises lo the same way
        if (lo == -1 && (beta <= 0 || hi <= 0) && !mayReach(s, opp, oppCl, lbMe + 1)) lo = 0;
        if (decided()) return;
        if ((lo == 0 || beta == 1) && !mayReach(s, opp, oppCl, lbMe)) lo = 1;
    }
    // tightenBounds for the moves `rest` of the side to move only: [lo, hi] bounds the best of them
    // (the growth bounds hold move by move). myCl: our clusters; the opponent's are computed here.
    void restBounds(const Pos& s, const Move* rest, int nRest, int& lo, int& hi, int alpha, int beta, int lbMe, int ubMe,
                    int lbOp, int ubOp, int bonus, const Clusters& myCl, Clusters& oppCl) {
        int l, u;
        snakeClusters(s, s.stm ^ 1, l, u, oppCl);
        tightenBounds(s, rest, nRest, lo, hi, alpha, beta, lbMe, ubMe, lbOp, ubOp, bonus, myCl, oppCl);
    }
    bool mayReach(const Pos& s, int c, const Clusters& cl, int target) { return onePlayer.mayReach(s, c, cl, target); }

    bool checkAbort() {
        if (aborted) return true;
        if ((nodes & 4095) == 0) {
            if (stop && stop->load(std::memory_order_relaxed)) aborted = true;
            else if (useDeadline && std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        if (nodes > nodeLimit) aborted = true;
        return aborted;
    }

    int search(const Pos& s, int alpha, int beta) {
        nodes++;
        if (checkAbort()) return 0;
        if (s.over()) return terminalValue(s);
        uint64_t n0 = nodes;
        int pp = std::min<int>(s.ply, 15);
        pVisit[pp]++;

        int lo = -1, hi = 1;
        int t;
        uint64_t k = s.key(t);
        uint32_t check = s.keyCheck();
        int ttMove = -1, eLo, eHi, eBest = -1;
        if (tt->probe(k, check, eLo, eHi, eBest)) {
            lo = eLo; hi = eHi;
            if (lo >= hi || lo >= beta) { pTT[pp]++; return lo; }
            if (hi <= alpha) { pTT[pp]++; return hi; }
            if (eBest >= 0) ttMove = T.gidSym[eBest][T.inv[t]];
        }
        int last = lastPieces(s);
        if (last != -2) {
            pLast[pp]++;
            tt->store(k, check, last, last, eBest, 0);
            return last;
        }
        // snake-length bound cutoffs
        int lbMe, ubMe, lbOp, ubOp;
        int rem = pieceCount(s.avail[0]) + pieceCount(s.avail[1]);
        bool tight = rem <= TIGHT_BOUNDS_MAX_PIECES;
        Clusters myClusters, oppClusters;  // kept for the threat scan and the tight bounds
        snakeClusters(s, s.stm, lbMe, ubMe, myClusters);
        if (tight) snakeClusters(s, s.stm ^ 1, lbOp, ubOp, oppClusters);
        else snakeBounds(s, s.stm ^ 1, lbOp, ubOp);
        int bonus = s.stm == 0 ? RED_BONUS : -RED_BONUS;  // the side to move's share of SOLVER_RED_BONUS
        lbMe += bonus;
        ubMe += bonus;
        int blo = lbMe > ubOp ? 1 : lbMe >= ubOp ? 0 : -1;
        int bhi = ubMe < lbOp ? -1 : ubMe <= lbOp ? 0 : 1;
        lo = std::max(lo, blo);
        hi = std::min(hi, bhi);
        if (lo >= hi || lo >= beta) { pBound[pp]++; return lo; }
        if (hi <= alpha) { pBound[pp]++; return hi; }

        // per-depth move buffers (no per-node initialisation of 3000-element arrays)
        Move* moves = mvBuf[s.ply & 31].data();
        int* score = scBuf[s.ply & 31].data();
        int n = 0;
        // the tight bounds need every legal move of ours (the growth bounds), which the search then reuses
        Move* legal = legalBuf[s.ply & 31].data();
        int nLegal = 0;
        if (tight) {
            s.forEachLegal([&](Move m) { legal[nLegal++] = m; });
            tightenBounds(s, legal, nLegal, lo, hi, alpha, beta, lbMe, ubMe, lbOp, ubOp, bonus, myClusters, oppClusters);
            if (lo >= hi || lo >= beta || hi <= alpha) {
                pTight[pp]++;
                tt->store(k, check, lo, hi, eBest, 0);
                return lo >= hi || lo >= beta ? lo : hi;
            }
        }
        pExp[pp]++;
        alpha = std::max(alpha, lo);
        beta = std::min(beta, hi);
        int a0 = alpha, b0 = beta;

        // generate + order
        BB me = s.own[s.stm];
        BB opp = s.own[s.stm ^ 1];
        BB oppFront = expand(shapeOf(opp).ends) & s.empty();
        int ply = std::min<int>(s.ply, 31);
        int k1 = killer[ply][0], k2 = killer[ply][1];
        // threats: the opponent's refutations one ply down are the natural candidates
        Threat threats[MAX_THREATS];
        int nThreats = 0, tried[MAX_TRIED], nTried = 0;
        int skipped = -2;  // best bound of the moves skipped because of threats
        bool threatsOn = s.avail[s.stm ^ 1] && ply < 31;
        if (threatsOn) {
            addThreat(s, killer[ply + 1][0], threats, nThreats, tried, nTried, tight);
            addThreat(s, killer[ply + 1][1], threats, nThreats, tried, nTried, tight);
        }
        bool scanned = !threatsOn;
        bool allNodeChecked = tight || rem > ALL_NODE_TIGHT_BOUNDS_MAX_PIECES;
        auto addMove = [&](Move m) {
            const Placement& pl = m.pl();
            if (nThreats) {
                int tb = threatBound(pl.m, threats, nThreats);
                if (tb <= alpha) { skipped = std::max(skipped, tb); pPruned[pp]++; return; }
            }
            int sc = PIECE_CELLS[m.piece()] * 64;
            if (pl.nb & me) sc += 256;
            if (pl.m & oppFront) sc += 128;
            else if (pl.nb & opp) sc += 32;
            if (m.gid == ttMove) sc = 1 << 30;
            else if (m.gid == k1) sc = 1 << 29;
            else if (m.gid == k2) sc = 1 << 28;
            moves[n] = m; score[n] = sc; n++;
        };
        if (tight) for (int i = 0; i < nLegal; i++) addMove(legal[i]);
        else s.forEachLegal(addMove);
        bool doEval = n > 1 && (rem >= evalOrderRem || (rem >= evalOrderRemWin && learnedLogit(s) > evalWinThreshold));
        bool doEtc = n > 0 && rem >= etcRem;
        // moves scored at least `ahead` (the table move and the killers) stay ahead of the evaluation
        // ordering. The evaluation is expensive: when such a move exists it is tried first, and the
        // other moves are evaluated only if it does not cut off
        constexpr int ahead = 1 << 28;
        bool evalLater = false;
        if (doEval && !doEtc)
            for (int i = 0; i < n && !evalLater; i++) evalLater = score[i] >= ahead;
        if (evalLater) doEval = false;
        auto evaluateRest = [&](int from) {
            for (int i = from; i < n; i++)
                if (score[i] < ahead) {
                    Pos c = s; c.play(moves[i]);
                    score[i] = c.over() ? -terminalValue(c) * 100000 : -(int)(1000 * learnedLogit(c));
                }
        };
        if (doEval || doEtc) {
            for (int i = 0; i < n; i++) {
                Pos c = s; c.play(moves[i]);
                if (doEtc) {
                    // enhanced transposition cutoff: a child already known to be good enough
                    int cv = -9;
                    if (c.over()) cv = -terminalValue(c);
                    else { int ct, clo, chi, cb; if (tt->probe(c.key(ct), c.keyCheck(), clo, chi, cb)) cv = -chi; }  // our value >= -chi
                    if (cv >= beta) {
                        tt->store(k, check, std::max(lo, cv), hi, T.gidSym[moves[i].gid][t], 0);
                        return cv;
                    }
                }
            }
            if (doEval) evaluateRest(0);
        }
        pMoves[pp] += n;
        int best = -2, bestGid = -1;
        if (n == 0 && skipped == -2) {
            Pos c = s;
            c.play(Move{});
            best = -search(c, -beta, -alpha);
        } else {
            for (int i = 0; i < n; i++) {
                if (evalLater && i == 1) { evaluateRest(1); evalLater = false; }
                if (i < FULL_SORT_AFTER) {  // pick the best remaining move: most nodes cut on the first one
                    int bi = i;
                    for (int j = i + 1; j < n; j++) if (score[j] > score[bi]) bi = j;
                    std::swap(moves[i], moves[bi]); std::swap(score[i], score[bi]);
                } else if (i == FULL_SORT_AFTER) {  // every move may be needed: sort the rest once
                    uint64_t* keys = sortBuf[s.ply & 31].data();
                    for (int j = i; j < n; j++) keys[j] = uint64_t(uint32_t(score[j]) ^ 0x80000000u) << 16 | uint16_t(moves[j].gid);
                    std::sort(keys + i, keys + n, std::greater<uint64_t>());
                    for (int j = i; j < n; j++) { moves[j].gid = int16_t(keys[j] & 0xFFFF); score[j] = int(uint32_t(keys[j] >> 16) ^ 0x80000000u); }
                }
                if (nThreats) {
                    int tb = threatBound(moves[i].pl().m, threats, nThreats);
                    if (tb <= alpha) { skipped = std::max(skipped, tb); pPruned[pp]++; continue; }
                }
                // once the rest is sorted the next move is known: its table entry is fetched from memory
                // while this move is searched
                if (i >= FULL_SORT_AFTER && i + 1 < n && s.ply >= SYM_PLY) tt->prefetch(s.keyAfter(moves[i + 1]));
                Pos c = s;
                c.play(moves[i]);
                int v = -search(c, -beta, -alpha);
                if (aborted) return 0;
                if (v > best) { best = v; bestGid = moves[i].gid; }
                if (v > alpha) alpha = v;
                if (alpha >= beta) {
                    if (killer[ply][0] != moves[i].gid) { killer[ply][1] = killer[ply][0]; killer[ply][0] = moves[i].gid; }
                    break;
                }
                // the first move failed: above the gate, the tight bounds over the moves left may
                // settle them all at once
                if (!allNodeChecked && i + 2 < n) {
                    allNodeChecked = true;
                    int restLo = -1, restHi = 1;
                    restBounds(s, moves + i + 1, n - i - 1, restLo, restHi, alpha, beta, lbMe, ubMe, lbOp, ubOp, bonus, myClusters, oppClusters);
                    if (restHi <= alpha) { pTight[pp]++; best = std::max(best, restHi); break; }
                    if (restLo >= beta) { pTight[pp]++; best = restLo; break; }
                }
                // the refutation just found may be a threat against the remaining moves; once the
                // first move failed, this is probably a node where every move must be refuted, so
                // look for all the opponent's threats
                if (threatsOn && i + 2 < n) {
                    addThreat(s, killer[ply + 1][0], threats, nThreats, tried, nTried, tight);
                    if (!scanned) {
                        scanned = true;
                        int before = nThreats;
                        scanThreats(s, myClusters, alpha, bonus, threats, nThreats);
                        pScans[pp]++;
                        if (nThreats > before) pScanHits[pp]++;
                    }
                }
            }
            best = std::max(best, skipped);
        }
        if (aborted) return 0;
        // store
        int nlo = lo, nhi = hi;
        if (best <= a0) nhi = std::min(nhi, best);
        else if (best >= b0) nlo = std::max(nlo, best);
        else nlo = nhi = best;
        uint64_t work = nodes - n0 + 1;
        int w = 63 - __builtin_clzll(work);
        tt->store(k, check, nlo, nhi, bestGid >= 0 ? T.gidSym[bestGid][t] : eBest, w);
        return best;
    }

    // Returns value for the side to move, or -9 if aborted. A narrower window answers a
    // yes/no question more cheaply: (0, 1) asks "does the side to move win?" (result 1 or
    // <= 0), (-1, 0) asks "does it avoid losing?" (result >= 0 or -1).
    // (parallel version: see parallelSolve below)
    int solve(const Pos& s, double seconds = 0, uint64_t maxNodes = UINT64_MAX, int alpha = -1, int beta = 1) {
        aborted = false;
        nodes = 0;
        nodeLimit = maxNodes;
        useDeadline = seconds > 0;
        if (useDeadline)
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::microseconds((long long)(seconds * 1e6));
        int v = search(s, alpha, beta);
        return aborted ? -9 : v;
    }
};

enum SolveQuestion { EXACT, LOSS_TEST, WIN_TEST };
// Parallel exact solve: root moves (symmetry-reduced, ordered by the learned
// evaluation) are distributed over threads, each with its own solver (killers reset
// for every root move) and TT, or one shared TT. Stops as soon as a winning move is
// found. perMove receives the value of every root move for the side to move (-9 = unknown).
// question: EXACT values, or only whether the side to move loses (LOSS_TEST: every child is
// a win test for the opponent; a root move is -1, or 0 for "does not lose"; stops at the first
// move that does not lose) or wins (WIN_TEST: a root move is 1, or 0 for "does not win"; stops
// at the first winning move). The yes/no questions are much cheaper than exact values.
#include <functional>
#include <mutex>
#include <thread>
struct ParallelSolveResult {
    int value = -9;  // for the side to move, -9 unknown
    uint64_t nodes = 0;
    double seconds = 0;
    std::vector<std::pair<Move, int>> perMove;
};
// known: root moves whose value is already known (e.g. from an interrupted run) - skipped.
// onResult: called (under a lock) for every newly solved root move.
// table: one table for all threads (a sweep's, kept across calls); without it SOLVER_SHARED=<log2 entries>
// gives the threads one table, otherwise each thread has its own of 2^ttLog2 entries.
inline ParallelSolveResult parallelSolve(const Pos& root, double seconds, int threads, int ttLog2 = 22,
                                         const std::vector<std::pair<Move, int>>& known = {},
                                         std::function<void(Move, int, uint64_t, double)> onResult = nullptr,
                                         SolveQuestion question = EXACT, std::shared_ptr<LTT> table = nullptr) {
    auto t0 = std::chrono::steady_clock::now();
    ParallelSolveResult res;
    std::vector<Move> all, mv;
    root.genMoves(all);
    if (all.empty()) all.push_back(Move{});
    std::vector<uint64_t> seen;
    for (Move m : all) {
        Pos c = root; c.play(m);
        int t; uint64_t k = c.key(t);
        if (std::find(seen.begin(), seen.end(), k) == seen.end()) { seen.push_back(k); mv.push_back(m); }
    }
    std::vector<double> ord(mv.size());
    for (size_t i = 0; i < mv.size(); i++) {
        Pos c = root; c.play(mv[i]);
        ord[i] = c.over() ? -Solver::terminalValue(c) * 1000.0 : -learnedLogit(c);
    }
    std::vector<size_t> idx(mv.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return ord[a] > ord[b]; });
    std::vector<int> val(mv.size(), -9);
    std::atomic<bool> stop{false};
    int enough = question == LOSS_TEST ? 0 : 1;  // a root move this good settles the root
    for (auto& [m, v] : known)
        for (size_t i = 0; i < mv.size(); i++)
            if (mv[i] == m) { val[i] = v; if (v >= enough) stop = true; }
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> nodes{0};
    std::mutex mu;
    threads = std::max(1, std::min<int>(threads, (int)mv.size()));
    std::vector<std::thread> th;
    int sharedLog2 = envInt("SOLVER_SHARED", 0);
    std::shared_ptr<LTT> shared = table ? table : sharedLog2 ? std::make_shared<LTT>(sharedLog2 - 1) : nullptr;
    auto work = [&] {
        std::unique_ptr<Solver> SP(shared ? new Solver(shared) : new Solver(ttLog2));
        Solver& S = *SP;
        S.stop = &stop;
        for (;;) {
            size_t k = next++;
            if (k >= idx.size() || stop) break;
            size_t i = idx[k];
            if (val[i] != -9) continue;  // known from a previous run
            Pos c = root; c.play(mv[i]);
            S.newPosition();  // killers from a sibling root move cost nodes
            auto ts = std::chrono::steady_clock::now();
            double left = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) break;
            // child windows: LOSS_TEST asks whether the opponent wins, WIN_TEST whether it avoids losing
            int v = question == LOSS_TEST ? S.solve(c, left, UINT64_MAX, 0, 1)
                    : question == WIN_TEST ? S.solve(c, left, UINT64_MAX, -1, 0)
                                           : S.solve(c, left);
            nodes += S.nodes;
            if (v == -9) continue;
            if (question == LOSS_TEST) v = v >= 1 ? 1 : 0;
            if (question == WIN_TEST) v = v <= -1 ? -1 : 0;
            std::lock_guard<std::mutex> lk(mu);
            val[i] = -v;
            if (-v >= enough) stop = true;
            if (onResult) onResult(mv[i], -v, S.nodes, std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
        }
    };
    if (threads == 1) work();  // inline, so it also runs where there are no threads (WebAssembly)
    else {
        for (int w = 0; w < threads; w++) th.emplace_back(work);
        for (auto& t : th) t.join();
    }
    int best = -2;
    bool allKnown = true;
    for (size_t i = 0; i < mv.size(); i++) {
        res.perMove.push_back({mv[i], val[i]});
        if (val[i] == -9) allKnown = false; else best = std::max(best, val[i]);
    }
    if (best >= enough || (allKnown && best > -2)) res.value = best;
    res.nodes = nodes;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}
