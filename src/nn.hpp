// Network evaluation for the "nn" engine: an efficiently updatable perspective network
// (NNUE style), int16 with AVX2 (and a scalar path that computes the same).
//
// Inputs, from one player's perspective ("me"), local functions of the board (a move changes only
// the cells it covers and their neighbourhood) except for the bounds:
//   per player (mine, then the opponent's), one plane of 91 cells each:
//     full   own cells with 2 own neighbours (snake body, can never be touched again)
//     end    the other own cells (snake ends and single cells)
//     front  empty cells next to an own end that the player may still grow into
//     dead   empty cells next to an own full cell (the player can never occupy them)
//   iso      empty cells next to no tile (where a new snake can start)
//   the pieces each player has left (8 + 8), and one-hot (capped) each player's longest snake and
//   the upper bound of its final snake (snakeBounds).
// Both perspectives share the first layer: accumulator acc[p] = b1 + sum of the rows of p's active
// inputs, kept for both players and updated from the parent position by the inputs that changed.
// Output: SCReLU(acc[side to move]) and SCReLU(acc[opponent]) times the output weights of the
// ply's bucket: the logit of P(side to move wins), draws counting 1/2, as the logistic evaluation.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "core.hpp"
#if defined(__AVX2__)
#include <immintrin.h>
#endif
// The default network, compiled in as base64 text (NN_NET_B64, written by tools/train_nn.py --embed),
// in every build: the WebAssembly one runs it with the scalar code.
#if __has_include("nn_net.hpp")
#include "nn_net.hpp"
#define NN_HAVE_DEFAULT_NET 1
#else
#define NN_HAVE_DEFAULT_NET 0
#endif
// The default network's move ranking (NN_POLICY_B64, written by tools/train_policy.py --embed).
#if NN_HAVE_DEFAULT_NET && __has_include("nn_policy.hpp")
#include "nn_policy.hpp"
#define NN_HAVE_DEFAULT_POLICY 1
#else
#define NN_HAVE_DEFAULT_POLICY 0
#endif

