#include "core/heap.h"

#include <algorithm>
#include <bit>

namespace orchard
{
int Heap::size_class(uint64_t size)
{
    if (size <= 512) return int((std::max<uint64_t>(size, 1) + 15) / 16) - 1;
    int log = std::bit_width(size - 1) - 1;
    int sub = int(((size - 1) >> (log - 2)) & 3);
    return 32 + (log - 9) * 4 + sub;
}

uint64_t Heap::class_size(int cls)
{
    if (cls < 32) return uint64_t(cls + 1) * 16;
    int log = 9 + (cls - 32) / 4;
    int sub = (cls - 32) % 4;
    return (uint64_t(1) << log) + uint64_t(sub + 1) * (uint64_t(1) << (log - 2));
}

GuestAddr Heap::carve(uint64_t size, uint64_t align)
{
    GuestAddr p = (arena_next_ + align - 1) & ~(align - 1);
    if (!arena_next_ || p + size > arena_end_)
    {
        uint64_t len = std::max(kArenaSize, page_align_up(size + align));
        arena_next_ = mem_.map_anywhere(len, "heap");
        arena_end_ = arena_next_ + len;
        p = (arena_next_ + align - 1) & ~(align - 1);
    }
    arena_next_ = p + size;
    return p;
}

GuestAddr Heap::alloc(uint64_t size, uint64_t align)
{
    std::lock_guard g(lock_);
    align = std::max<uint64_t>(align, 16);
    if (size < kLargeThreshold && align == 16)
    {
        int cls = size_class(size);
        uint64_t csize = class_size(cls);
        auto& list = free_small_[cls];
        GuestAddr p;
        if (!list.empty())
        {
            p = list.back();
            list.pop_back();
        }
        else
        {
            p = carve(csize, 16);
        }
        live_[p] = {csize, true};
        return p;
    }

    uint64_t len = page_align_up(std::max(size, uint64_t(1)));
    if (align <= layout::kPageSize)
    {
        auto it = free_large_.lower_bound(len);
        if (it != free_large_.end() && it->first <= len * 2)
        {
            GuestAddr p = it->second;
            uint64_t have = it->first;
            free_large_.erase(it);
            live_[p] = {have, false};
            return p;
        }
        GuestAddr p = mem_.map_anywhere(len, "heap-large");
        live_[p] = {len, false};
        return p;
    }
    GuestAddr raw = mem_.map_anywhere(len + align, "heap-large");
    GuestAddr p = (raw + align - 1) & ~(align - 1);
    live_[p] = {len, false};
    return p;
}

GuestAddr Heap::calloc(uint64_t size)
{
    GuestAddr p = alloc(size);
    std::memset(mem_.host(p), 0, size);
    return p;
}

GuestAddr Heap::realloc(GuestAddr p, uint64_t size)
{
    if (!p) return alloc(size);
    uint64_t old = size_of(p);
    if (size <= old && size > old / 2) return p;
    GuestAddr n = alloc(size);
    std::memcpy(mem_.host(n), mem_.host(p), std::min(old, size));
    free(p);
    return n;
}

void Heap::free(GuestAddr p)
{
    if (!p) return;
    std::lock_guard g(lock_);
    auto it = live_.find(p);
    if (it == live_.end()) return;
    Block b = it->second;
    live_.erase(it);
    if (b.is_small)
        free_small_[size_class(b.size)].push_back(p);
    else
    {
        mem_.discard(p, b.size);
        free_large_.emplace(b.size, p);
    }
}

uint64_t Heap::size_of(GuestAddr p) const
{
    std::lock_guard g(lock_);
    auto it = live_.find(p);
    return it == live_.end() ? 0 : it->second.size;
}

Heap::Stats Heap::stats() const
{
    std::lock_guard g(lock_);
    Stats s;
    for (auto& [p, b] : live_)
        (b.is_small ? s.live_small : s.live_large) += b.size;
    s.allocations = live_.size();
    for (size_t cls = 0; cls < free_small_.size(); ++cls)
        s.free_small += free_small_[cls].size() * class_size(int(cls));
    for (auto& [size, p] : free_large_)
        s.free_large += size;
    return s;
}

}
