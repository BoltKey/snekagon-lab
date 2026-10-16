// Snekagon core: board geometry, pieces, positions, move generation, bounds.
//
// Board: hexagon of radius 5 (91 cells) in axial coords (q, r), stored in a
// padded 11x11 grid -> cell index = (r+5)*11 + (q+5), 0..120. Fits in 128 bits,
// and neighbour expansion is a handful of shifts.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

typedef unsigned __int128 BB;

constexpr int W = 11, R = 5, NIDX = 121, NCELLS = 91;
constexpr int NPIECE = 8;
constexpr int NSYM = 12;
constexpr int SYM_PLY = 6;  // symmetry-canonical hashing up to this ply

inline BB bit(int i) { return (BB)1 << i; }
inline int popc(BB b) {
    return __builtin_popcountll((uint64_t)b) + __builtin_popcountll((uint64_t)(b >> 64));
}
inline int lsb(BB b) {
    uint64_t lo = (uint64_t)b;
    return lo ? __builtin_ctzll(lo) : 64 + __builtin_ctzll((uint64_t)(b >> 64));
}
inline int qOf(int i) { return i % W - R; }
inline int rOf(int i) { return i / W - R; }
inline int idxOf(int q, int r) { return (r + R) * W + (q + R); }
inline bool onBoard(int q, int r) {
    return q >= -R && q <= R && r >= -R && r <= R && std::abs(q + r) <= R;
}
// neighbour directions in ring order (consecutive entries are adjacent to each other)
constexpr int DQ[6] = {1, 1, 0, -1, -1, 0};
constexpr int DR[6] = {0, -1, -1, 0, 1, 1};