namespace nn {

constexpr bool HAVE_DEFAULT_NET = NN_HAVE_DEFAULT_NET;
constexpr bool HAVE_DEFAULT_POLICY = NN_HAVE_DEFAULT_POLICY;

constexpr int NC = NCELLS;
enum PlayerPlane { PP_FULL, PP_END, PP_FRONT, PP_DEAD, N_PLAYER_PLANES };
constexpr int LEN_CAP = 24;  // longest snake one-hot: 0..LEN_CAP (longer counts as LEN_CAP)
constexpr int F_MINE = 0, F_OPP = N_PLAYER_PLANES * NC, F_ISO = 2 * N_PLAYER_PLANES * NC;
constexpr int F_PIECES = F_ISO + NC;              // my pieces (8), then the opponent's (8)
constexpr int F_LEN = F_PIECES + 2 * NPIECE;      // my longest snake (LEN_CAP + 1), then the opponent's
constexpr int UB_CAP = 25;                        // upper bound of the final snake (snakeBounds), one-hot
constexpr int F_UB = F_LEN + 2 * (LEN_CAP + 1);   // mine (UB_CAP + 1), then the opponent's
constexpr int N_INPUTS = F_UB + 2 * (UB_CAP + 1);
constexpr int HIDDEN = 128;
constexpr int BUCKETS = 16;                       // output bucket = ply (capped)
constexpr int QA = 255, QB = 128;
static_assert(HIDDEN % 16 == 0, "the AVX2 code needs HIDDEN to be a multiple of 16");

inline int bucketOf(int ply) { return std::min(ply, BUCKETS - 1); }

// cell index (0..120) -> input cell (0..90), and the symmetry maps of the inputs (for the trainer)
struct CellMap {
    int16_t cellOf[NIDX];
    bool ready = false;
    void init() {
        for (auto& c : cellOf) c = -1;
        for (int k = 0; k < NC; k++) cellOf[T.cells[k]] = (int16_t)k;
        ready = true;
    }
};
inline CellMap CM;

// The board planes the inputs are read from (per player) and the non-cell inputs.
struct Planes {
    BB pl[2][N_PLAYER_PLANES];
    BB iso;
    uint8_t avail[2];
    uint8_t len[2];
    uint8_t ub[2];
    bool operator==(const Planes&) const = default;
};

inline void boundsOf(const Pos& s, Planes& P) {
    for (int c = 0; c < 2; c++) {
        int lb, ub;
        snakeBounds(s, c, lb, ub);
        P.ub[c] = (uint8_t)std::min(ub, UB_CAP);
    }
}

inline void planesOf(const Pos& s, Planes& P) {
    BB occ = s.own[0] | s.own[1], E = T.VALID & ~occ;
    for (int c = 0; c < 2; c++) {
        Shape sh = shapeOf(s.own[c]);
        BB dead = E & expand(sh.full);
        P.pl[c][PP_FULL] = sh.full;
        P.pl[c][PP_END] = sh.ends;
        P.pl[c][PP_FRONT] = s.avail[c] ? expand(sh.ends) & E & ~dead : 0;
        P.pl[c][PP_DEAD] = dead;
        P.avail[c] = s.avail[c];
        P.len[c] = (uint8_t)std::min<int>(s.L[c], LEN_CAP);
    }
    P.iso = E & ~expand(occ);
    boundsOf(s, P);
}

// The planes of child = s after the side to move places pl (legal), from s's planes P (ub: the
// child's snakeBounds upper bounds when the caller has them already).
inline void planesAfter(const Pos& s, const Planes& P, const Placement& pl, int piece, const Pos& child, Planes& Q,
                        const int* ub = nullptr) {
    int me = s.stm, op = me ^ 1;
    BB occ = s.own[0] | s.own[1] | pl.m, E = T.VALID & ~occ;
    BB own = s.own[me] | pl.m;
    Shape sh = shapeOf(own);
    BB dead = E & expand(sh.full);
    uint8_t av = s.avail[me] & ~(1u << piece);
    Q.pl[me][PP_FULL] = sh.full;
    Q.pl[me][PP_END] = sh.ends;
    Q.pl[me][PP_FRONT] = av ? expand(sh.ends) & E & ~dead : 0;
    Q.pl[me][PP_DEAD] = dead;
    // the opponent's cells are unchanged; its empty-cell planes lose the covered cells
    Q.pl[op][PP_FULL] = P.pl[op][PP_FULL];
    Q.pl[op][PP_END] = P.pl[op][PP_END];
    Q.pl[op][PP_FRONT] = P.pl[op][PP_FRONT] & ~pl.m;
    Q.pl[op][PP_DEAD] = P.pl[op][PP_DEAD] & ~pl.m;
    Q.avail[me] = av;
    Q.avail[op] = P.avail[op];
    Q.len[me] = (uint8_t)std::min<int>(lengthAfter(s, me, pl), LEN_CAP);
    Q.len[op] = P.len[op];
    Q.iso = P.iso & ~(pl.m | pl.nb);
    if (ub) {
        for (int c = 0; c < 2; c++) Q.ub[c] = (uint8_t)std::min(ub[c], UB_CAP);
        return;
    }
    boundsOf(child, Q);
}

// Active inputs of perspective p.
template <class F>
inline void forEachInput(const Planes& P, int p, F&& f) {
    for (int c = 0; c < 2; c++) {
        int base = c == p ? F_MINE : F_OPP;
        for (int k = 0; k < N_PLAYER_PLANES; k++)
            for (BB b = P.pl[c][k]; b; b &= b - 1) f(base + k * NC + CM.cellOf[lsb(b)]);
        for (int q = 0; q < NPIECE; q++)
            if (P.avail[c] >> q & 1) f(F_PIECES + (c == p ? 0 : NPIECE) + q);
        f(F_LEN + (c == p ? 0 : LEN_CAP + 1) + P.len[c]);
        f(F_UB + (c == p ? 0 : UB_CAP + 1) + P.ub[c]);
    }
    for (BB b = P.iso; b; b &= b - 1) f(F_ISO + CM.cellOf[lsb(b)]);
}

// Inputs that differ between P and Q, for perspective p: added to `add`, removed to `sub`.
struct Delta {
    int16_t add[2][160], sub[2][160];
    int nAdd[2] = {0, 0}, nSub[2] = {0, 0};
};
inline void deltaOf(const Planes& P, const Planes& Q, Delta& d) {
    d.nAdd[0] = d.nAdd[1] = d.nSub[0] = d.nSub[1] = 0;
    auto push = [&](int p, int idx, bool added) {
        if (added) d.add[p][d.nAdd[p]++] = (int16_t)idx;
        else d.sub[p][d.nSub[p]++] = (int16_t)idx;
    };
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < N_PLAYER_PLANES; k++) {
            BB x = P.pl[c][k] ^ Q.pl[c][k];
            for (BB b = x; b; b &= b - 1) {
                int i = lsb(b);
                bool added = Q.pl[c][k] >> i & 1;
                int cell = CM.cellOf[i];
                for (int p = 0; p < 2; p++) push(p, (c == p ? F_MINE : F_OPP) + k * NC + cell, added);
            }
        }
    for (BB b = P.iso ^ Q.iso; b; b &= b - 1) {
        int i = lsb(b);
        bool added = Q.iso >> i & 1;
        for (int p = 0; p < 2; p++) push(p, F_ISO + CM.cellOf[i], added);
    }
    for (int c = 0; c < 2; c++) {
        uint8_t x = P.avail[c] ^ Q.avail[c];
        for (int q = 0; q < NPIECE; q++)
            if (x >> q & 1)
                for (int p = 0; p < 2; p++) push(p, F_PIECES + (c == p ? 0 : NPIECE) + q, Q.avail[c] >> q & 1);
        if (P.len[c] != Q.len[c])
            for (int p = 0; p < 2; p++) {
                int base = F_LEN + (c == p ? 0 : LEN_CAP + 1);
                push(p, base + P.len[c], false);
                push(p, base + Q.len[c], true);
            }
        if (P.ub[c] != Q.ub[c])
            for (int p = 0; p < 2; p++) {
                int base = F_UB + (c == p ? 0 : UB_CAP + 1);
                push(p, base + P.ub[c], false);
                push(p, base + Q.ub[c], true);
            }
    }
}

