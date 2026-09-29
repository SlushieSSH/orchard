#include "loader/cache_images.h"

#include <cstdio>
#include <deque>
#include <functional>

#include "core/runtime.h"

namespace orchard
{
namespace
{
constexpr uint32_t kSvc1 = 0xd4000021;
constexpr uint32_t S_ATTR_PURE_INSTRUCTIONS = 0x80000000;
constexpr uint32_t S_ATTR_SOME_INSTRUCTIONS = 0x00000400;

}

bool CacheImages::is_real_path(const std::string& p)
{
    static const char* kReal[] = {
        "/usr/lib/libc++.1.dylib",
        "/usr/lib/libc++abi.dylib",
        "/usr/lib/system/libunwind.dylib",
        "/usr/lib/system/libsystem_c.dylib",
        "/usr/lib/system/libsystem_m.dylib",
        "/usr/lib/system/libcompiler_rt.dylib",
        "/usr/lib/system/libsystem_platform.dylib",
        "/usr/lib/system/libmacho.dylib",
        "/usr/lib/system/libcommonCrypto.dylib",
        "/usr/lib/system/libcorecrypto.dylib",
        "/usr/lib/libz.1.dylib",
        "/usr/lib/libsqlite3.dylib",
        "/usr/lib/libcompression.dylib",
        "/usr/lib/libxml2.2.dylib",
        "/usr/lib/libiconv.2.dylib",
        "/usr/lib/libicucore.A.dylib",
        "/usr/lib/liblzma.5.dylib",
        "/usr/lib/libbz2.1.0.dylib",
    };
    if (p.starts_with("/usr/lib/swift/")) return true;
    for (const char* r : kReal)
        if (p == r) return true;
    return false;
}

CacheImages::CacheImages(Runtime& rt, DyldCache& cache) : rt_(rt), cache_(cache) {}

CacheImages::Dylib* CacheImages::dylib(const std::string& path)
{
    if (auto it = dylibs_.find(path); it != dylibs_.end()) return it->second.get();
    auto header = cache_.image_header(path);
    if (!header) return nullptr;

    auto d = std::make_unique<Dylib>();
    d->path = path;
    d->header = *header;
    uint32_t sizeofcmds = rt_.mem.read<uint32_t>(*header + 20);
    d->macho = macho::parse(std::span<const uint8_t>(rt_.mem.host(*header), sizeofcmds + 32), false);
    d->real = is_real_path(path);
    for (auto& seg : d->macho.segments)
        for (auto& sec : seg.sections)
            if (sec.flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))
                d->text_ranges.emplace_back(sec.addr, sec.addr + sec.size);
    Dylib* raw = d.get();
    dylibs_[path] = std::move(d);
    return raw;
}

CacheImages::Dylib* CacheImages::dylib_containing(GuestAddr addr)
{
    if (!cache_.contains(addr)) return nullptr;
    for (auto& [path, d] : dylibs_)
        for (auto& seg : d->macho.segments)
            if (seg.name != "__LINKEDIT" && addr >= seg.vmaddr && addr < seg.vmaddr + seg.vmsize) return d.get();
    return nullptr;
}

void CacheImages::load_exports(Dylib& d)
{
    if (d.exports_loaded) return;
    d.exports_loaded = true;
    if (!d.macho.exports.size) return;
    const macho::Segment* linkedit = nullptr;
    for (auto& seg : d.macho.segments)
        if (seg.name == "__LINKEDIT") linkedit = &seg;
    if (!linkedit) return;
    GuestAddr trie = linkedit->vmaddr + (d.macho.exports.offset - linkedit->fileoff);
    macho::Image view;
    view.exports = {0, d.macho.exports.size};
    auto span = std::span<const uint8_t>(rt_.mem.host(trie), d.macho.exports.size);
    for (auto& e : macho::parse_exports(span, view))
        d.exports.emplace(e.name, e);
}

std::optional<CacheImages::Found> CacheImages::lookup(const std::string& path, const std::string& name, int depth)
{
    if (depth > 12) return std::nullopt;
    Dylib* d = dylib(path);
    if (!d) return std::nullopt;
    load_exports(*d);
    if (auto it = d->exports.find(name); it != d->exports.end())
    {
        const auto& e = it->second;
        if (e.flags & macho::kExportReexport)
        {
            if (e.reexport_ordinal < 1 || size_t(e.reexport_ordinal) > d->macho.dylibs.size()) return std::nullopt;
            return lookup(d->macho.dylibs[e.reexport_ordinal - 1].path, e.reexport_name.empty() ? name : e.reexport_name, depth + 1);
        }
        if ((e.flags & macho::kExportKindMask) == macho::kExportKindAbsolute) return Found{e.address, d};
        return Found{d->header + e.address, d};
    }
    for (auto& dep : d->macho.dylibs)
        if (dep.kind == macho::DylibKind::Reexport)
            if (auto f = lookup(dep.path, name, depth + 1)) return f;
    return std::nullopt;
}

bool CacheImages::is_function(Dylib& d, GuestAddr addr) const
{
    for (auto& [lo, hi] : d.text_ranges)
        if (addr >= lo && addr < hi) return true;
    return false;
}

void CacheImages::collect_real(const std::string& root)
{
    std::function<void(const std::string&)> visit = [&](const std::string& path) {
        if (!real_seen_.insert(path).second) return;
        Dylib* d = dylib(path);
        if (!d || !d->real) return;
        for (auto& dep : d->macho.dylibs)
            visit(dep.path);
        real_order_.push_back(d);
    };
    visit(root);
}

void CacheImages::patch_reachable(const std::vector<std::string>& roots)
{
    std::deque<std::string> queue(roots.begin(), roots.end());
    std::unordered_set<std::string> seen(roots.begin(), roots.end());
    size_t patched = 0;
    while (!queue.empty())
    {
        std::string path = queue.front();
        queue.pop_front();
        Dylib* d = dylib(path);
        if (!d) continue;
        for (auto& dep : d->macho.dylibs)
        {
            if ((d->real || dep.kind == macho::DylibKind::Reexport) && seen.insert(dep.path).second) queue.push_back(dep.path);
        }
        if (!patched_dylibs_.insert(path).second) continue;
        load_exports(*d);
        for (auto& [name, e] : d->exports)
        {
            if (e.flags & (macho::kExportReexport | macho::kExportKindAbsolute)) continue;
            if ((d->real || name.starts_with("_$s")) && !rt_.hle.claims(name)) continue;
            GuestAddr addr = d->header + e.address;
            if (!is_function(*d, addr) || patches_.count(addr)) continue;
            patches_[addr] = rt_.hle.resolve(name, path);
            rt_.mem.write<uint32_t>(addr, kSvc1);
            ++patched;
        }
    }
    std::printf("patched %zu entry points in %zu replaced system dylibs\n", patched, patched_dylibs_.size());
}

GuestAddr CacheImages::relative_selector_base()
{
    if (selector_base_) return selector_base_;
    Dylib* objc = dylib("/usr/lib/libobjc.A.dylib");
    if (!objc) return 0;
    for (auto& seg : objc->macho.segments)
        for (auto& sec : seg.sections)
            if (sec.sectname == "__objc_opt_ro" && rt_.mem.read<uint32_t>(sec.addr) >= 16)
                selector_base_ = sec.addr + rt_.mem.read<int64_t>(sec.addr + 0x28);
    return selector_base_;
}

std::optional<GuestAddr> CacheImages::patch_target(GuestAddr entry) const
{
    auto it = patches_.find(entry);
    if (it == patches_.end()) return std::nullopt;
    return it->second;
}

}