// ---- pieces ---------------------------------------------------------------
// Shapes read off the print & play sheet. Pieces are double-sided (may flip).
enum { P1 = 0, P2, P3I, P3V, P4I, P4A, P4B, P4C };
inline const char* PNAME[NPIECE] = {"1", "2", "3I", "3V", "4I", "4A", "4B", "4C"};
constexpr int PIECE_CELLS[NPIECE] = {1, 2, 3, 3, 4, 4, 4, 4};
inline const std::vector<std::pair<int, int>> PSHAPE[NPIECE] = {
    {{0, 0}},
    {{0, 0}, {1, 0}},
    {{0, 0}, {0, 1}, {0, 2}},
    {{0, 0}, {1, 0}, {1, 1}},
    {{0, 0}, {-1, 1}, {-2, 2}, {-3, 3}},
    {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
    {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
    {{0, 0}, {-1, 1}, {-1, 2}, {0, 2}},
};
// Player 1 starts with the 1-hex (forced first move); player 2 never has it.
constexpr uint8_t ALL_PIECES = 0xFF;
constexpr uint8_t P2_PIECES = 0xFE;

struct Placement {
    BB m;      // cells covered
    BB nb;     // cells adjacent to the piece (excluding the piece)
    uint8_t c[4];
    uint8_t n;
    uint8_t piece;
    int gid;   // global placement id (for history tables / symmetry maps)
};

// One orientation of a piece, used by the bit-parallel move generator: legality
// of every anchor cell at once is an AND/OR of shifted bitboards.
struct Orient {
    int piece, n;
    int dq[4], dr[4];
    int ideg[4];                              // neighbours inside the piece
    std::vector<std::pair<int, int>> tri;     // outside cells touching 2 adjacent (or >=3) piece cells
    std::vector<std::pair<int, int>> two;     // outside cells touching exactly 2 non-adjacent piece cells
    int16_t gidAt[NIDX];                      // anchor cell -> placement gid (-1 if off board)
    BB nextTo[NIDX];                          // cell -> anchors whose placement covers a neighbour of it, not it
};

struct Tables {
    BB VALID = 0, NOT_QMAX = 0, NOT_QMIN = 0;
    BB COLR[17];  // COLR[dq+8]: cells whose q lies in [-5+dq, 5+dq]
    BB NB[NIDX];
    int cells[NCELLS];
    int perm[NSYM][NIDX];  // cell -> transformed cell
    int inv[NSYM];
    std::vector<Placement> placements;      // every placement, by gid (piece by piece)
    std::span<const Placement> PL[NPIECE];  // the placements of each piece
    std::vector<Orient> OR;
    int orBegin[NPIECE + 1];
    std::vector<std::pair<int, int>> gidInfo;  // gid -> (piece, index)
    std::vector<std::array<int, NSYM>> gidSym; // gid -> transformed gid
    int NGID = 0;
    uint64_t zCell[2][NIDX];
    uint64_t zPiece[2][NPIECE];
    uint64_t zStm;
    uint8_t fit[256][NIDX + 1];  // max subset-sum of piece sizes in mask <= n
};
inline Tables T;

inline BB expand(BB b) {
    BB a = b & T.NOT_QMAX, c = b & T.NOT_QMIN;
    return ((a << 1) | (c >> 1) | (b << 11) | (b >> 11) | (a >> 10) | (c << 10)) & T.VALID;
}
// set of cells x such that x + (dq, dr) is in b
inline BB shiftFrom(BB b, int dq, int dr) {
    BB m = b & T.COLR[dq + 8];
    int d = dr * W + dq;
    return (d >= 0 ? (m >> d) : (m << -d)) & T.VALID;
}
inline BB flood(BB seed, BB allowed) {
    BB c = seed, prev = 0;
    while (c != prev) { prev = c; c |= expand(c) & allowed; }
    return c;
}

inline std::pair<int, int> symApply(int t, int q, int r) {
    // t = rotations (0..5) + 6*reflect. rot60: (q,r)->(-r, q+r); reflect: (q,r)->(r,q)
    if (t >= 6) std::swap(q, r);
    for (int k = 0; k < t % 6; k++) {
        int nq = -r, nr = q + r;
        q = nq; r = nr;
    }
    return {q, r};
}

inline uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

inline bool hexAdj(int q1, int r1, int q2, int r2) {
    for (int k = 0; k < 6; k++) if (q1 + DQ[k] == q2 && r1 + DR[k] == r2) return true;
    return false;
}

inline void initTables() {
    int k = 0;
    for (int i = 0; i < NIDX; i++) {
        int q = qOf(i), r = rOf(i);
        if (!onBoard(q, r)) continue;
        T.VALID |= bit(i);
        T.cells[k++] = i;
        if (q < R) T.NOT_QMAX |= bit(i);
        if (q > -R) T.NOT_QMIN |= bit(i);
    }
    for (int dq = -8; dq <= 8; dq++) {
        BB m = 0;
        for (int i = 0; i < NIDX; i++) if (qOf(i) >= -R + dq && qOf(i) <= R + dq) m |= bit(i);
        T.COLR[dq + 8] = m;
    }
    for (int i = 0; i < NIDX; i++) T.NB[i] = (T.VALID >> i & 1) ? expand(bit(i)) : 0;
    for (int t = 0; t < NSYM; t++)
        for (int i = 0; i < NIDX; i++) {
            if (!(T.VALID >> i & 1)) { T.perm[t][i] = i; continue; }
            auto [q, r] = symApply(t, qOf(i), rOf(i));
            T.perm[t][i] = idxOf(q, r);
        }
    for (int t = 0; t < NSYM; t++)
        for (int u = 0; u < NSYM; u++) {
            bool ok = true;
            for (int c : T.cells) if (T.perm[u][T.perm[t][c]] != c) { ok = false; break; }
            if (ok) { T.inv[t] = u; break; }
        }
    // orientations + placements (placement order = orientation-major, anchors ascending;
    // the bit-parallel generator enumerates in exactly the same order)
    T.gidInfo.clear();
    T.placements.clear();
    T.OR.clear();
    int plBegin[NPIECE + 1];
    for (int p = 0; p < NPIECE; p++) {
        T.orBegin[p] = (int)T.OR.size();
        plBegin[p] = (int)T.placements.size();
        std::vector<std::vector<std::pair<int, int>>> ors;
        for (int t = 0; t < NSYM; t++) {
            std::vector<std::pair<int, int>> o;
            for (auto [q, r] : PSHAPE[p]) o.push_back(symApply(t, q, r));
            std::sort(o.begin(), o.end(), [](auto a, auto b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
            auto b0 = o[0];
            for (auto& c : o) { c.first -= b0.first; c.second -= b0.second; }
            if (std::find(ors.begin(), ors.end(), o) == ors.end()) ors.push_back(o);
        }
        for (auto& o : ors) {
            Orient od{};
            od.piece = p;
            od.n = (int)o.size();
            for (int j = 0; j < od.n; j++) {
                od.dq[j] = o[j].first; od.dr[j] = o[j].second;
                od.ideg[j] = 0;
                for (int l = 0; l < od.n; l++)
                    if (l != j && hexAdj(o[j].first, o[j].second, o[l].first, o[l].second)) od.ideg[j]++;
            }
            // outside cells touching >= 2 piece cells
            std::vector<std::pair<int, int>> seen;
            for (int j = 0; j < od.n; j++)
                for (int d = 0; d < 6; d++) {
                    std::pair<int, int> e{o[j].first + DQ[d], o[j].second + DR[d]};
                    if (std::find(o.begin(), o.end(), e) != o.end()) continue;
                    if (std::find(seen.begin(), seen.end(), e) != seen.end()) continue;
                    seen.push_back(e);
                    std::vector<int> touching;
                    for (int l = 0; l < od.n; l++)
                        if (hexAdj(e.first, e.second, o[l].first, o[l].second)) touching.push_back(l);
                    if (touching.size() < 2) continue;
                    bool adjPair = touching.size() >= 3;
                    for (size_t x = 0; x < touching.size(); x++)
                        for (size_t y = x + 1; y < touching.size(); y++)
                            if (hexAdj(o[touching[x]].first, o[touching[x]].second, o[touching[y]].first, o[touching[y]].second)) adjPair = true;
                    (adjPair ? od.tri : od.two).push_back(e);
                }
            for (int i = 0; i < NIDX; i++) { od.gidAt[i] = -1; od.nextTo[i] = 0; }
            for (int c : T.cells) {
                Placement pl{};
                pl.piece = (uint8_t)p;
                bool ok = true;
                for (auto [dq, dr] : o) {
                    int q = qOf(c) + dq, r = rOf(c) + dr;
                    if (!onBoard(q, r)) { ok = false; break; }
                    pl.c[pl.n++] = (uint8_t)idxOf(q, r);
                    pl.m |= bit(idxOf(q, r));
                }
                if (!ok) continue;
                pl.nb = expand(pl.m) & ~pl.m;
                pl.gid = (int)T.gidInfo.size();
                od.gidAt[c] = (int16_t)pl.gid;
                for (BB nb = pl.nb; nb; nb &= nb - 1) od.nextTo[lsb(nb)] |= bit(c);
                T.gidInfo.push_back({p, (int)T.placements.size() - plBegin[p]});
                T.placements.push_back(pl);
            }
            T.OR.push_back(od);
        }
    }
    T.orBegin[NPIECE] = (int)T.OR.size();
    plBegin[NPIECE] = (int)T.placements.size();
    for (int p = 0; p < NPIECE; p++) T.PL[p] = std::span<const Placement>(T.placements).subspan(plBegin[p], plBegin[p + 1] - plBegin[p]);
    T.NGID = (int)T.gidInfo.size();
    // symmetry map on placements
    T.gidSym.assign(T.NGID, {});
    for (int p = 0; p < NPIECE; p++) {
        for (auto& pl : T.PL[p])
            for (int t = 0; t < NSYM; t++) {
                BB m = 0;
                for (int j = 0; j < pl.n; j++) m |= bit(T.perm[t][pl.c[j]]);
                int found = -1;
                for (auto& pl2 : T.PL[p]) if (pl2.m == m) { found = pl2.gid; break; }
                T.gidSym[pl.gid][t] = found;
            }
    }
    uint64_t seed = 0x5eca9011ULL;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < NIDX; i++) T.zCell[c][i] = splitmix(seed);
        for (int p = 0; p < NPIECE; p++) T.zPiece[c][p] = splitmix(seed);
    }
    T.zStm = splitmix(seed);
    for (int m = 0; m < 256; m++) {
        std::vector<bool> can(NIDX + 1, false);
        can[0] = true;
        for (int p = 0; p < NPIECE; p++)
            if (m >> p & 1)
                for (int s = NIDX; s >= PIECE_CELLS[p]; s--) if (can[s - PIECE_CELLS[p]]) can[s] = true;
        int best = 0;
        for (int n = 0; n <= NIDX; n++) { if (can[n]) best = n; T.fit[m][n] = (uint8_t)best; }
    }
}

// ---- moves & positions ------------------------------------------------------
struct Move {
    int16_t gid = -1;  // -1 = pass
    bool isPass() const { return gid < 0; }
    int piece() const { return T.placements[gid].piece; }
    const Placement& pl() const { return T.placements[gid]; }
    bool operator==(const Move& o) const { return gid == o.gid; }
};

// Per-player structure of the own cells.
struct Shape {
    BB own;    // own cells
    BB ge1, ge2, ge3;  // cells with >= 1/2/3 own neighbours
    BB tri2;   // cells with two mutually adjacent own neighbours
    BB full;   // own cells with 2 own neighbours (snake body; can never be touched again)
    BB ends;   // own cells with <= 1 own neighbour
};
inline Shape shapeOf(BB own) {
    Shape s;
    s.own = own;
    BB n[6];
    for (int d = 0; d < 6; d++) n[d] = shiftFrom(own, DQ[d], DR[d]);
    BB g1 = 0, g2 = 0, g3 = 0, t2 = 0;
    for (int d = 0; d < 6; d++) {
        g3 |= g2 & n[d]; g2 |= g1 & n[d]; g1 |= n[d];
        t2 |= n[d] & n[(d + 1) % 6];
    }
    s.ge1 = g1; s.ge2 = g2; s.ge3 = g3; s.tri2 = t2;
    s.full = own & g2;
    s.ends = own & ~g2;
    return s;
}

struct GenCtx {
    BB iso;       // empty cells not adjacent to any tile (new snakes)
    BB allow[3];  // cells a grow-piece cell with 0/1/2 internal neighbours may occupy
    BB adjOwn;    // empty cells adjacent to own tiles
    BB me, meDeg1;
};

// anchors of the legal placements of orientation o that grow an own snake
inline BB orientGrowLegal(const Orient& o, const GenCtx& g) {
    BB gr = T.VALID, touch = 0;
    for (int k = 0; k < o.n; k++) {
        gr &= shiftFrom(g.allow[o.ideg[k]], o.dq[k], o.dr[k]);
        touch |= shiftFrom(g.adjOwn, o.dq[k], o.dr[k]);
    }
    gr &= touch;
    if (gr) {
        for (auto [q, r] : o.tri) gr &= ~shiftFrom(g.me, q, r);
        for (auto [q, r] : o.two) gr &= ~shiftFrom(g.meDeg1, q, r);
    }
    return gr;
}
inline BB orientLegal(const Orient& o, const GenCtx& g) {
    BB nw = T.VALID;
    for (int k = 0; k < o.n; k++) nw &= shiftFrom(g.iso, o.dq[k], o.dr[k]);
    if (!g.adjOwn) return nw;
    return nw | orientGrowLegal(o, g);
}

struct Pos {
    BB own[2] = {0, 0};
    uint8_t avail[2] = {ALL_PIECES, P2_PIECES};
    uint8_t stm = 0;     // 0 = player 1 (red / starts), 1 = player 2
    uint8_t passes = 0;  // consecutive passes
    uint8_t ply = 0;
    uint64_t h[NSYM] = {};  // h[1..11] (symmetric variants) are maintained only while ply <= SYM_PLY
    // snakes of each player, kept up to date by play(): component bitboards + longest length
    BB comp[2][8];
    uint8_t ncomp[2] = {0, 0};
    uint8_t L[2] = {0, 0};

    void initComps() {
        for (int c = 0; c < 2; c++) {
            ncomp[c] = 0; L[c] = 0;
            BB b = own[c];
            while (b) {
                BB k = flood(bit(lsb(b)), b);
                b &= ~k;
                comp[c][ncomp[c]++] = k;
                L[c] = std::max<int>(L[c], popc(k));
            }
        }
    }
    void initHash() {
        initComps();
        for (int t = 0; t < NSYM; t++) {
            uint64_t x = 0;
            for (int c = 0; c < 2; c++) {
                BB b = own[c];
                while (b) { int i = lsb(b); b &= b - 1; x ^= T.zCell[c][T.perm[t][i]]; }
                for (int p = 0; p < NPIECE; p++) if (avail[c] >> p & 1) x ^= T.zPiece[c][p];
            }
            if (stm) x ^= T.zStm;
            h[t] = x;
        }
    }
    static Pos start() { Pos s; s.initHash(); return s; }

    bool over() const { return passes >= 2 || (avail[0] == 0 && avail[1] == 0); }
    BB occ() const { return own[0] | own[1]; }
    BB empty() const { return T.VALID & ~(own[0] | own[1]); }
    uint8_t movable() const { return ply == 0 ? (avail[stm] & 1) : avail[stm]; }

    // canonical (symmetry-reduced) key; returns transform used via t
    // Symmetry-canonical key near the root (where mirrored transpositions happen); deeper
    // positions are practically never symmetric and use the plain hash.
    uint64_t key(int& t) const {
        t = 0;
        if (ply > SYM_PLY) return h[0];
        for (int u = 1; u < NSYM; u++) if (h[u] < h[t]) t = u;
        return h[t];
    }
    // The key of the position after move m (not a pass), without making it; only once the symmetric
    // hashes are no longer kept (ply >= SYM_PLY), where play() updates h[0] alone.
    uint64_t keyAfter(Move m) const {
        const Placement& pl = m.pl();
        uint64_t x = h[0] ^ T.zPiece[stm][pl.piece] ^ T.zStm;
        for (int j = 0; j < pl.n; j++) x ^= T.zCell[stm][pl.c[j]];
        return x;
    }
    // 32 more bits that tell positions with the same key apart (the transposition table stores them),
    // from the cells and pieces, independently of the Zobrist key. Near the root, where the key is
    // symmetry-canonical, it is 0 (those few positions do not need it).
    uint32_t keyCheck() const {
        if (ply <= SYM_PLY) return 0;
        uint64_t x = (uint64_t)own[0] * 0x9E3779B97F4A7C15ULL ^ (uint64_t)(own[0] >> 64) * 0xC2B2AE3D27D4EB4FULL ^
                     (uint64_t)own[1] * 0x165667B19E3779F9ULL ^ (uint64_t)(own[1] >> 64) * 0x27D4EB2F165667C5ULL ^
                     uint64_t(avail[0] | avail[1] << 8 | stm << 16) * 0x85EBCA77C2B2AE63ULL;
        return uint32_t(x >> 32);
    }

    // Straightforward per-placement legality check (reference for the fast generator).
    bool legal(const Placement& pl) const { return legalFor(stm, pl); }
    // The same check for player c, whether or not it is c's turn (used for threats).
    bool legalFor(int c, const Placement& pl) const {
        BB occ = own[0] | own[1];
        if (pl.m & occ) return false;
        BB me = own[c];
        if ((pl.nb & occ) && !(pl.nb & me)) return false;  // new snake must touch nothing
        BB nw = me | pl.m;
        for (int j = 0; j < pl.n; j++) {
            BB n = T.NB[pl.c[j]] & nw;
            int d = popc(n);
            if (d > 2) return false;
            if (d == 2) {  // sharp turn = triangle
                int a = lsb(n);
                if (T.NB[a] & (n & (n - 1))) return false;
            }
        }
        BB adj = pl.nb & me;
        while (adj) {
            int i = lsb(adj); adj &= adj - 1;
            if (popc(T.NB[i] & nw) > 2) return false;
        }
        return true;
    }
    bool legalMove(Move m) const {
        if (m.isPass()) return !hasMove();
        if (!(movable() >> m.piece() & 1)) return false;
        return legal(m.pl());
    }

    void genCtx(GenCtx& g) const {
        BB occ = own[0] | own[1], E = T.VALID & ~occ;
        Shape sh = shapeOf(own[stm]);
        g.iso = E & ~expand(occ);
        BB base = E & ~expand(sh.full);
        g.allow[0] = base & ~sh.ge3 & ~sh.tri2;
        g.allow[1] = base & ~sh.ge2;
        g.allow[2] = base & ~sh.ge1;
        g.adjOwn = E & sh.ge1;
        g.me = sh.own;
        g.meDeg1 = sh.own & sh.ge1;
    }
    template <class F>
    void forEachLegal(F&& f) const { forEachLegalImpl<false, false>(0, f); }
    // only the legal placements covering at least one cell of `touch`
    template <class F>
    void forEachLegalTouching(BB touch, F&& f) const { forEachLegalImpl<true, false>(touch, f); }
    // only the legal placements that grow an own snake
    template <class F>
    void forEachLegalGrow(F&& f) const { forEachLegalImpl<false, true>(0, f); }
    template <bool Touching, bool GrowOnly, class F>
    void forEachLegalImpl(BB touch, F&& f) const {
        uint8_t av = movable();
        if (!av) return;
        GenCtx g;
        genCtx(g);
        if (GrowOnly && !g.adjOwn) return;
        for (int p = 0; p < NPIECE; p++) {
            if (!(av >> p & 1)) continue;
            for (int o = T.orBegin[p]; o < T.orBegin[p + 1]; o++) {
                const Orient& od = T.OR[o];
                BB lg = GrowOnly ? orientGrowLegal(od, g) : orientLegal(od, g);
                if (Touching && lg) {
                    BB anchors = 0;
                    for (int k = 0; k < od.n; k++) anchors |= shiftFrom(touch, od.dq[k], od.dr[k]);
                    lg &= anchors;
                }
                while (lg) { int a = lsb(lg); lg &= lg - 1; f(Move{od.gidAt[a]}); }
            }
        }
    }
    void genMoves(std::vector<Move>& out) const {
        out.clear();
        forEachLegal([&](Move m) { out.push_back(m); });
    }
    bool hasMove() const {
        uint8_t av = movable();
        if (!av) return false;
        GenCtx g;
        genCtx(g);
        for (int p = 0; p < NPIECE; p++) {
            if (!(av >> p & 1)) continue;
            for (int o = T.orBegin[p]; o < T.orBegin[p + 1]; o++) if (orientLegal(T.OR[o], g)) return true;
        }
        return false;
    }

    void play(Move mv) {
        if (mv.isPass()) {
            passes++;
        } else {
            passes = 0;
            const Placement& pl = mv.pl();
            int p = mv.piece();
            own[stm] |= pl.m;
            avail[stm] &= ~(1u << p);
            int nh = ply < SYM_PLY ? NSYM : 1;
            for (int t = 0; t < nh; t++) {
                uint64_t x = h[t] ^ T.zPiece[stm][p];
                for (int j = 0; j < pl.n; j++) x ^= T.zCell[stm][T.perm[t][pl.c[j]]];
                h[t] = x;
            }
            // merge the snakes the piece touches
            BB merged = pl.m;
            int k = 0;
            for (int j = 0; j < ncomp[stm]; j++) {
                if (comp[stm][j] & pl.nb) merged |= comp[stm][j];
                else comp[stm][k++] = comp[stm][j];
            }
            comp[stm][k++] = merged;
            ncomp[stm] = (uint8_t)k;
            L[stm] = std::max<int>(L[stm], popc(merged));
        }
        int nh = ply < SYM_PLY ? NSYM : 1;
        for (int t = 0; t < nh; t++) h[t] ^= T.zStm;
        stm ^= 1;
        ply++;
    }
};

// Component analysis
inline BB componentOf(BB b, int i) { return flood(bit(i), b); }
inline int longest(BB b) {
    int best = 0;
    while (b) {
        BB c = componentOf(b, lsb(b));
        best = std::max(best, popc(c));
        b &= ~c;
    }
    return best;
}
inline int remainingCells(uint8_t av) {
    // pieces 0-1 have 1 and 2 cells, 2-3 three, 4-7 four
    static_assert(PIECE_CELLS[P1] == 1 && PIECE_CELLS[P2] == 2 && PIECE_CELLS[P3I] == 3 && PIECE_CELLS[P3V] == 3 &&
                  PIECE_CELLS[P4I] == 4 && PIECE_CELLS[P4A] == 4 && PIECE_CELLS[P4B] == 4 && PIECE_CELLS[P4C] == 4);
    return (av & 1) + (av & 2) + 3 * __builtin_popcount(av & 0x0C) + 4 * __builtin_popcount(av & 0xF0);
}
inline int pieceCount(uint8_t av) { return __builtin_popcount(av); }

// Lower / upper bound of the final longest snake of player c (sound).
//  - A player can never again occupy a cell adjacent to one of its "full" cells
//    (that cell would get 3 own neighbours) -> growable cells G = empty & ~N(full).
//  - The final snake is connected and lies in own cells + G, so it lies inside one
//    connected cluster K of (own | G). New cells come from whole pieces, and each
//    piece lies inside one connected region of G, so the new cells in K are at most
//    fit(sum over G-regions r in K of fit(|r|)).
//  - Every old snake ends up whole inside the final snake or not at all, and the final
//    snake is a path in which old snakes and new pieces alternate, so with k pieces left
//    it contains at most k + 1 old snakes: only the k + 1 longest of the cluster count.
// The clusters themselves (cells and per-cluster upper bound), for threat detection.
struct Clusters {
    int n = 0, nRegions = 0;
    BB cells[NCELLS];    // growable cells and own snakes of the cluster
    int ub[NCELLS];      // upper bound of a final snake inside the cluster
    BB region[NCELLS];   // the connected regions of growable cells (of all clusters)
};
// Upper bound (at least lb) of the final snake of a player with pieces av, snakes comps and
// growable cells G, cluster by cluster as described above.
template <bool Record>
inline int clusterBound(BB G, const BB* comps, int ncomp, uint8_t av, int lb, Clusters* out) {
    if (Record) out->n = out->nRegions = 0;
    // clusters = growable regions joined through the snakes touching them. Regions are
    // compact blobs (cheap floods); snakes are attached by adjacency, never flooded.
    BB cl[NCELLS];
    int own[NCELLS], cap[NCELLS], n = 0;
    BB g = G;
    while (g) {
        BB r = flood(bit(lsb(g)), g);
        g &= ~r;
        cl[n] = r; own[n] = 0; cap[n] = T.fit[av][popc(r)]; n++;
        if (Record) out->region[out->nRegions++] = r;
    }
    for (int j = 0; j < ncomp; j++) {
        BB cm = comps[j], nb = expand(cm);
        int into = -1;
        for (int i = 0; i < n; i++) {
            if (i == into || !(cl[i] & nb)) continue;
            if (into < 0) { into = i; continue; }
            // the snake joins cluster i into cluster `into`; fill slot i with the last cluster
            cl[into] |= cl[i]; own[into] += own[i]; cap[into] += cap[i];
            n--;
            cl[i] = cl[n]; own[i] = own[n]; cap[i] = cap[n];
            if (into == n) into = i;
            i--;
        }
        if (into >= 0) { cl[into] |= cm; own[into] += popc(cm); }
    }
    int joinable = pieceCount(av) + 1;
    if (ncomp > joinable) {
        for (int i = 0; i < n; i++) {
            int len[8], m = 0;
            for (int j = 0; j < ncomp; j++)
                if (cl[i] & comps[j]) len[m++] = popc(comps[j]);
            if (m <= joinable) continue;
            std::partial_sort(len, len + joinable, len + m, std::greater<int>());
            own[i] = 0;
            for (int j = 0; j < joinable; j++) own[i] += len[j];
        }
    }
    int ub = lb;
    for (int i = 0; i < n; i++) {
        int u = own[i] + T.fit[av][std::min(cap[i], NIDX)];
        ub = std::max(ub, u);
        if (Record) { out->cells[out->n] = cl[i]; out->ub[out->n++] = u; }
    }
    return ub;
}
// Growable cells of player c: empty cells not next to one of its full cells.
inline BB growable(const Pos& s, int c) { return s.empty() & ~expand(shapeOf(s.own[c]).full); }
template <bool Record>
inline void snakeBoundsImpl(const Pos& s, int c, int& lb, int& ub, Clusters* out) {
    lb = s.L[c];
    if (Record) out->n = 0;
    uint8_t av = s.avail[c];
    if (!av) { ub = lb; return; }
    ub = clusterBound<Record>(growable(s, c), s.comp[c], s.ncomp[c], av, lb, out);
}
inline void snakeBounds(const Pos& s, int c, int& lb, int& ub) { snakeBoundsImpl<false>(s, c, lb, ub, nullptr); }
inline void snakeClusters(const Pos& s, int c, int& lb, int& ub, Clusters& out) { snakeBoundsImpl<true>(s, c, lb, ub, &out); }
// Upper bound of player c after cells m (empty now) are taken by the opponent, given c's
// clusters: only the clusters that m covers change, and only those are recomputed.
// When m's growable cells all lie in one region (they can be split by a cell that is not growable,
// and then the cluster is recomputed) and the region stays connected without m
// (the cells next to m are still connected within distance 2 of m), every snake keeps
// touching it and the region still holds as many piece cells, the cluster bound is unchanged.
inline int boundAfterBlock(const Pos& s, int c, const Clusters& cl, int lb, BB m) {
    int ub = lb;
    uint8_t av = s.avail[c];
    for (int i = 0; i < cl.n; i++) {
        if (!(cl.cells[i] & m)) { ub = std::max(ub, cl.ub[i]); continue; }
        BB rest = cl.cells[i] & ~s.own[c] & ~m;
        BB comps[8];
        int nc = 0;
        for (int j = 0; j < s.ncomp[c]; j++)
            if (cl.cells[i] & s.comp[c][j]) comps[nc++] = s.comp[c][j];
        BB r = 0, mg = m & cl.cells[i] & ~s.own[c];  // the blocked growable cells
        for (int k = 0; k < cl.nRegions; k++) if (cl.region[k] & mg) { r = cl.region[k]; break; }
        BB after = r & ~m, near = expand(m) & after;
        bool unchanged = near && !(mg & ~r) && T.fit[av][popc(after)] == T.fit[av][popc(r)];
        if (unchanged) {
            BB around = flood(bit(lsb(near)), expand(expand(m)) & after);
            unchanged = !(near & ~around);
            for (int j = 0; j < nc && unchanged; j++)
                if ((expand(comps[j]) & r) && !(expand(comps[j]) & after)) unchanged = false;
        }
        ub = std::max(ub, unchanged ? cl.ub[i] : clusterBound<false>(rest, comps, nc, av, lb, nullptr));
    }
    return ub;
}
// Longest snake of player c if c placed pl (a legal placement for c) now.
inline int lengthAfter(const Pos& s, int c, const Placement& pl) {
    int merged = pl.n;
    for (int j = 0; j < s.ncomp[c]; j++)
        if (s.comp[c][j] & pl.nb) merged += popc(s.comp[c][j]);
    return std::max<int>(s.L[c], merged);
}

// Exact one-player optimum: could player c make a snake of at least `target` cells if the
// opponent never moved again? It is a relaxation of the real game, so a "no" is a sound upper
// bound, and much tighter than clusterBound, because it follows how the fixed piece shapes chain:
//  - c's future cells are growable cells, and the final snake lies in one cluster (as above);
//  - the opponent's future cells, the order of c's moves and the rule that a new snake touches
//    nothing are dropped (each only removes options);
//  - the final snake is an induced path or ring of whole old snakes and whole new pieces, each new
//    piece attached at an end of the path built so far (a ring is closed by the last one).
// A depth-first search grows the path from a seed: an old snake (merging an old snake with a lower
// index is skipped, that path is grown from that snake), or for a path of new pieces only, its
// lowest piece. It extends one end, then the other, with the grow rules of orientLegal narrowed
// to "touches the end being extended", and stops where the remaining pieces and the snakes they
// could merge are too short. The last piece goes to either end in one step. Answers are memoised
// per cluster on (cluster cells, own cells, pieces): nothing outside the cluster matters.
struct OnePlayer {
    static constexpr uint64_t DFS_BUDGET = 20000;  // search nodes per cluster; beyond it the answer is "maybe"
    static constexpr int MEMO_LOG2 = 16;
    struct MemoEntry { BB cells = 0, own = 0; uint8_t av = 0; int8_t reachable = 0, unreachable = 127; };
    std::vector<MemoEntry> memo = std::vector<MemoEntry>(size_t(1) << MEMO_LOG2);
    uint64_t queries = 0, unreachableAnswers = 0, dfsNodes = 0, outOfBudget = 0;
    void clear() { std::fill(memo.begin(), memo.end(), MemoEntry{}); }

    // false only if c surely cannot reach `target`; cl: c's clusters (snakeClusters)
    bool mayReach(const Pos& s, int c, const Clusters& cl, int target) {
        if (s.L[c] >= target) return true;
        uint8_t av = s.avail[c];
        if (!av) return false;
        queries++;
        for (int i = 0; i < cl.n; i++) {
            if (cl.ub[i] < target) continue;
            BB K = cl.cells[i], ownK = s.own[c] & K;
            uint64_t h = (uint64_t)K * 0x9E3779B97F4A7C15ULL ^ (uint64_t)(K >> 64) * 0xC2B2AE3D27D4EB4FULL ^
                         (uint64_t)ownK * 0x165667B19E3779F9ULL ^ (uint64_t)(ownK >> 64) * 0x27D4EB2F165667C5ULL ^ av * 0x85EBCA77C2B2AE63ULL;
            MemoEntry& e = memo[(h ^ (h >> 29)) & (memo.size() - 1)];
            if (e.cells != K || e.own != ownK || e.av != av) e = MemoEntry{K, ownK, av, 0, 127};
            if (e.reachable >= target) return true;
            if (e.unreachable <= target) continue;
            BB comps[8];
            int nc = 0;
            for (int j = 0; j < s.ncomp[c]; j++) if (s.comp[c][j] & K) comps[nc++] = s.comp[c][j];
            int r = reachIn(ownK, K & ~s.own[c], comps, nc, av, target);
            if (r != 0) {
                if (r == 1) e.reachable = (int8_t)target;
                else outOfBudget++;
                return true;
            }
            e.unreachable = (int8_t)target;
        }
        unreachableAnswers++;
        return false;
    }

  private:
    BB own = 0, freeCells = 0;  // own cells of the cluster, its growable cells
    BB comps[8];
    int ncomp = 0, compLen[8];
    int target = 0;
    bool newOnly = false;       // a path of new pieces only: it never touches an old snake
    int minPiece = 0;           // ... and its other pieces have a higher index than its seed piece
    int seedComp = -1;          // the seed snake: snakes with a lower index are not merged
    uint64_t used = 0;

    // 1 reachable, 0 not, -1 out of budget. P: the path (a component of M), keptEnd: the end not
    // extended now; secondEnd: the first end is finished and the other one is being extended.
    int dfs(BB P, BB M, BB placed, uint8_t av, int keptEnd, bool secondEnd) {
        int len = popc(P);
        if (len >= target) return 1;
        if (!av) return 0;
        if (++used > DFS_BUDGET) return -1;
        // cheap count first: every remaining piece and the longest snakes it could merge
        int quick = len + remainingCells(av);
        if (!newOnly) {
            int k = pieceCount(av), lens[8], m = 0;
            for (int j = 0; j < ncomp; j++) if (!(comps[j] & P)) lens[m++] = compLen[j];
            int merged = std::min(k, m);
            std::partial_sort(lens, lens + merged, lens + m, std::greater<int>());
            for (int j = 0; j < merged; j++) quick += lens[j];
        }
        if (quick < target) return 0;
        Shape sm = shapeOf(M);
        BB E = P & sm.ends;  // P is a component of M
        if (!E) return 0;    // a ring
        int a = popc(E) == 1 ? lsb(E) : (E & ~bit(keptEnd)) ? lsb(E & ~bit(keptEnd)) : lsb(E);
        BB active = secondEnd ? bit(a) : E;
        // with one piece left, placing it at either active end is the last step
        bool lastPiece = (av & (av - 1)) == 0;
        int childKept = popc(E) == 1 ? a : keptEnd;
        BB extendAt = lastPiece ? active : bit(a);
        int e0 = lsb(extendAt), e1 = extendAt & (extendAt - 1) ? lsb(extendAt & (extendAt - 1)) : e0;
        // legal extensions, bit-parallel per orientation: orientLegal's grow rules with "touches
        // own" narrowed to "touches the end being extended"
        BB A = freeCells & ~placed;
        if (newOnly) A &= ~expand(own);
        BB base = A & ~expand(sm.full);
        BB allow[3] = {base & ~sm.ge3 & ~sm.tri2, base & ~sm.ge2, base & ~sm.ge1};
        BB meDeg1 = M & sm.ge1;
        int result = 0;
        for (int p = NPIECE - 1; p >= 0; p--) {  // big pieces first: a path that reaches is found sooner
            if (!(av >> p & 1) || (newOnly && p <= minPiece)) continue;
            for (int o = T.orBegin[p]; o < T.orBegin[p + 1]; o++) {
                const Orient& od = T.OR[o];
                BB gr = od.nextTo[e0] | od.nextTo[e1];
                for (int k = 0; k < od.n && gr; k++) gr &= shiftFrom(allow[od.ideg[k]], od.dq[k], od.dr[k]);
                if (!gr) continue;
                for (auto [q, r] : od.tri) gr &= ~shiftFrom(M, q, r);
                for (auto [q, r] : od.two) gr &= ~shiftFrom(meDeg1, q, r);
                while (gr) {
                    int anchor = lsb(gr);
                    gr &= gr - 1;
                    const Placement& pl = T.placements[od.gidAt[anchor]];
                    // the piece joins P and every old snake it touches
                    BB P2 = P | pl.m;
                    bool lower = false;
                    for (int j = 0; j < ncomp; j++)
                        if ((comps[j] & pl.nb) && !(comps[j] & P)) {
                            if (!newOnly && j < seedComp) lower = true;
                            P2 |= comps[j];
                        }
                    if (lastPiece) {
                        if (popc(P2) >= target) return 1;
                        continue;
                    }
                    if (lower) continue;
                    int r = dfs(P2, M | pl.m, placed | pl.m, (uint8_t)(av & ~(1u << p)), childKept, secondEnd);
                    if (r == 1) return 1;
                    if (r < 0) result = -1;
                    if (used > DFS_BUDGET) return -1;
                }
            }
        }
        if (!lastPiece && !secondEnd && popc(E) == 2) {
            int r = dfs(P, M, placed, av, a, true);
            if (r != 0) return r;
        }
        return result;
    }

    // The decision inside one cluster: own cells, free growable cells, its snakes.
    int reachIn(BB own_, BB free_, const BB* comps_, int ncomp_, uint8_t av, int target_) {
        target = target_;
        own = own_;
        freeCells = free_;
        ncomp = ncomp_;
        for (int j = 0; j < ncomp; j++) { comps[j] = comps_[j]; compLen[j] = popc(comps[j]); }
        used = 0;
        int r = searchSeeds(av);
        dfsNodes += used;
        return r;
    }
    int searchSeeds(uint8_t av) {
        int result = 0;
        // seeds: the old snakes
        newOnly = false;
        for (int j = 0; j < ncomp; j++) {
            seedComp = j;
            BB E = shapeOf(comps[j]).ends;
            if (!E) continue;
            int r = dfs(comps[j], own, 0, av, lsb(E), false);
            if (r == 1) return 1;
            if (r < 0) result = -1;
            if (used > DFS_BUDGET) return -1;
        }
        // seeds: paths of new pieces only, seeded by their lowest piece. Such a path lies in one
        // region of the growable cells away from the old snakes, which must hold target cells.
        if (remainingCells(av) < target) return result;
        newOnly = true;
        BB fr = freeCells & ~expand(own), ok = 0;
        for (BB rest = fr; rest;) {
            BB r = flood(bit(lsb(rest)), fr);
            rest &= ~r;
            if (T.fit[av][std::min(popc(r), NIDX)] >= target) ok |= r;
        }
        if (!ok) return result;
        for (int p = 0; p < NPIECE; p++) {
            if (!(av >> p & 1)) continue;
            uint8_t higher = (uint8_t)(av & ~((2u << p) - 1));
            if (PIECE_CELLS[p] + remainingCells(higher) < target) continue;
            minPiece = p;
            for (int o = T.orBegin[p]; o < T.orBegin[p + 1]; o++) {
                const Orient& od = T.OR[o];
                BB anchors = T.VALID;
                for (int k = 0; k < od.n; k++) anchors &= shiftFrom(ok, od.dq[k], od.dr[k]);
                while (anchors) {
                    int anchor = lsb(anchors);
                    anchors &= anchors - 1;
                    const Placement& pl = T.placements[od.gidAt[anchor]];
                    int r = dfs(pl.m, own | pl.m, pl.m, higher, lsb(shapeOf(pl.m).ends), false);
                    if (r == 1) return 1;
                    if (r < 0) result = -1;
                    if (used > DFS_BUDGET) return -1;
                }
            }
        }
        return result;
    }
};

inline int finalScore(const Pos& s) {  // from player 1's perspective: >0 player 1 wins
    return s.L[0] - s.L[1];
}

// ---- text notation ------------------------------------------------------------
inline std::string moveStr(Move m) {
    if (m.isPass()) return "pass";
    std::string s = PNAME[m.piece()];
    s += ':';
    const Placement& pl = m.pl();
    std::vector<int> cs(pl.c, pl.c + pl.n);
    std::sort(cs.begin(), cs.end());
    for (size_t j = 0; j < cs.size(); j++) { if (j) s += ','; s += std::to_string(cs[j]); }
    return s;
}
inline bool parseMove(const std::string& tok, Move& out) {
    if (tok == "pass") { out = Move{}; return true; }
    auto col = tok.find(':');
    if (col == std::string::npos) return false;
    std::string pn = tok.substr(0, col);
    int p = -1;
    for (int k = 0; k < NPIECE; k++) if (pn == PNAME[k]) p = k;
    if (p < 0) return false;
    BB m = 0;
    size_t i = col + 1;
    while (i < tok.size()) {
        size_t j = tok.find(',', i);
        if (j == std::string::npos) j = tok.size();
        int c = std::atoi(tok.substr(i, j - i).c_str());
        if (c < 0 || c >= NIDX) return false;
        m |= bit(c);
        i = j + 1;
    }
    for (auto& pl : T.PL[p]) if (pl.m == m) { out = Move{(int16_t)pl.gid}; return true; }
    return false;
}
