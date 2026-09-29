#include "core/memory.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace orchard
{
namespace
{
std::mutex g_instances_lock;
std::vector<Memory*> g_instances;

LONG CALLBACK on_access_violation(PEXCEPTION_POINTERS info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = uintptr_t(info->ExceptionRecord->ExceptionInformation[1]);
    std::vector<Memory*> instances;
    {
        std::lock_guard g(g_instances_lock);
        instances = g_instances;
    }
    for (Memory* m : instances)
        if (m->handle_fault(addr)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

}

Memory::Memory()
{
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, layout::kSize, MEM_RESERVE, PAGE_NOACCESS));
    if (!base_) throw std::runtime_error("could not reserve the guest address space");
    lazy_filled_.assign((layout::kSize / layout::kPageSize + 63) / 64, 0);
    static PVOID handler = AddVectoredExceptionHandler(1, on_access_violation);
    (void)handler;
    std::lock_guard g(g_instances_lock);
    g_instances.push_back(this);
}

Memory::~Memory()
{
    {
        std::lock_guard g(g_instances_lock);
        std::erase(g_instances, this);
    }
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}

void Memory::map_lazy(GuestAddr addr, uint64_t size, std::string name, PageFiller fill)
{
    std::lock_guard g(lock_);
    GuestAddr start = page_align_down(addr);
    uint64_t len = page_align_up(addr + size) - start;
    check(start, len);
    lazy_[start] = {start, len, std::move(fill)};
    regions_[start] = {len, std::move(name)};
}

bool Memory::handle_fault(uintptr_t host_addr)
{
    if (host_addr < uintptr_t(base_) || host_addr >= uintptr_t(base_) + layout::kSize) return false;
    GuestAddr addr = host_addr - uintptr_t(base_);
    GuestAddr page = page_align_down(addr);
    const LazyRegion* region = nullptr;
    {
        std::lock_guard g(lock_);
        auto it = lazy_.upper_bound(addr);
        if (it == lazy_.begin()) return false;
        --it;
        if (addr >= it->second.start + it->second.size) return false;
        region = &it->second;
    }
    std::lock_guard g(fault_lock_);
    uint64_t index = page / layout::kPageSize;
    uint64_t& word = lazy_filled_[index / 64];
    uint64_t bit = uint64_t(1) << (index % 64);
    if (word & bit) return true;
    if (!VirtualAlloc(base_ + page, layout::kPageSize, MEM_COMMIT, PAGE_READWRITE)) return false;
    region->fill(page, base_ + page, layout::kPageSize);
    word |= bit;
    return true;
}

void Memory::map(GuestAddr addr, uint64_t size, std::string name)
{
    std::lock_guard g(lock_);
    GuestAddr start = page_align_down(addr);
    uint64_t len = page_align_up(addr + size) - start;
    check(start, len);
    claim_free_range(start, start + len);
    if (!VirtualAlloc(base_ + start, len, MEM_COMMIT, PAGE_READWRITE))
        throw std::runtime_error("could not commit guest memory for " + name);
    regions_[start] = {len, std::move(name)};
}

void Memory::unmap(GuestAddr addr, uint64_t size)
{
    std::lock_guard g(lock_);
    GuestAddr start = page_align_down(addr);
    GuestAddr end = page_align_up(addr + size);
    if (end <= start || end > layout::kSize) return;
    auto it = regions_.upper_bound(start);
    if (it != regions_.begin()) --it;
    while (it != regions_.end() && it->first < end)
    {
        GuestAddr r_start = it->first, r_end = it->first + it->second.size;
        if (r_end <= start || lazy_.count(r_start))
        {
            ++it;
            continue;
        }
        Region r = it->second;
        it = regions_.erase(it);
        if (r_start < start) regions_[r_start] = {start - r_start, r.name};
        if (r_end > end) regions_[end] = {r_end - end, r.name};
        GuestAddr cut_start = std::max(r_start, start), cut_end = std::min(r_end, end);
        VirtualFree(base_ + cut_start, cut_end - cut_start, MEM_DECOMMIT);
        if (cut_start >= layout::kRegionsBase && cut_end <= layout::kCommPage) add_free_range(cut_start, cut_end - cut_start);
        it = regions_.lower_bound(cut_end);
    }
}

void Memory::add_free_range(GuestAddr addr, uint64_t size)
{
    auto erase_size = [&](GuestAddr a, uint64_t sz) {
        auto [lo, hi] = free_by_size_.equal_range(sz);
        for (auto i = lo; i != hi; ++i)
            if (i->second == a)
            {
                free_by_size_.erase(i);
                return;
            }
    };
    auto next = free_by_addr_.lower_bound(addr);
    if (next != free_by_addr_.end() && next->first == addr + size)
    {
        size += next->second;
        erase_size(next->first, next->second);
        next = free_by_addr_.erase(next);
    }
    if (next != free_by_addr_.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr)
        {
            addr = prev->first;
            size += prev->second;
            erase_size(prev->first, prev->second);
            free_by_addr_.erase(prev);
        }
    }
    free_by_addr_[addr] = size;
    free_by_size_.emplace(size, addr);
}