struct alignas(32) Accumulator {
    int16_t v[2][HIDDEN];
};

inline bool readFile(const std::string& path, std::vector<unsigned char>& buf) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    return true;
}

// The bytes of base64 text (the compiled-in weights).
inline std::vector<unsigned char> decodeBase64(const char* text) {
    std::vector<unsigned char> buf;
    int acc = 0, bits = 0;
    for (const char* p = text; *p; p++) {
        const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char* d = std::strchr(digits, *p);
        if (!d || *p == '=') continue;
        acc = acc << 6 | (int)(d - digits);
        bits += 6;
        if (bits >= 8) { bits -= 8; buf.push_back((unsigned char)(acc >> bits & 0xFF)); }
    }
    return buf;
}

struct Net {
    std::vector<int16_t> w1;  // [N_INPUTS][HIDDEN]
    alignas(32) int16_t b1[HIDDEN];
    alignas(32) int16_t wo[BUCKETS][2 * HIDDEN];
    int32_t bo[BUCKETS];
    std::string name;

    // File: "SNN1", int32 N_INPUTS, HIDDEN, BUCKETS, then w1, b1, wo (int16) and bo (int32).
    bool loadBytes(const unsigned char* data, size_t size) {
        size_t need = 16 + ((size_t)N_INPUTS * HIDDEN + HIDDEN + BUCKETS * 2 * HIDDEN) * 2 + BUCKETS * 4;
        if (size != need || std::memcmp(data, "SNN1", 4)) return false;
        int32_t hdr[3];
        std::memcpy(hdr, data + 4, 12);
        if (hdr[0] != N_INPUTS || hdr[1] != HIDDEN || hdr[2] != BUCKETS) return false;
        const unsigned char* p = data + 16;
        w1.resize((size_t)N_INPUTS * HIDDEN);
        std::memcpy(w1.data(), p, w1.size() * 2); p += w1.size() * 2;
        std::memcpy(b1, p, sizeof b1); p += sizeof b1;
        std::memcpy(wo, p, sizeof wo); p += sizeof wo;
        std::memcpy(bo, p, sizeof bo);
        return true;
    }
    bool loadFile(const std::string& path) {
        std::vector<unsigned char> buf;
        name = path;
        return readFile(path, buf) && loadBytes(buf.data(), buf.size());
    }

