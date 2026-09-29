#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/memory.h"

namespace orchard
{
class Heap
{
public:
    explicit Heap(Memory& mem) : mem_(mem) {}

    GuestAddr alloc(uint64_t size, uint64_t align = 16);
    GuestAddr calloc(uint64_t size);
    GuestAddr realloc(GuestAddr p, uint64_t size);
    void free(GuestAddr p);
    uint64_t size_of(GuestAddr p) const;

    struct Stats
    {
        uint64_t live_small = 0, live_large = 0, free_small = 0, free_large = 0, allocations = 0;
    };
    Stats stats() const;

private:
    static constexpr uint64_t kLargeThreshold = 256 * 1024;
    static constexpr uint64_t kArenaSize = 64ull << 20;

    static int size_class(uint64_t size);
    static uint64_t class_size(int cls);

    GuestAddr carve(uint64_t size, uint64_t align);

    Memory& mem_;
    mutable std::mutex lock_;
    struct Block
    {
        uint64_t size;
        bool is_small;
    };
    std::unordered_map<GuestAddr, Block> live_;
    std::vector<std::vector<GuestAddr>> free_small_{72};
    std::multimap<uint64_t, GuestAddr> free_large_;
    GuestAddr arena_next_ = 0;
    GuestAddr arena_end_ = 0;
};

}
