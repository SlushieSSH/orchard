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