    // The network compiled into the executable (base64 text in nn_net.hpp).
    bool loadEmbedded() {
#if NN_HAVE_DEFAULT_NET
        std::vector<unsigned char> buf = decodeBase64(NN_NET_B64);
        name = "default";
        return loadBytes(buf.data(), buf.size());
#else
        return false;
#endif
    }

    const int16_t* row(int i) const { return &w1[(size_t)i * HIDDEN]; }

    void refresh(const Planes& P, Accumulator& acc) const {
        if (!CM.ready) CM.init();
        for (int p = 0; p < 2; p++) {
            int16_t* a = acc.v[p];
            std::memcpy(a, b1, sizeof b1);
            forEachInput(P, p, [&](int i) {
                const int16_t* r = row(i);
                for (int j = 0; j < HIDDEN; j++) a[j] += r[j];
            });
        }
    }

    // dst = src + rows(add) - rows(sub), per perspective
    void apply(const Accumulator& src, const Delta& d, Accumulator& dst) const {
        for (int p = 0; p < 2; p++) {
#if defined(__AVX2__)
            constexpr int NR = HIDDEN / 16;
            __m256i r[NR];
            for (int k = 0; k < NR; k++) r[k] = _mm256_load_si256((const __m256i*)(src.v[p] + 16 * k));
            for (int i = 0; i < d.nAdd[p]; i++) {
                const int16_t* w = row(d.add[p][i]);
                for (int k = 0; k < NR; k++) r[k] = _mm256_add_epi16(r[k], _mm256_loadu_si256((const __m256i*)(w + 16 * k)));
            }
            for (int i = 0; i < d.nSub[p]; i++) {
                const int16_t* w = row(d.sub[p][i]);
                for (int k = 0; k < NR; k++) r[k] = _mm256_sub_epi16(r[k], _mm256_loadu_si256((const __m256i*)(w + 16 * k)));
            }
            for (int k = 0; k < NR; k++) _mm256_store_si256((__m256i*)(dst.v[p] + 16 * k), r[k]);
#else
            int16_t* a = dst.v[p];
            if (&dst != &src) std::memcpy(a, src.v[p], sizeof src.v[p]);
            for (int i = 0; i < d.nAdd[p]; i++) { const int16_t* w = row(d.add[p][i]); for (int j = 0; j < HIDDEN; j++) a[j] += w[j]; }
            for (int i = 0; i < d.nSub[p]; i++) { const int16_t* w = row(d.sub[p][i]); for (int j = 0; j < HIDDEN; j++) a[j] -= w[j]; }
#endif
        }
    }

    static int64_t screluDot(const int16_t* acc, const int16_t* w) {
#if defined(__AVX2__)
        const __m256i zero = _mm256_setzero_si256(), qa = _mm256_set1_epi16(QA);
        __m256i sum = _mm256_setzero_si256();
        for (int i = 0; i < HIDDEN; i += 16) {
            __m256i v = _mm256_load_si256((const __m256i*)(acc + i));
            v = _mm256_min_epi16(_mm256_max_epi16(v, zero), qa);
            __m256i wv = _mm256_load_si256((const __m256i*)(w + i));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_mullo_epi16(v, wv), v));  // |v * w| <= 255 * 127
        }
        __m128i s = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
        return _mm_cvtsi128_si32(s);
#else
        int64_t sum = 0;
        for (int i = 0; i < HIDDEN; i++) {
            int v = acc[i] < 0 ? 0 : acc[i] > QA ? QA : acc[i];
            sum += int16_t(v * w[i]) * v;
        }
        return sum;
