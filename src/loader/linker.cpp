#include "loader/linker.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "loader/cache_images.h"
#include "objc/runtime.h"

namespace fs = std::filesystem;

namespace orchard
{
namespace
{
constexpr uint32_t S_MOD_INIT_FUNC_POINTERS = 0x09;
constexpr uint32_t S_THREAD_LOCAL_REGULAR = 0x11;
constexpr uint32_t S_THREAD_LOCAL_ZEROFILL = 0x12;
constexpr uint32_t S_THREAD_LOCAL_VARIABLES = 0x13;
constexpr uint32_t S_INIT_FUNC_OFFSETS = 0x16;

constexpr uint16_t DYLD_CHAINED_PTR_64 = 2;
constexpr uint16_t DYLD_CHAINED_PTR_64_OFFSET = 6;
constexpr uint16_t DYLD_CHAINED_PTR_START_NONE = 0xffff;

std::string basename(const std::string& path)
{
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool is_system_path(const std::string& path)
{
    return path.starts_with("/System/") || path.starts_with("/usr/lib/");
}

std::string canonical_library(const std::string& path)
{
    if (path.starts_with("@rpath/libswift")) return "/usr/lib/swift/" + path.substr(7);
    return path;
}

template <typename T> T rd(std::span<const uint8_t> d, size_t off)
{
    if (off + sizeof(T) > d.size()) throw std::runtime_error("linker: linkedit read out of bounds");
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

}

Linker::Linker(Runtime& rt) : rt_(rt)
{
    rt_.linker = this;
}

void Linker::load_bundle(const fs::path& app)
{
    std::vector<fs::path> paths{app / app.stem()};
    fs::path frameworks = app / "Frameworks";
    if (fs::is_directory(frameworks))
    {
        std::vector<fs::path> entries;
        for (auto& e : fs::directory_iterator(frameworks))
            entries.push_back(e.path());
        std::sort(entries.begin(), entries.end());
        for (auto& p : entries)
        {
            if (p.extension() == ".framework" && fs::exists(p / p.stem()))
                paths.push_back(p / p.stem());
            else if (p.extension() == ".dylib")
                paths.push_back(p);
        }
    }

    GuestAddr next_base = 0;
    for (auto& p : paths)
        load_image(p, next_base);
    for (size_t i = 1; i < images_.size(); ++i)
        images_[i]->inert = !rt_.run_all_frameworks && images_[i]->name != "UnityFramework";

    for (auto& img : images_)
    {
        for (auto& d : img->macho.dylibs)
            if (auto* dep = bundled_image(d.path)) img->deps.push_back(dep);
    }
    for (auto& img : images_)
        link(*img);
    size_t app_images = images_.size();
    attach_cache_dylibs();
    for (auto& img : images_)
    {
        collect_initializers(*img);
        setup_tlv(*img);
    }
    for (size_t i = app_images; i < images_.size(); ++i)
        rt_.objc->register_image(*images_[i]);
    for (size_t i = 0; i < app_images; ++i)
        rt_.objc->register_image(*images_[i]);
}

LoadedImage& Linker::load_image(const fs::path& path, GuestAddr& next_base)
{
    auto img = std::make_unique<LoadedImage>();
    img->name = path.filename().string();
    img->path = path;
    img->file = macho::read_file(path);
    img->macho = macho::parse(img->file);
    if (img->macho.cryptid.value_or(0) != 0)
        throw std::runtime_error(img->name + " is still FairPlay-encrypted; a decrypted IPA is required");

    uint64_t lo = ~uint64_t(0), hi = 0;
    for (auto& seg : img->macho.segments)
    {
        if (seg.name == "__PAGEZERO" || !seg.vmsize) continue;
        lo = std::min(lo, seg.vmaddr);
        hi = std::max(hi, seg.vmaddr + seg.vmsize);
    }
    if (lo > hi) throw std::runtime_error(img->name + " has no segments");

    bool is_main = images_.empty();
    GuestAddr base;
    if (is_main)
    {
        base = lo;
        if (base < layout::kImagesBase) throw std::runtime_error("main executable below 4 GiB is unsupported");
        next_base = page_align_up(base + (hi - lo)) + 0x100000;
    }
    else
    {
        base = next_base;
        next_base = page_align_up(base + (hi - lo)) + 0x100000;
    }
    if (next_base >= layout::kStubsBase) throw std::runtime_error("images do not fit below the stubs area");
    img->slide = int64_t(base) - int64_t(lo);
    img->base = base;
    img->end = base + (hi - lo);

    auto slice = img->slice();
    for (auto& seg : img->macho.segments)
    {
        if (seg.name == "__PAGEZERO" || !seg.vmsize) continue;
        GuestAddr at = seg.vmaddr + img->slide;
        rt_.mem.map(at, seg.vmsize, img->name + ":" + seg.name);
        uint64_t n = std::min(seg.filesize, seg.vmsize);
        if (n)
        {
            if (seg.fileoff + n > slice.size()) throw std::runtime_error(img->name + ": segment beyond end of file");
            rt_.mem.write_bytes(at, slice.data() + seg.fileoff, n);
        }
    }

    for (auto& e : macho::parse_exports(slice, img->macho))
        img->exports.emplace(e.name, e);

    for (auto& seg : img->macho.segments)
        if (seg.name != "__PAGEZERO" && seg.vmsize) img->ranges.emplace_back(seg.vmaddr + img->slide, seg.vmaddr + img->slide + seg.vmsize);
    by_name_[img->name] = img.get();
    by_base_[img->base] = img.get();
    images_.push_back(std::move(img));
    return *images_.back();
}

std::optional<GuestAddr> Linker::resolve_in_cache(const std::string& lib, const std::string& name)
{
    if (!rt_.cache_images || rt_.hle.claims(name)) return std::nullopt;
    auto found = rt_.cache_images->lookup(lib, name);
    if (!found) return std::nullopt;
    if (found->owner->real)
    {
        cache_roots_.push_back(found->owner->path);
        return found->addr;
    }
    if (!rt_.cache_images->is_function(*found->owner, found->addr) || name.starts_with("_$s")) return found->addr;
    return std::nullopt;
}

void Linker::attach_cache_dylibs()
{
    auto* ci = rt_.cache_images.get();
    if (!ci) return;
    std::sort(cache_roots_.begin(), cache_roots_.end());
    cache_roots_.erase(std::unique(cache_roots_.begin(), cache_roots_.end()), cache_roots_.end());
    for (auto& r : cache_roots_)
        ci->collect_real(r);
    std::vector<std::string> real;
    for (auto* d : ci->real_dylibs())
        real.push_back(d->path);
    ci->patch_reachable(real);
    if (auto f = ci->lookup("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", "___CFConstantStringClassReference"))
        rt_.objc->alias(f->addr, rt_.objc->host_class("__NSCFConstantString"));

    for (auto* d : ci->real_dylibs())
    {
        auto img = std::make_unique<LoadedImage>();
        img->name = basename(d->path);
        img->install_path = d->path;
        img->from_cache = true;
        img->macho = d->macho;
        img->base = d->header;
        img->end = d->header;
        for (auto& seg : d->macho.segments)
        {
            if (seg.name == "__LINKEDIT" || !seg.vmsize) continue;
            img->ranges.emplace_back(seg.vmaddr, seg.vmaddr + seg.vmsize);
            img->end = std::max(img->end, seg.vmaddr + seg.vmsize);
        }
        for (auto& [n, e] : d->exports)
            img->exports.emplace(n, e);
        by_base_[img->base] = img.get();
        images_.push_back(std::move(img));
    }
    std::printf("using %zu real system dylibs from the shared cache\n", ci->real_dylibs().size());
}

LoadedImage* Linker::bundled_image(const std::string& install_path)
{
    if (is_system_path(canonical_library(install_path))) return nullptr;
    auto it = by_name_.find(basename(install_path));
    return it == by_name_.end() ? nullptr : it->second;
}

std::optional<GuestAddr> Linker::lookup_export(LoadedImage& img, const std::string& name, int depth)
{
    if (depth > 8) return std::nullopt;
    auto it = img.exports.find(name);
    if (it == img.exports.end()) return std::nullopt;
    const auto& e = it->second;
    if (e.flags & macho::kExportReexport)
    {
        if (e.reexport_ordinal < 1 || size_t(e.reexport_ordinal) > img.macho.dylibs.size()) return std::nullopt;
        const std::string& lib = canonical_library(img.macho.dylibs[e.reexport_ordinal - 1].path);
        const std::string& target = e.reexport_name.empty() ? name : e.reexport_name;
        if (auto* dep = bundled_image(lib)) return lookup_export(*dep, target, depth + 1);
        return rt_.hle.resolve(target, lib);
    }
    if ((e.flags & macho::kExportKindMask) == macho::kExportKindAbsolute) return e.address;
    return img.base + e.address;
}

GuestAddr Linker::resolve_import(LoadedImage& img, const macho::Import& imp)
{
    auto search_all = [&]() -> std::optional<GuestAddr> {
        for (auto& other : images_)
            if (auto a = lookup_export(*other, imp.name)) return a;
        return std::nullopt;
    };

    std::optional<GuestAddr> found;
    std::string lib;
    switch (imp.ordinal)
    {
    case macho::kOrdinalSelf:
        found = lookup_export(img, imp.name);
        lib = img.name;
        break;
    case macho::kOrdinalMainExecutable:
        found = lookup_export(main_image(), imp.name);
        lib = main_image().name;
        break;
    case macho::kOrdinalFlatLookup:
        found = search_all();
        lib = "<flat>";
        break;
    case macho::kOrdinalWeakLookup:
        found = search_all();
        lib = "/usr/lib/libc++.1.dylib";
        break;
    default: {
        if (imp.ordinal < 1 || size_t(imp.ordinal) > img.macho.dylibs.size())
        {
            ++stats_.unresolved;
            return 0;
        }
        lib = canonical_library(img.macho.dylibs[imp.ordinal - 1].path);
        if (auto* dep = bundled_image(lib))
        {
            found = lookup_export(*dep, imp.name);
        }
        else if (is_system_path(lib))
        {
            ++stats_.system_binds;
            if (auto a = resolve_in_cache(lib, imp.name)) return *a;
            return rt_.hle.resolve(imp.name, lib);
        }
    }
    }
    if (found) return *found;
    if (imp.ordinal == macho::kOrdinalWeakLookup || imp.ordinal == macho::kOrdinalFlatLookup)
    {
        ++stats_.system_binds;
        for (const char* cache_lib : {"/usr/lib/libc++.1.dylib", "/usr/lib/libSystem.B.dylib"})
            if (auto a = resolve_in_cache(cache_lib, imp.name)) return *a;
        return rt_.hle.resolve(imp.name, lib);
    }
    if (imp.weak) return 0;
    ++stats_.unresolved;
    std::fprintf(stderr, "[link] %s: unresolved %s from %s\n", img.name.c_str(), imp.name.c_str(), lib.c_str());
    return rt_.hle.resolve(imp.name, lib);
}

void Linker::link(LoadedImage& img)
{
    if (img.macho.fixups == macho::FixupKind::ChainedFixups)
        apply_chained_fixups(img);
    else if (img.macho.fixups == macho::FixupKind::DyldInfo)
        apply_dyld_info(img);
}

void Linker::apply_chained_fixups(LoadedImage& img)
{
    auto slice = img.slice();
    size_t hdr = img.macho.chained_fixups.offset;
    uint32_t starts_offset = rd<uint32_t>(slice, hdr + 4);

    std::vector<GuestAddr> targets;
    targets.reserve(img.macho.imports.size());
    for (auto& imp : img.macho.imports)
        targets.push_back(resolve_import(img, imp) + GuestAddr(imp.addend));

    size_t starts = hdr + starts_offset;
    uint32_t seg_count = rd<uint32_t>(slice, starts);
    for (uint32_t s = 0; s < seg_count; ++s)
    {
        uint32_t info_off = rd<uint32_t>(slice, starts + 4 + size_t(s) * 4);
        if (!info_off) continue;
        size_t info = starts + info_off;
        uint16_t page_size = rd<uint16_t>(slice, info + 4);
        uint16_t format = rd<uint16_t>(slice, info + 6);
        uint64_t segment_offset = rd<uint64_t>(slice, info + 8);
        uint16_t page_count = rd<uint16_t>(slice, info + 20);
        if (format != DYLD_CHAINED_PTR_64 && format != DYLD_CHAINED_PTR_64_OFFSET)
            throw std::runtime_error(img.name + ": unsupported chained pointer format " + std::to_string(format));

        for (uint16_t p = 0; p < page_count; ++p)
        {
            uint16_t start = rd<uint16_t>(slice, info + 22 + size_t(p) * 2);
            if (start == DYLD_CHAINED_PTR_START_NONE) continue;
            GuestAddr at = img.base + segment_offset + uint64_t(p) * page_size + start;
            for (;;)
            {
                uint64_t v = rt_.mem.read<uint64_t>(at);
                uint64_t next = (v >> 51) & 0xfff;
                uint64_t value;
                if (v >> 63)
                {
                    uint32_t ordinal = uint32_t(v & 0xffffff);
                    uint64_t addend = (v >> 24) & 0xff;
                    if (ordinal >= targets.size()) throw std::runtime_error(img.name + ": bind ordinal out of range");
                    value = targets[ordinal] + addend;
                    ++stats_.binds;
                }
                else
                {
                    uint64_t target = v & 0xfffffffff;
                    uint64_t high8 = (v >> 36) & 0xff;
                    value = (format == DYLD_CHAINED_PTR_64 ? target + img.slide : img.base + target) | (high8 << 56);
                    ++stats_.rebases;
                }
                rt_.mem.write<uint64_t>(at, value);
                if (!next) break;
                at += next * 4;
            }
        }
    }
}

void Linker::apply_dyld_info(LoadedImage& img)
{
    auto slice = img.slice();
    auto seg_addr = [&](uint32_t index) -> GuestAddr {
        if (index >= img.macho.segments.size()) throw std::runtime_error(img.name + ": bad segment index");
        return img.macho.segments[index].vmaddr + img.slide;
    };

    {
        size_t i = img.macho.rebase.offset, end = i + img.macho.rebase.size;
        GuestAddr addr = 0;
        auto rebase = [&] {
            rt_.mem.write<uint64_t>(addr, rt_.mem.read<uint64_t>(addr) + img.slide);
            addr += 8;
            ++stats_.rebases;
        };
        while (i < end)
        {
            uint8_t b = slice[i++];
            uint8_t imm = b & 0x0f;
            switch (b & 0xf0)
            {
            case 0x00: i = end; break;
            case 0x10: break;
            case 0x20: addr = seg_addr(imm) + macho::read_uleb(slice, i); break;
            case 0x30: addr += macho::read_uleb(slice, i); break;
            case 0x40: addr += uint64_t(imm) * 8; break;
            case 0x50:
                for (int k = 0; k < imm; ++k)
                    rebase();
                break;
            case 0x60:
                for (uint64_t k = 0, n = macho::read_uleb(slice, i); k < n; ++k)
                    rebase();
                break;
            case 0x70: {
                rebase();
                addr += macho::read_uleb(slice, i);
                break;
            }
            case 0x80: {
                uint64_t n = macho::read_uleb(slice, i), skip = macho::read_uleb(slice, i);
                for (uint64_t k = 0; k < n; ++k)
                {
                    rebase();
                    addr += skip;
                }
                break;
            }
            default: throw std::runtime_error(img.name + ": bad rebase opcode");
            }
        }
    }

    auto run_binds = [&](const macho::LinkeditRange& range, bool lazy) {
        size_t i = range.offset, end = i + range.size;
        GuestAddr addr = 0;
        macho::Import imp;
        auto bind = [&] {
            rt_.mem.write<uint64_t>(addr, resolve_import(img, imp) + GuestAddr(imp.addend));
            addr += 8;
            ++stats_.binds;
        };
        while (i < end)
        {
            uint8_t b = slice[i++];
            uint8_t imm = b & 0x0f;
            switch (b & 0xf0)
            {
            case 0x00:
                if (!lazy) i = end;
                break;
            case 0x10: imp.ordinal = imm; break;
            case 0x20: imp.ordinal = int(macho::read_uleb(slice, i)); break;
            case 0x30: imp.ordinal = imm ? int(int8_t(0xf0 | imm)) : 0; break;
            case 0x40:
                imp.name = reinterpret_cast<const char*>(slice.data() + i);
                i += imp.name.size() + 1;
                imp.weak = imm & 1;
                break;
            case 0x50: break;
            case 0x60: imp.addend = macho::read_sleb(slice, i); break;
            case 0x70: addr = seg_addr(imm) + macho::read_uleb(slice, i); break;
            case 0x80: addr += macho::read_uleb(slice, i); break;
            case 0x90: bind(); break;
            case 0xa0:
                bind();
                addr += macho::read_uleb(slice, i);
                break;
            case 0xb0:
                bind();
                addr += uint64_t(imm) * 8;
                break;
            case 0xc0: {
                uint64_t n = macho::read_uleb(slice, i), skip = macho::read_uleb(slice, i);
                for (uint64_t k = 0; k < n; ++k)
                {
                    bind();
                    addr += skip;
                }
                break;
            }
            default: throw std::runtime_error(img.name + ": unsupported bind opcode");
            }
        }
    };
    run_binds(img.macho.bind, false);
    run_binds(img.macho.lazy_bind, true);
}

void Linker::collect_initializers(LoadedImage& img)
{
    for (auto& seg : img.macho.segments)
    {
        for (auto& sec : seg.sections)
        {
            uint32_t type = sec.flags & 0xff;
            GuestAddr at = sec.addr + img.slide;
            if (type == S_MOD_INIT_FUNC_POINTERS)
            {
                for (uint64_t o = 0; o + 8 <= sec.size; o += 8)
                    img.initializers.push_back(rt_.mem.read<uint64_t>(at + o));
            }
            else if (type == S_INIT_FUNC_OFFSETS)
            {
                for (uint64_t o = 0; o + 4 <= sec.size; o += 4)
                    img.initializers.push_back(img.base + rt_.mem.read<uint32_t>(at + o));
            }
        }
    }
}

void Linker::setup_tlv(LoadedImage& img)
{
    GuestAddr lo = ~GuestAddr(0), init_end = 0, hi = 0;
    std::vector<const macho::Section*> vars;
    for (auto& seg : img.macho.segments)
    {
        for (auto& sec : seg.sections)
        {
            uint32_t type = sec.flags & 0xff;
            GuestAddr at = sec.addr + img.slide;
            if (type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL)
            {
                lo = std::min(lo, at);
                hi = std::max(hi, at + sec.size);
                if (type == S_THREAD_LOCAL_REGULAR) init_end = std::max(init_end, at + sec.size);
            }
            else if (type == S_THREAD_LOCAL_VARIABLES)
            {
                vars.push_back(&sec);
            }
        }
    }
    if (vars.empty()) return;
    if (lo > hi) lo = hi = init_end = 0;
    img.tlv_start = lo;
    img.tlv_init_size = init_end > lo ? init_end - lo : 0;
    img.tlv_total_size = hi - lo;

    uint64_t key = tlv_keys_.size() + 1;
    tlv_keys_[key] = &img;
    for (auto* sec : vars)
    {
        for (uint64_t o = 0; o + 24 <= sec->size; o += 24)
            rt_.mem.write<uint64_t>(sec->addr + img.slide + o + 8, key);
    }
}

GuestAddr Linker::tlv_address(Cpu& cpu, GuestAddr descriptor)
{
    uint64_t key = rt_.mem.read<uint64_t>(descriptor + 8);
    uint64_t offset = rt_.mem.read<uint64_t>(descriptor + 16);
    auto it = cpu.tlv_blocks.find(key);
    if (it == cpu.tlv_blocks.end())
    {
        auto img = tlv_keys_.find(key);
        if (img == tlv_keys_.end())
        {
            cpu.stop("tlv_get_addr with unknown key");
            return 0;
        }
        LoadedImage& li = *img->second;
        GuestAddr block = rt_.heap.calloc(std::max<uint64_t>(li.tlv_total_size, 16));
        if (li.tlv_init_size) std::memcpy(rt_.mem.host(block), rt_.mem.host(li.tlv_start), li.tlv_init_size);
        it = cpu.tlv_blocks.emplace(key, block).first;
    }
    return it->second + offset;
}

void Linker::init_image(Cpu& cpu, LoadedImage& img, GuestAddr argc, GuestAddr argv, GuestAddr envp, GuestAddr apple)
{
    if (img.initialized || cpu.stopped()) return;
    img.initialized = true;
    for (auto* dep : img.deps)
        init_image(cpu, *dep, argc, argv, envp, apple);
    if (img.inert) return;
    rt_.objc->call_loads(cpu, img);
    for (GuestAddr fn : img.initializers)
    {
        if (cpu.stopped()) return;
        cpu.call(fn, {argc, argv, envp, apple, 0});
    }
}

void Linker::run_initializers(Cpu& cpu, GuestAddr argc, GuestAddr argv, GuestAddr envp, GuestAddr apple)
{
    for (auto& img : images_)
        if (img->from_cache) init_image(cpu, *img, argc, argv, envp, apple);
    rt_.objc->attach_stub_categories(cpu);
    for (size_t i = 1; i < images_.size(); ++i)
        init_image(cpu, *images_[i], argc, argv, envp, apple);
    init_image(cpu, main_image(), argc, argv, envp, apple);
}

GuestAddr Linker::main_entry() const
{
    const auto& m = *images_.front();
    if (!m.macho.entryoff) throw std::runtime_error("main executable has no LC_MAIN");
    return m.base + *m.macho.entryoff;
}

const LoadedImage* Linker::image_containing(GuestAddr addr) const
{
    auto it = by_base_.upper_bound(addr);
    if (it != by_base_.begin())
    {
        --it;
        if (!it->second->from_cache && addr < it->second->end) return it->second;
    }
    for (auto& img : images_)
        if (img->from_cache)
            for (auto& [lo, hi] : img->ranges)
                if (addr >= lo && addr < hi) return img.get();
    return nullptr;
}

std::string Linker::symbolicate(GuestAddr addr) const
{
    char buf[64];
    if (rt_.hle.is_stub(addr)) return "[hle " + rt_.hle.stub_name(addr & ~GuestAddr(7)) + "]";
    const LoadedImage* img = image_containing(addr);
    if (!img)
    {
        std::snprintf(buf, sizeof buf, "0x%llx (%s)", (unsigned long long)addr, rt_.mem.describe(addr).c_str());
        return buf;
    }
    std::snprintf(buf, sizeof buf, "+0x%llx", (unsigned long long)(addr - img->base));
    std::string out = img->name + buf;

    auto& syms = symbols_[img];
    if (syms.empty())
    {
        if (!img->from_cache)
            for (auto& s : macho::parse_symbols(img->slice(), img->macho))
                syms[s.address + img->slide] = s.name;
        for (auto& [name, e] : img->exports)
            if (!(e.flags & (macho::kExportReexport | macho::kExportKindAbsolute))) syms.emplace(img->base + e.address, name);
        if (syms.empty()) syms[0] = "";
    }
    auto it = syms.upper_bound(addr);
    if (it != syms.begin())
    {
        --it;
        if (it->first >= img->base && !it->second.empty() && addr - it->first < 0x2000)
        {
            std::snprintf(buf, sizeof buf, "+0x%llx", (unsigned long long)(addr - it->first));
            out += " (" + it->second + buf + ")";
        }
    }
    return out;
}

}
