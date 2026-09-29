#include <cstring>
#include <random>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "loader/linker.h"

namespace orchard
{
namespace
{
char* hp(Cpu& c, int i)
{
    return reinterpret_cast<char*>(c.mem.host(c.arg(i)));
}

GuestAddr alloc_data(Runtime& rt, uint64_t size)
{
    GuestAddr a = rt.mem.alloc_system(size, 16);
    std::memset(rt.mem.host(a), 0, size);
    return a;
}

void register_memory(Hle& h)
{
    h.fn("_malloc", [](Cpu& c) { c.ret(c.rt.heap.alloc(c.arg(0))); });
    h.fn("_calloc", [](Cpu& c) { c.ret(c.rt.heap.calloc(c.arg(0) * c.arg(1))); });
    h.fn("_realloc", [](Cpu& c) { c.ret(c.rt.heap.realloc(c.arg(0), c.arg(1))); });
    h.fn("_free", [](Cpu& c) { c.rt.heap.free(c.arg(0)); });
    h.fn("_malloc_size", [](Cpu& c) { c.ret(c.rt.heap.size_of(c.arg(0))); });
    h.fn("_malloc_good_size", [](Cpu& c) { c.ret((c.arg(0) + 15) & ~uint64_t(15)); });
    h.fn("_valloc", [](Cpu& c) { c.ret(c.rt.heap.alloc(c.arg(0), layout::kPageSize)); });
    h.fn("_aligned_alloc", [](Cpu& c) { c.ret(c.rt.heap.alloc(c.arg(1), c.arg(0))); });
    h.fn("_posix_memalign", [](Cpu& c) {
        GuestAddr p = c.rt.heap.alloc(c.arg(2), c.arg(1));
        c.mem.write<uint64_t>(c.arg(0), p);
        c.ret(0);
    });
    h.fn("_malloc_zone_malloc", [](Cpu& c) { c.ret(c.rt.heap.alloc(c.arg(1))); });
    h.fn("_malloc_zone_calloc", [](Cpu& c) { c.ret(c.rt.heap.calloc(c.arg(1) * c.arg(2))); });
    h.fn("_malloc_zone_realloc", [](Cpu& c) { c.ret(c.rt.heap.realloc(c.arg(1), c.arg(2))); });
    h.fn("_malloc_zone_free", [](Cpu& c) { c.rt.heap.free(c.arg(1)); });
    h.fn("_malloc_default_zone", [](Cpu& c) { c.ret(0x5a4f4e45); });

    h.fn("_memcpy", [](Cpu& c) { std::memcpy(hp(c, 0), hp(c, 1), c.arg(2)); });
    h.fn("_memmove", [](Cpu& c) { std::memmove(hp(c, 0), hp(c, 1), c.arg(2)); });
    h.fn("_memset", [](Cpu& c) { std::memset(hp(c, 0), int(c.arg(1)), c.arg(2)); });
    h.fn("_bzero", [](Cpu& c) { std::memset(hp(c, 0), 0, c.arg(1)); });
    h.fn("___bzero", [](Cpu& c) { std::memset(hp(c, 0), 0, c.arg(1)); });
    h.fn("_memset_pattern16", [](Cpu& c) {
        char* d = hp(c, 0);
        const char* p = hp(c, 1);
        for (uint64_t i = 0; i < c.arg(2); ++i)
            d[i] = p[i % 16];
    });
    h.fn("_memcmp", [](Cpu& c) { c.ret(uint64_t(int64_t(std::memcmp(hp(c, 0), hp(c, 1), c.arg(2))))); });
    h.fn("_bcmp", [](Cpu& c) { c.ret(std::memcmp(hp(c, 0), hp(c, 1), c.arg(2)) != 0); });
    h.fn("_memchr", [](Cpu& c) {
        auto* p = static_cast<const char*>(std::memchr(hp(c, 0), int(c.arg(1)), c.arg(2)));
        c.ret(p ? c.arg(0) + (p - hp(c, 0)) : 0);
    });
    h.fn("___memcpy_chk", [](Cpu& c) { std::memcpy(hp(c, 0), hp(c, 1), c.arg(2)); });
    h.fn("___memmove_chk", [](Cpu& c) { std::memmove(hp(c, 0), hp(c, 1), c.arg(2)); });
    h.fn("___memset_chk", [](Cpu& c) { std::memset(hp(c, 0), int(c.arg(1)), c.arg(2)); });
}

void register_strings(Hle& h)
{
    h.fn("_strlen", [](Cpu& c) { c.ret(std::strlen(hp(c, 0))); });
    h.fn("_strnlen", [](Cpu& c) { c.ret(strnlen(hp(c, 0), c.arg(1))); });
    h.fn("_strcmp", [](Cpu& c) { c.ret(uint64_t(int64_t(std::strcmp(hp(c, 0), hp(c, 1))))); });
    h.fn("_strncmp", [](Cpu& c) { c.ret(uint64_t(int64_t(std::strncmp(hp(c, 0), hp(c, 1), c.arg(2))))); });
    h.fn("_strcasecmp", [](Cpu& c) { c.ret(uint64_t(int64_t(_stricmp(hp(c, 0), hp(c, 1))))); });
    h.fn("_strncasecmp", [](Cpu& c) { c.ret(uint64_t(int64_t(_strnicmp(hp(c, 0), hp(c, 1), c.arg(2))))); });
    h.fn("_strcpy", [](Cpu& c) { std::strcpy(hp(c, 0), hp(c, 1)); });
    h.fn("_strncpy", [](Cpu& c) { std::strncpy(hp(c, 0), hp(c, 1), c.arg(2)); });
    h.fn("_strcat", [](Cpu& c) { std::strcat(hp(c, 0), hp(c, 1)); });
    h.fn("_strncat", [](Cpu& c) { std::strncat(hp(c, 0), hp(c, 1), c.arg(2)); });
    h.fn("_strlcpy", [](Cpu& c) {
        size_t n = std::strlen(hp(c, 1));
        if (c.arg(2))
        {
            size_t k = n < c.arg(2) - 1 ? n : c.arg(2) - 1;
            std::memcpy(hp(c, 0), hp(c, 1), k);
            hp(c, 0)[k] = 0;
        }
        c.ret(n);
    });
    h.fn("_strlcat", [](Cpu& c) {
        size_t size = c.arg(2);
        size_t dlen = strnlen(hp(c, 0), size);
        size_t slen = std::strlen(hp(c, 1));
        if (dlen < size)
        {
            size_t k = slen < size - dlen - 1 ? slen : size - dlen - 1;
            std::memcpy(hp(c, 0) + dlen, hp(c, 1), k);
            hp(c, 0)[dlen + k] = 0;
        }
        c.ret(dlen + slen);
    });
    h.fn("_strchr", [](Cpu& c) {
        const char* f = std::strchr(hp(c, 0), int(c.arg(1)));
        c.ret(f ? c.arg(0) + (f - hp(c, 0)) : 0);
    });
    h.fn("_strrchr", [](Cpu& c) {
        const char* f = std::strrchr(hp(c, 0), int(c.arg(1)));
        c.ret(f ? c.arg(0) + (f - hp(c, 0)) : 0);
    });
    h.fn("_strstr", [](Cpu& c) {
        const char* f = std::strstr(hp(c, 0), hp(c, 1));
        c.ret(f ? c.arg(0) + (f - hp(c, 0)) : 0);
    });
    h.fn("_strdup", [](Cpu& c) {
        size_t n = std::strlen(hp(c, 0)) + 1;
        GuestAddr p = c.rt.heap.alloc(n);
        std::memcpy(c.mem.host(p), hp(c, 0), n);
        c.ret(p);
    });
    h.fn("_strtol", [](Cpu& c) {
        char* end;
        long long v = std::strtoll(hp(c, 0), &end, int(c.arg(2)));
        if (c.arg(1)) c.mem.write<uint64_t>(c.arg(1), c.arg(0) + (end - hp(c, 0)));
        c.ret(uint64_t(v));
    });
    h.fn("_strtoul", [](Cpu& c) {
        char* end;
        unsigned long long v = std::strtoull(hp(c, 0), &end, int(c.arg(2)));
        if (c.arg(1)) c.mem.write<uint64_t>(c.arg(1), c.arg(0) + (end - hp(c, 0)));
        c.ret(v);
    });
    h.fn("_atoi", [](Cpu& c) { c.ret(uint64_t(int64_t(std::atoi(hp(c, 0))))); });
}

void register_process(Hle& h)
{
    h.data("___stack_chk_guard", [](Runtime& rt) {
        GuestAddr a = alloc_data(rt, 8);
        rt.mem.write<uint64_t>(a, std::random_device{}() | (uint64_t(std::random_device{}()) << 32));
        return a;
    });
    h.fn("___stack_chk_fail", [](Cpu& c) { c.stop("stack smashing detected (__stack_chk_fail) in " + c.rt.describe(c.lr())); });
    h.fn("_abort", [](Cpu& c) { c.stop("guest called abort() from " + c.rt.describe(c.lr())); });
    h.fn("_exit", [](Cpu& c) { c.stop("guest called exit(" + std::to_string(int(c.arg(0))) + ")"); });
    h.fn("___cxa_atexit", [](Cpu& c) { c.ret(0); });
    h.fn("_atexit", [](Cpu& c) { c.ret(0); });
    h.fn("__availability_version_check", [](Cpu& c) {
        constexpr uint32_t kRunning = (16u << 16) | (7u << 8) | 16u;
        for (uint64_t i = 0; i < c.arg(0); ++i)
        {
            uint32_t platform = c.mem.read<uint32_t>(c.arg(1) + i * 8), version = c.mem.read<uint32_t>(c.arg(1) + i * 8 + 4);
            if (platform == 2 && version > kRunning) return c.ret(0);
        }
        c.ret(1);
    });
    // these two must not touch any register (tlv: x0 only)
    h.fn("___chkstk_darwin", [](Cpu& c) {});
    h.fn("_thread_chkstk_darwin", [](Cpu& c) {});
    h.fn("__tlv_bootstrap", [](Cpu& c) { c.ret(c.rt.linker->tlv_address(c, c.arg(0))); });
}

}

void register_pthread(Hle& h);
void register_posix(Hle& h);
void register_dyld(Hle& h);
void register_mach(Hle& h);

void register_libsystem(Hle& h)
{
    register_pthread(h);
    register_posix(h);
    register_dyld(h);
    register_mach(h);
    register_dispatch(h);
    register_blocks(h);
    register_memory(h);
    register_strings(h);
    register_process(h);
}

}