#endif
    }

    // 100 * logit(P(side to move wins)) for side to move stm at ply `ply`
    int evaluate(const Accumulator& acc, int stm, int ply) const {
        int b = bucketOf(ply);
        int64_t sum = screluDot(acc.v[stm], wo[b]) + screluDot(acc.v[stm ^ 1], wo[b] + HIDDEN);
        sum = sum / QA + bo[b];
        int64_t v = sum * 100 / (QA * QB);
        return (int)std::max<int64_t>(-3000, std::min<int64_t>(3000, v));
    }

    int evaluate(const Pos& s) const {
        Planes P;
        planesOf(s, P);
        Accumulator acc;
        refresh(P, acc);
        return evaluate(acc, s.stm, s.ply);
    }
};

// Networks by file name, each loaded once ("" = the network compiled into the executable).
inline const Net* loadNet(const std::string& path) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<Net>> nets;
    std::lock_guard<std::mutex> lk(mu);
    if (!CM.ready) CM.init();  // before any thread evaluates
    auto& n = nets[path];
    if (!n) {
        n = std::make_unique<Net>();
        if (!(path.empty() ? n->loadEmbedded() : n->loadFile(path))) {
            fprintf(stderr, "cannot load network %s\n", path.empty() ? "(none compiled in)" : path.c_str());
            exit(1);
        }
    }
    return n.get();
}

// Move ranking (the policy): a logit per placement for the side to move, from the value network's
// accumulators of the position and from the planes the placement covers and touches:
//   logit(g) = SCReLU(acc[stm], acc[opp]) . W[g] + b[g]
//            + sum over the planes k of cover[piece][k] * |cells of g in k| + touch[piece][k] * |neighbours of g in k|
// with the planes of the side to move's perspective: its full, end, front and dead cells, the
// opponent's, and the isolated empty cells. Only the order of the logits of one position matters.
constexpr int POLICY_PLANES = 2 * N_PLAYER_PLANES + 1;
constexpr int QP = 64;  // W is int8: the float weights times QP; b, cover and touch are scaled by QA * QP

struct Policy {
    int ngid = 0;
    std::vector<int8_t> w;   // [ngid][2 * HIDDEN]
    std::vector<int32_t> b;  // [ngid]
    int32_t cover[NPIECE][POLICY_PLANES], touch[NPIECE][POLICY_PLANES];
    std::string name;

    // File: "SNP1", int32 placements, HIDDEN, POLICY_PLANES, then w (int8), b, cover and touch (int32).
    bool loadBytes(const unsigned char* data, size_t size) {
        if (size < 16 || std::memcmp(data, "SNP1", 4)) return false;
        int32_t hdr[3];
        std::memcpy(hdr, data + 4, 12);
        if (hdr[0] != T.NGID || hdr[1] != HIDDEN || hdr[2] != POLICY_PLANES) return false;
        ngid = hdr[0];
        size_t nw = (size_t)ngid * 2 * HIDDEN;
        if (size != 16 + nw + ngid * 4 + sizeof cover + sizeof touch) return false;
        const unsigned char* p = data + 16;
        w.resize(nw);
        std::memcpy(w.data(), p, nw); p += nw;
        b.resize(ngid);
        std::memcpy(b.data(), p, ngid * 4); p += ngid * 4;
        std::memcpy(cover, p, sizeof cover); p += sizeof cover;
        std::memcpy(touch, p, sizeof touch);
        return true;
    }

    // SCReLU(acc) . W[g] for v = the clipped accumulators of both perspectives (2 * HIDDEN int16):
    // sum of v * v * w, as Net::screluDot (|v * w| <= 255 * 127 fits int16; the sum fits int32).
    int32_t dot(const int16_t* v, int gid) const {
        const int8_t* wg = &w[(size_t)gid * 2 * HIDDEN];
#if defined(__AVX2__)
        __m256i sum = _mm256_setzero_si256();
        for (int i = 0; i < 2 * HIDDEN; i += 16) {
            __m256i vv = _mm256_load_si256((const __m256i*)(v + i));
            __m256i wv = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)(wg + i)));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_mullo_epi16(vv, wv), vv));
        }
        __m128i s = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
        return _mm_cvtsi128_si32(s);
#else
        int32_t sum = 0;
        for (int i = 0; i < 2 * HIDDEN; i++) sum += int16_t(v[i] * wg[i]) * v[i];
        return sum;
