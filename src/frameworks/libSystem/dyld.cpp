#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "loader/cache_images.h"
#include "loader/dyld_cache.h"
#include "loader/linker.h"

namespace orchard
{
namespace
{
std::mutex names_lock;
std::unordered_map<const LoadedImage*, GuestAddr> names;

GuestAddr image_name(Cpu& c, const LoadedImage& img)
{
    std::lock_guard g(names_lock);
    GuestAddr& n = names[&img];
    if (!n)
    {
        std::string path = img.from_cache ? img.install_path : "";
        if (!img.from_cache)
        {
            std::string rel = img.path.generic_string();
            auto at = rel.find(".app/");
            path = c.rt.vfs.bundle_path + (at == std::string::npos ? "/" + img.name : rel.substr(at + 4));
        }
        n = c.mem.alloc_cstr_region(path);
    }
    return n;
}

const LoadedImage* image_at(Cpu& c, uint64_t index)
{
    auto& imgs = c.rt.linker->images();
    return index < imgs.size() ? imgs[index].get() : nullptr;
}

const macho::Section* section(const LoadedImage& img, std::string_view seg, std::string_view sec)
{
    for (auto& s : img.macho.segments)
        if (s.name == seg)
            for (auto& x : s.sections)
                if (x.sectname == sec) return &x;
    return nullptr;
}

std::optional<GuestAddr> symbol_address(Cpu& c, const std::string& mangled)
{
    if (c.rt.hle.claims(mangled)) return c.rt.hle.resolve(mangled, "dlsym");
    for (auto& img : c.rt.linker->images())
    {
        auto it = img->exports.find(mangled);
        if (it == img->exports.end() || (it->second.flags & macho::kExportReexport)) continue;
        return img->base + it->second.address;
    }
    if (c.rt.cache_images)
        if (auto f = c.rt.cache_images->lookup("/usr/lib/libSystem.B.dylib", mangled)) return f->addr;
    return std::nullopt;
}

}

void register_dyld(Hle& h)
{
    h.fn("__dyld_image_count", [](Cpu& c) { c.ret(c.rt.linker->images().size()); });
    h.fn("__dyld_get_image_header", [](Cpu& c) {
        auto* img = image_at(c, c.arg(0));
        c.ret(img ? img->base : 0);
    });
    h.fn("__dyld_get_image_vmaddr_slide", [](Cpu& c) {
        auto* img = image_at(c, c.arg(0));
        c.ret(img ? uint64_t(img->slide) : 0);
    });
    h.fn("__dyld_get_image_slide", [](Cpu& c) {
        for (auto& img : c.rt.linker->images())
            if (img->base == c.arg(0)) return c.ret(uint64_t(img->slide));
        c.ret(0);
    });
    h.fn("__dyld_get_image_name", [](Cpu& c) {
        auto* img = image_at(c, c.arg(0));
        c.ret(img ? image_name(c, *img) : 0);
    });
    h.fn("__dyld_register_func_for_add_image", [](Cpu& c) {
        GuestAddr fn = c.arg(0);
        for (auto& img : c.rt.linker->images())
        {
            if (c.stopped()) return;
            if (!img->base) continue;
            c.call(fn, {img->base, uint64_t(img->slide)});
        }
    });
    h.fn("__dyld_register_func_for_remove_image", [](Cpu& c) {});
    h.fn("__dyld_get_shared_cache_range", [](Cpu& c) {
        if (!c.rt.cache) return c.ret(0);
        if (c.arg(0)) c.mem.write<uint64_t>(c.arg(0), c.rt.cache->end() - c.rt.cache->start());
        c.ret(c.rt.cache->start());
    });
    h.fn("__dyld_is_memory_immutable", [](Cpu& c) { c.ret(0); });
    h.fn("_dyld_image_header_containing_address", [](Cpu& c) {
        if (auto* img = c.rt.linker->image_containing(c.arg(0))) return c.ret(img->base);
        auto* d = c.rt.cache_images ? c.rt.cache_images->dylib_containing(c.arg(0)) : nullptr;
        c.ret(d ? d->header : 0);
    });
    h.fn("__NSGetExecutablePath", [](Cpu& c) {
        std::string exe = c.mem.read_cstr(image_name(c, c.rt.linker->main_image()));
        uint32_t cap = c.mem.read<uint32_t>(c.arg(1));
        c.mem.write<uint32_t>(c.arg(1), uint32_t(exe.size() + 1));
        if (cap < exe.size() + 1) return c.ret(uint64_t(-1));
        c.mem.write_bytes(c.arg(0), exe.c_str(), exe.size() + 1);
        c.ret(0);
    });
    h.fn("_dyld_get_active_platform", [](Cpu& c) { c.ret(2); });
    h.fn("_dyld_get_program_sdk_version", [](Cpu& c) { c.ret(0x001b0000); });
    h.fn("_dyld_get_sdk_version", [](Cpu& c) { c.ret(0x001b0000); });
    h.fn("_dyld_get_program_min_os_version", [](Cpu& c) { c.ret(0x000f0000); });
    h.fn("_dyld_program_sdk_at_least", [](Cpu& c) { c.ret(1); });
    h.fn("_dyld_program_minos_at_least", [](Cpu& c) { c.ret(1); });
    h.fn("__tlv_atexit", [](Cpu& c) {});
    h.fn("___cxa_thread_atexit", [](Cpu& c) { c.ret(0); });

    auto find_in_header = [](Cpu& c, GuestAddr mh, const std::string& seg, const std::string* sect, uint64_t& size) -> GuestAddr {
        size = 0;
        if (!mh || !c.mem.is_mapped(mh, 32) || c.mem.read<uint32_t>(mh) != 0xfeedfacf)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                std::fprintf(stderr, "[dyld] section lookup on a bad image header 0x%llx from %s\n", (unsigned long long)mh,
                             c.rt.describe(c.lr()).c_str());
            }
            return 0;
        }
        uint32_t ncmds = c.mem.read<uint32_t>(mh + 16);
        int64_t slide = 0;
        GuestAddr cmd = mh + 32;
        std::vector<GuestAddr> segments;
        for (uint32_t i = 0; i < ncmds; ++i)
        {
            uint32_t kind = c.mem.read<uint32_t>(cmd), cmdsize = c.mem.read<uint32_t>(cmd + 4);
            if (kind == 0x19)
            {
                segments.push_back(cmd);
                if (c.mem.read<uint64_t>(cmd + 40) == 0 && c.mem.read<uint64_t>(cmd + 48) != 0)
                    slide = int64_t(mh) - int64_t(c.mem.read<uint64_t>(cmd + 24));
            }
            cmd += cmdsize;
        }
        for (GuestAddr s : segments)
        {
            if (c.mem.read_cstr(s + 8, 16) != seg) continue;
            if (!sect)
            {
                size = c.mem.read<uint64_t>(s + 32);
                return c.mem.read<uint64_t>(s + 24) + slide;
            }
            uint32_t nsects = c.mem.read<uint32_t>(s + 64);
            for (uint32_t k = 0; k < nsects; ++k)
            {
                GuestAddr sc = s + 72 + GuestAddr(k) * 80;
                if (c.mem.read_cstr(sc, 16) != *sect) continue;
                size = c.mem.read<uint64_t>(sc + 40);
                return c.mem.read<uint64_t>(sc + 32) + slide;
            }
        }
        return 0;
    };
    static decltype(find_in_header) s_find = find_in_header;
    h.fn("_getsectiondata", [](Cpu& c) {
        std::string seg = c.mem.read_cstr(c.arg(1), 16), sect = c.mem.read_cstr(c.arg(2), 16);
        uint64_t size = 0;
        GuestAddr at = s_find(c, c.arg(0), seg, &sect, size);
        if (c.arg(3)) c.mem.write<uint64_t>(c.arg(3), size);
        c.ret(at);
    });
    h.fn("_getsegmentdata", [](Cpu& c) {
        std::string seg = c.mem.read_cstr(c.arg(1), 16);
        uint64_t size = 0;
        GuestAddr at = s_find(c, c.arg(0), seg, nullptr, size);
        if (c.arg(2)) c.mem.write<uint64_t>(c.arg(2), size);
        c.ret(at);
    });
    for (const char* n : {"_dyld_shared_cache_some_image_overridden", "__dyld_swift_optimizations_version", "__dyld_is_objc_constant",
                          "__dyld_has_preoptimized_swift_protocol_conformances"})
        h.fn(n, [](Cpu& c) { c.ret(0); });
    h.fn("__dyld_find_unwind_sections", [](Cpu& c) {
        auto* img = c.rt.linker->image_containing(c.arg(0));
        if (!img) return c.ret(0);
        GuestAddr info = c.arg(1);
        auto* eh = section(*img, "__TEXT", "__eh_frame");
        auto* cu = section(*img, "__TEXT", "__unwind_info");
        c.mem.write<uint64_t>(info, img->base);
        c.mem.write<uint64_t>(info + 8, eh ? eh->addr + img->slide : 0);
        c.mem.write<uint64_t>(info + 16, eh ? eh->size : 0);
        c.mem.write<uint64_t>(info + 24, cu ? cu->addr + img->slide : 0);
        c.mem.write<uint64_t>(info + 32, cu ? cu->size : 0);
        c.ret(1);
    });

    h.fn("_dladdr", [](Cpu& c) {
        auto* img = c.rt.linker->image_containing(c.arg(0));
        if (!img) return c.ret(0);
        GuestAddr info = c.arg(1);
        c.mem.write<uint64_t>(info, image_name(c, *img));
        c.mem.write<uint64_t>(info + 8, img->base);
        c.mem.write<uint64_t>(info + 16, 0);
        c.mem.write<uint64_t>(info + 24, 0);
        c.ret(1);
    });
    h.fn("_dlopen", [](Cpu& c) {
        if (!c.arg(0)) return c.ret(c.rt.linker->main_image().base);
        std::string path = c.mem.read_cstr(c.arg(0));
        for (auto& img : c.rt.linker->images())
            if (img->install_path == path || c.mem.read_cstr(image_name(c, *img)) == path) return c.ret(img->base);
        if (c.rt.cache_images && c.rt.cache_images->dylib(path)) return c.ret(uint64_t(-2));
        c.ret(0);
    });
    h.fn("_dlopen_preflight", [](Cpu& c) { c.ret(1); });
    h.fn("_dlsym", [](Cpu& c) {
        std::string name = "_" + c.mem.read_cstr(c.arg(1));
        auto a = symbol_address(c, name);
        c.ret(a ? *a : 0);
    });
    h.fn("_dlclose", [](Cpu& c) { c.ret(0); });
    h.fn("_dlerror", [](Cpu& c) { c.ret(0); });
}

}