void Memory::claim_free_range(GuestAddr start, GuestAddr end)
{
    auto it = free_by_addr_.upper_bound(start);
    if (it != free_by_addr_.begin()) --it;
    while (it != free_by_addr_.end() && it->first < end)
    {
        GuestAddr f_start = it->first, f_end = it->first + it->second;
        if (f_end <= start)
        {
            ++it;
            continue;
        }
        auto [lo, hi] = free_by_size_.equal_range(it->second);
        for (auto i = lo; i != hi; ++i)
            if (i->second == f_start)
            {
                free_by_size_.erase(i);
                break;
            }
        free_by_addr_.erase(it);
        if (f_start < start) add_free_range(f_start, start - f_start);
        if (f_end > end) add_free_range(end, f_end - end);
        it = free_by_addr_.lower_bound(end);
    }
}

GuestAddr Memory::take_free_range(uint64_t size)
{
    auto it = free_by_size_.lower_bound(size);
    if (it == free_by_size_.end()) return 0;
    uint64_t have = it->first;
    GuestAddr addr = it->second;
    free_by_size_.erase(it);
    free_by_addr_.erase(addr);
    if (have > size) add_free_range(addr + size, have - size);
    return addr;
}

void Memory::discard(GuestAddr addr, uint64_t size)
{
    GuestAddr start = page_align_up(addr), end = page_align_down(addr + size);
    if (end > start) VirtualAlloc(base_ + start, end - start, MEM_RESET, PAGE_READWRITE);
}

bool Memory::is_mapped(GuestAddr addr, uint64_t size) const
{
    std::lock_guard g(lock_);
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return false;
    --it;
    return addr + size <= it->first + it->second.size;
}

GuestAddr Memory::map_anywhere(uint64_t size, std::string name)
{
    std::lock_guard g(lock_);
    uint64_t len = page_align_up(size);
    if (GuestAddr reused = take_free_range(len))
    {
        map(reused, len, std::move(name));
        return reused;
    }
    GuestAddr addr = next_region_;
    next_region_ += len + layout::kPageSize;
    if (next_region_ >= layout::kCommPage) throw std::runtime_error("guest address space exhausted");
    map(addr, len, std::move(name));
    return addr;
}

std::string Memory::describe(GuestAddr addr) const
{
    std::lock_guard g(lock_);
    auto it = regions_.upper_bound(addr);
    if (it != regions_.begin())
    {
        --it;
        if (addr < it->first + it->second.size)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(addr - it->first));
            return it->second.name + buf;
        }
    }
    return "unmapped";
}

std::string Memory::read_cstr(GuestAddr addr, size_t max) const
{
    std::string s;
    for (size_t i = 0; i < max; ++i)
    {
        char c = read<char>(addr + i);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

std::map<std::string, uint64_t> Memory::committed_by_name() const
{
    std::map<std::string, uint64_t> out;
    std::lock_guard g(lock_);
    for (auto& [start, r] : regions_)
    {
        if (lazy_.count(start)) continue;
        out[r.name.substr(0, r.name.find(':'))] += r.size;
    }
    uint64_t filled = 0;
    for (uint64_t w : lazy_filled_)
        filled += std::popcount(w);
    out["dyld cache (paged in)"] = filled * layout::kPageSize;
    return out;
}

GuestAddr Memory::alloc_system(uint64_t size, uint64_t align)
{
    std::lock_guard g(lock_);
    GuestAddr addr = (system_next_ + align - 1) & ~(align - 1);
    GuestAddr end = addr + size;
    if (end > layout::kRegionsBase) throw std::runtime_error("system data area exhausted");
    if (end > system_committed_)
    {
        GuestAddr grow_to = page_align_up(end + 0x100000);
        map(system_committed_, grow_to - system_committed_, "system-data");
        system_committed_ = grow_to;
    }
    system_next_ = end;
    return addr;
}

GuestAddr Memory::alloc_cstr_region(std::string_view s)
{
    GuestAddr addr = alloc_system(s.size() + 1, 1);
    write_bytes(addr, s.data(), s.size());
    write<char>(addr + s.size(), 0);
    return addr;
}

}