#endif
    }

    // The logits of the moves mv[0..n) (no passes) of the position with accumulator acc and planes P, side to move stm.
    void logits(const Accumulator& acc, const Planes& P, int stm, const Move* mv, int n, int32_t* out) const {
        alignas(32) int16_t v[2 * HIDDEN];
        for (int j = 0; j < HIDDEN; j++) {
            v[j] = (int16_t)std::clamp<int>(acc.v[stm][j], 0, QA);
            v[HIDDEN + j] = (int16_t)std::clamp<int>(acc.v[stm ^ 1][j], 0, QA);
        }
        BB plane[POLICY_PLANES];
        for (int k = 0; k < N_PLAYER_PLANES; k++) plane[k] = P.pl[stm][k], plane[N_PLAYER_PLANES + k] = P.pl[stm ^ 1][k];
        plane[2 * N_PLAYER_PLANES] = P.iso;
        constexpr int AHEAD = 4;  // the weights of a move a few moves ahead are fetched while this one is scored
        auto prefetch = [&](int i) {
            const char* p = (const char*)&w[(size_t)mv[i].gid * 2 * HIDDEN];
            for (int k = 0; k < 2 * HIDDEN; k += 64) __builtin_prefetch(p + k);
        };
        for (int i = 0; i < std::min(n, AHEAD); i++) prefetch(i);
        for (int i = 0; i < n; i++) {
            if (i + AHEAD < n) prefetch(i + AHEAD);
            const Placement& pl = mv[i].pl();
            int32_t x = dot(v, mv[i].gid) / QA + b[mv[i].gid];
            for (int k = 0; k < POLICY_PLANES; k++)
                x += cover[pl.piece][k] * popc(pl.m & plane[k]) + touch[pl.piece][k] * popc(pl.nb & plane[k]);
            out[i] = x;
        }
    }

    bool loadFile(const std::string& path) {
        std::vector<unsigned char> buf;
        name = path;
        return readFile(path, buf) && loadBytes(buf.data(), buf.size());
    }

    // The policy compiled into the executable (base64 text in nn_policy.hpp).
    bool loadEmbedded() {
#if NN_HAVE_DEFAULT_POLICY
        std::vector<unsigned char> buf = decodeBase64(NN_POLICY_B64);
        name = "default";
        return loadBytes(buf.data(), buf.size());
#else
        return false;
#endif
    }
};

// Policies by file name, each loaded once ("" = the policy compiled into the executable).
inline const Policy* loadPolicy(const std::string& path) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<Policy>> policies;
    std::lock_guard<std::mutex> lk(mu);
    if (!CM.ready) CM.init();
    auto& p = policies[path];
    if (!p) {
        p = std::make_unique<Policy>();
        if (!(path.empty() ? p->loadEmbedded() : p->loadFile(path))) {
            fprintf(stderr, "cannot load policy %s\n", path.empty() ? "(none compiled in)" : path.c_str());
            exit(1);
        }
    }
    return p.get();
}

// The policy that goes with the network netFile ("" = the network compiled in): for the compiled-in
// network its compiled-in policy, or the file NN_POLICY names; none for a network loaded from a file,
// since a policy reads the first layer it was trained on.
inline const Policy* policyFor(const std::string& netFile) {
    const char* policyFile = getenv("NN_POLICY");
    if (!netFile.empty() || !(policyFile || HAVE_DEFAULT_POLICY)) return nullptr;
    return loadPolicy(policyFile ? policyFile : "");
}

// For the trainer: the image of every input under each of the 12 board symmetries.
inline void inputSymmetries(std::vector<int16_t>& out) {
    if (!CM.ready) CM.init();
    out.assign((size_t)NSYM * N_INPUTS, 0);
    for (int t = 0; t < NSYM; t++)
        for (int i = 0; i < N_INPUTS; i++) {
            int j = i;
            if (i < F_PIECES) {
                int plane = i / NC, cell = i % NC;
                j = plane * NC + CM.cellOf[T.perm[t][T.cells[cell]]];
            }
            out[(size_t)t * N_INPUTS + i] = (int16_t)j;
        }
}

}  // namespace nn
