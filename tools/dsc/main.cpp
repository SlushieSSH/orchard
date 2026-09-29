#include <cstdio>
#include <exception>
#include <string>

#include "core/runtime.h"
#include "loader/cache_images.h"
#include "loader/dyld_cache.h"
#include "loader/macho.h"

using namespace orchard;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: orchard-dsc <dyld_shared_cache_arm64> [image substring]\n");
        return 2;
    }
    try
    {
        Runtime rt;
        DyldCache cache(rt.mem, argv[1]);
        std::printf("cache 0x%llx-0x%llx, %zu images, slide info v%d\n", (unsigned long long)cache.start(), (unsigned long long)cache.end(),
                    cache.images().size(), cache.slide_version());
        if (argc < 3) return 0;
        std::string query = argv[2];
        if (query.starts_with("0x"))
        {
            GuestAddr addr = std::stoull(query, nullptr, 16);
            CacheImages images(rt, cache);
            for (auto& img : cache.images())
            {
                auto* d = images.dylib(img.path);
                for (auto& seg : d->macho.segments)
                {
                    if (seg.name == "__LINKEDIT" || addr < seg.vmaddr || addr >= seg.vmaddr + seg.vmsize) continue;
                    std::printf("%s %s+0x%llx\n", img.path.c_str(), seg.name.c_str(), (unsigned long long)(addr - seg.vmaddr));
                    images.lookup(img.path, "_");
                    std::string best;
                    GuestAddr best_addr = 0;
                    for (auto& [name, e] : d->exports)
                    {
                        GuestAddr a = d->header + e.address;
                        if (!(e.flags & macho::kExportReexport) && a <= addr && a > best_addr) best_addr = a, best = name;
                    }
                    std::printf("  nearest export: %s+0x%llx\n", best.c_str(), (unsigned long long)(addr - best_addr));
                    return 0;
                }
            }
            std::printf("not in any image\n");
            return 0;
        }
        for (auto& img : cache.images())
        {
            if (img.path.find(argv[2]) == std::string::npos) continue;
            std::printf("\n%s @ 0x%llx\n", img.path.c_str(), (unsigned long long)img.header);
            auto span = std::span<const uint8_t>(rt.mem.host(img.header), 0x10000);
            macho::Image m = macho::parse(span, false);
            for (auto& seg : m.segments)
            {
                std::printf("  %-16s 0x%llx +0x%llx\n", seg.name.c_str(), (unsigned long long)seg.vmaddr, (unsigned long long)seg.vmsize);
                for (auto& sec : seg.sections)
                    std::printf("      %-18s 0x%llx +0x%llx\n", sec.sectname.c_str(), (unsigned long long)sec.addr,
                                (unsigned long long)sec.size);
            }
            for (auto& d : m.dylibs)
                std::printf("  links %s\n", d.path.c_str());
            std::printf("  exports trie at file offset 0x%x (+0x%x)\n", m.exports.offset, m.exports.size);
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
