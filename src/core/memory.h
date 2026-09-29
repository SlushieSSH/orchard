#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace orchard
{
using GuestAddr = uint64_t;

namespace layout
{
constexpr uint64_t kAddressBits = 36;
constexpr uint64_t kSize = uint64_t(1) << kAddressBits;
constexpr uint64_t kPageSize = 0x4000;
constexpr GuestAddr kImagesBase = 0x1'0000'0000;
constexpr GuestAddr kStubsBase = 0x7'0000'0000;
constexpr GuestAddr kStubsEnd = 0x7'4000'0000;
constexpr GuestAddr kSystemDataBase = 0x7'8000'0000;
constexpr GuestAddr kRegionsBase = 0x8'0000'0000;
constexpr GuestAddr kCommPage = 0xF'FFFF'C000;
}

constexpr uint64_t page_align_up(uint64_t v)
{
    return (v + layout::kPageSize - 1) & ~(layout::kPageSize - 1);
}
constexpr uint64_t page_align_down(uint64_t v)
{
    return v & ~(layout::kPageSize - 1);
}

class GuestFault : public std::exception
{
public:
    explicit GuestFault(GuestAddr addr) : addr_(addr) {}
    GuestAddr addr() const { return addr_; }
    const char* what() const noexcept override { return "guest memory fault"; }

private:
    GuestAddr addr_;
};

class Memory
{
public:
    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    uint8_t* base() const { return base_; }

    void map(GuestAddr addr, uint64_t size, std::string name);
    void unmap(GuestAddr addr, uint64_t size);
    void discard(GuestAddr addr, uint64_t size);
    bool is_mapped(GuestAddr addr, uint64_t size = 1) const;

    using PageFiller = std::function<void(GuestAddr page, uint8_t* host, uint64_t size)>;
    void map_lazy(GuestAddr addr, uint64_t size, std::string name, PageFiller fill);

    bool handle_fault(uintptr_t host_addr);

    GuestAddr map_anywhere(uint64_t size, std::string name);

    std::string describe(GuestAddr addr) const;

    uint8_t* host(GuestAddr addr) const { return base_ + addr; }

    template <typename T> T* ptr(GuestAddr addr) const
    {
        check(addr, sizeof(T));
        return reinterpret_cast<T*>(base_ + addr);
    }

    template <typename T> T read(GuestAddr addr) const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        check(addr, sizeof(T));
        T v;
        std::memcpy(&v, base_ + addr, sizeof(T));
        return v;
    }

    template <typename T> void write(GuestAddr addr, const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        check(addr, sizeof(T));
        std::memcpy(base_ + addr, &v, sizeof(T));
    }

    void read_bytes(GuestAddr addr, void* dst, uint64_t size) const
    {
        check(addr, size);
        std::memcpy(dst, base_ + addr, size);
    }
    void write_bytes(GuestAddr addr, const void* src, uint64_t size)
    {
        check(addr, size);
        std::memcpy(base_ + addr, src, size);
    }

    std::string read_cstr(GuestAddr addr, size_t max = 1 << 20) const;
    GuestAddr alloc_cstr_region(std::string_view s);

    GuestAddr alloc_system(uint64_t size, uint64_t align = 16);

    std::map<std::string, uint64_t> committed_by_name() const;

private:
    void check(GuestAddr addr, uint64_t size) const
    {
        if (addr >= layout::kSize || size > layout::kSize - addr) throw GuestFault(addr);
    }

    struct Region
    {
        uint64_t size;
        std::string name;
    };
    struct LazyRegion
    {
        GuestAddr start;
        uint64_t size;
        PageFiller fill;
    };

    uint8_t* base_ = nullptr;
    std::map<GuestAddr, Region> regions_;
    GuestAddr next_region_ = layout::kRegionsBase;
    GuestAddr system_next_ = layout::kSystemDataBase;
    GuestAddr system_committed_ = layout::kSystemDataBase;
    mutable std::recursive_mutex lock_;
    void add_free_range(GuestAddr addr, uint64_t size);
    GuestAddr take_free_range(uint64_t size);
    void claim_free_range(GuestAddr start, GuestAddr end);

    std::map<GuestAddr, LazyRegion> lazy_;
    std::map<GuestAddr, uint64_t> free_by_addr_;
    std::multimap<uint64_t, GuestAddr> free_by_size_;
    std::vector<uint64_t> lazy_filled_;
    std::mutex fault_lock_;
};

}
