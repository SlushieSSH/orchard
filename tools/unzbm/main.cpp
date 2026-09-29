#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "loader/dyld_cache.h"

using namespace orchard;

namespace
{
std::vector<uint8_t> read_all(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

}

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: orchard-unzbm <dyld_shared_cache_arm64> <decoder hex> <input> <output>\n");
        return 2;
    }
    try
    {
        Runtime rt;
        DyldCache cache(rt.mem, argv[1]);
        GuestAddr decoder = std::stoull(argv[2], nullptr, 16);
        std::string in = argv[3];
        auto header = read_all(in + ":com.apple.decmpfs");
        auto fork = read_all(in + ":com.apple.ResourceFork");
        uint32_t type;
        uint64_t size;
        std::memcpy(&type, header.data() + 4, 4);
        std::memcpy(&size, header.data() + 8, 8);
        if (std::memcmp(header.data(), "fpmc", 4) != 0 || type != 14) throw std::runtime_error("not an LZBITMAP resource-fork file");

        uint64_t chunks = (size + 0xffff) / 0x10000;
        Cpu cpu(rt, 0, 64);
        rt.threads->attach(cpu, 4 << 20, "unzbm");
        GuestAddr src = rt.heap.alloc(0x20000), dst = rt.heap.alloc(0x20000), scratch = rt.heap.alloc(1 << 20);

        std::ofstream out(argv[4], std::ios::binary);
        for (uint64_t i = 0; i < chunks; ++i)
        {
            uint32_t begin, end;
            std::memcpy(&begin, fork.data() + i * 4, 4);
            std::memcpy(&end, fork.data() + (i + 1) * 4, 4);
            uint64_t want = std::min<uint64_t>(0x10000, size - i * 0x10000);
            const uint8_t* chunk = fork.data() + begin;
            uint64_t len = end - begin;
            if (chunk[0] == 0xff)
            {
                out.write(reinterpret_cast<const char*>(chunk + 1), std::streamsize(len - 1));
                continue;
            }
            rt.mem.write_bytes(src, chunk, len);
            uint64_t got = cpu.call(decoder, {dst, want, src, len, scratch});
            if (cpu.stopped()) throw std::runtime_error("decoder stopped: " + rt.failure().reason);
            if (got != want)
            {
                std::fprintf(stderr, "chunk %llu: decoder returned %llu, expected %llu\n", (unsigned long long)i, (unsigned long long)got,
                             (unsigned long long)want);
                return 1;
            }
            std::vector<uint8_t> buf(want);
            rt.mem.read_bytes(dst, buf.data(), want);
            if (i == 0) std::printf("first bytes: %.16s\n", reinterpret_cast<const char*>(buf.data()));
            out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(want));
            if (i % 500 == 0) std::printf("chunk %llu/%llu\n", (unsigned long long)i, (unsigned long long)chunks);
        }
        std::printf("wrote %llu bytes\n", (unsigned long long)size);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
