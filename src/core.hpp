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
#include <string>
#include <vector>

typedef unsigned __int128 BB;

constexpr int W = 11, R = 5, NIDX = 121, NCELLS = 91;
constexpr int NPIECE = 8;
constexpr int NSYM = 12;

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
inline const int PSIZE[NPIECE] = {1, 2, 3, 3, 4, 4, 4, 4};
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
};

struct Tables {
    BB VALID = 0, NOT_QMAX = 0, NOT_QMIN = 0;
    BB COLR[17];  // COLR[dq+8]: cells whose q lies in [-5+dq, 5+dq]
    BB NB[NIDX];
    int cells[NCELLS];
    int perm[NSYM][NIDX];  // cell -> transformed cell
    int inv[NSYM];
    std::vector<Placement> PL[NPIECE];
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
    T.OR.clear();
    for (int p = 0; p < NPIECE; p++) {
        T.orBegin[p] = (int)T.OR.size();
        std::vector<std::vector<std::pair<int, int>>> ors;
        for (int t = 0; t < NSYM; t++) {
            std::vector<std::pair<int, int>> o;
            for (auto [q, r] : PSHAPE[p]) o.push_back(symApply(t, q, r));
            std::sort(o.begin(), o.end(), [](auto a, auto b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
            auto b0 = o[0];
            for (auto& c : o) { c.first -= b0.first; c.second -= b0.second; }
            if (std::find(ors.begin(), ors.end(), o) == ors.end()) ors.push_back(o);
        }
        T.PL[p].clear();
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
            for (int i = 0; i < NIDX; i++) od.gidAt[i] = -1;
            for (int c : T.cells) {
                Placement pl{};
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
                T.gidInfo.push_back({p, (int)T.PL[p].size()});
                T.PL[p].push_back(pl);
            }
            T.OR.push_back(od);
        }
    }
    T.orBegin[NPIECE] = (int)T.OR.size();
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
                for (int s = NIDX; s >= PSIZE[p]; s--) if (can[s - PSIZE[p]]) can[s] = true;
        int best = 0;
        for (int n = 0; n <= NIDX; n++) { if (can[n]) best = n; T.fit[m][n] = (uint8_t)best; }
    }
}

// ---- moves & positions ------------------------------------------------------
struct Move {
    int16_t gid = -1;  // -1 = pass
    bool isPass() const { return gid < 0; }
    int piece() const { return T.gidInfo[gid].first; }
    const Placement& pl() const { auto [p, i] = T.gidInfo[gid]; return T.PL[p][i]; }
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

inline BB orientLegal(const Orient& o, const GenCtx& g) {
    BB nw = T.VALID;
    for (int k = 0; k < o.n; k++) nw &= shiftFrom(g.iso, o.dq[k], o.dr[k]);
    if (!g.adjOwn) return nw;
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
    return nw | gr;
}

struct Pos {
    BB own[2] = {0, 0};
    uint8_t avail[2] = {ALL_PIECES, P2_PIECES};
    uint8_t stm = 0;     // 0 = player 1 (red / starts), 1 = player 2
    uint8_t passes = 0;  // consecutive passes
    uint8_t ply = 0;
    uint64_t h[NSYM] = {};

    void initHash() {
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
    uint64_t key(int& t) const {
        t = 0;
        for (int u = 1; u < NSYM; u++) if (h[u] < h[t]) t = u;
        return h[t];
    }

    // Straightforward per-placement legality check (reference for the fast generator).
    bool legal(const Placement& pl) const {
        BB occ = own[0] | own[1];
        if (pl.m & occ) return false;
        BB me = own[stm];
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
    void forEachLegal(F&& f) const {
        uint8_t av = movable();
        if (!av) return;
        GenCtx g;
        genCtx(g);
        for (int p = 0; p < NPIECE; p++) {
            if (!(av >> p & 1)) continue;
            for (int o = T.orBegin[p]; o < T.orBegin[p + 1]; o++) {
                const Orient& od = T.OR[o];
                BB lg = orientLegal(od, g);
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
            for (int t = 0; t < NSYM; t++) {
                uint64_t x = h[t] ^ T.zPiece[stm][p];
                for (int j = 0; j < pl.n; j++) x ^= T.zCell[stm][T.perm[t][pl.c[j]]];
                h[t] = x;
            }
        }
        for (int t = 0; t < NSYM; t++) h[t] ^= T.zStm;
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
    int s = 0;
    for (int p = 0; p < NPIECE; p++) if (av >> p & 1) s += PSIZE[p];
    return s;
}
inline int pieceCount(uint8_t av) { return __builtin_popcount(av); }

// Lower / upper bound of the final longest snake of player c (sound).
//  - A player can never again occupy a cell adjacent to one of its "full" cells
//    (that cell would get 3 own neighbours) -> growable cells G = empty & ~N(full).
//  - The final snake is connected and lies in own cells + G, so it lies inside one
//    connected cluster K of (own | G). New cells come from whole pieces, and each
//    piece lies inside one connected region of G, so the new cells in K are at most
//    fit(sum over G-regions r in K of fit(|r|)).
inline void snakeBounds(const Pos& s, int c, int& lb, int& ub) {
    BB me = s.own[c];
    lb = longest(me);
    uint8_t av = s.avail[c];
    if (!av) { ub = lb; return; }
    Shape sh = shapeOf(me);
    BB G = s.empty() & ~expand(sh.full);
    BB U = me | G;
    ub = lb;
    while (U) {
        BB K = flood(bit(lsb(U)), U);
        U &= ~K;
        BB g = K & G;
        if (!g) continue;
        int cap = 0;
        while (g) {
            BB r = flood(bit(lsb(g)), g);
            g &= ~r;
            cap += T.fit[av][popc(r)];
        }
        int v = popc(K & me) + T.fit[av][std::min(cap, NIDX)];
        ub = std::max(ub, v);
    }
}
inline int finalScore(const Pos& s) {  // from player 1's perspective: >0 player 1 wins
    return longest(s.own[0]) - longest(s.own[1]);
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
