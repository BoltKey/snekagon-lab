// Transposition table of the exact solver: remembers what is known about positions already
// searched, so transpositions (the same position reached by different move orders) and
// re-searches cost one lookup.
//
// Lockless and shareable between threads. Each slot stores the pair (key ^ data, data); an entry
// torn by two threads writing at once fails the key check instead of returning garbage.
// Buckets have two slots: slot 0 keeps the entry with the most work behind it, slot 1 is
// always replaced.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>

struct TTEntry {
    int lower, upper;  // bounds of the exact value (-1 loss, 0 draw, +1 win for the side to move)
    int bestMove;      // placement id in the canonical orientation, -1 = none
};

class TranspositionTable {
  public:
    explicit TranspositionTable(int log2Buckets)
        : slots(new Slot[size_t(2) << log2Buckets]), buckets(size_t(1) << log2Buckets), mask(buckets - 1) {}

    void clear() {
        for (size_t i = 0; i < 2 * buckets; i++) {
            slots[i].check.store(0, std::memory_order_relaxed);
            slots[i].data.store(0, std::memory_order_relaxed);
        }
    }

    bool probe(uint64_t key, TTEntry& entry) const {
        size_t bucket = (key & mask) * 2;
        for (int i = 0; i < 2; i++) {
            uint64_t check = slots[bucket + i].check.load(std::memory_order_relaxed);
            uint64_t data = slots[bucket + i].data.load(std::memory_order_relaxed);
            if (data && (check ^ data) == key) {
                entry = unpack(data);
                return true;
            }
        }
        return false;
    }

    // workLog2: log2 of the number of nodes the result cost (decides what stays in slot 0)
    void store(uint64_t key, int lower, int upper, int bestMove, int workLog2) {
        size_t bucket = (key & mask) * 2;
        uint64_t data = pack(lower, upper, bestMove, workLog2);
        uint64_t check0 = slots[bucket].check.load(std::memory_order_relaxed);
        uint64_t data0 = slots[bucket].data.load(std::memory_order_relaxed);
        bool samePosition = (check0 ^ data0) == key;
        int slot = (samePosition || workLog2 >= workOf(data0)) ? 0 : 1;
        slots[bucket + slot].data.store(data, std::memory_order_relaxed);
        slots[bucket + slot].check.store(key ^ data, std::memory_order_relaxed);
    }

  private:
    struct Slot {
        std::atomic<uint64_t> check{0}, data{0};
    };
    std::unique_ptr<Slot[]> slots;
    size_t buckets;
    uint64_t mask;

    // data layout: bits 0-2 lower + 2, bits 3-5 upper + 2, bits 6-21 bestMove + 1, bits 22+ work.
    // (lower + 2 >= 1, so a used slot never has data == 0.)
    static uint64_t pack(int lower, int upper, int bestMove, int workLog2) {
        return uint64_t(lower + 2) | uint64_t(upper + 2) << 3 | uint64_t(bestMove + 1) << 6 | uint64_t(workLog2) << 22;
    }
    static TTEntry unpack(uint64_t data) {
        return {int(data & 7) - 2, int(data >> 3 & 7) - 2, int(data >> 6 & 0xFFFF) - 1};
    }
    static int workOf(uint64_t data) { return int(data >> 22 & 63); }
};

// Integer setting from an environment variable (for experiments without recompiling).
inline int envInt(const char* name, int defaultValue) {
    const char* value = getenv(name);
    return value ? atoi(value) : defaultValue;
}
