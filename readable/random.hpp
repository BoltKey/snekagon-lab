// Small, fast, reproducible random numbers (splitmix64).
#pragma once
#include <cstdint>

inline uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ULL + 1) {}

    uint64_t next() { return splitmix64(state); }
    int below(int n) { return (int)((next() >> 33) % (uint64_t)n); }      // 0 .. n-1
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
};
