#include "loader/macho.h"

#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace orchard::macho
{
namespace
{
constexpr uint32_t kMagic64 = 0xfeedfacf;
constexpr uint32_t kFatMagic = 0xcafebabe;
constexpr uint32_t kFatMagic64 = 0xcafebabf;
constexpr uint32_t kCpuArm64 = 0x0100000c;

constexpr uint32_t LC_SYMTAB = 0x2;
constexpr uint32_t LC_SEGMENT_64 = 0x19;
constexpr uint32_t LC_LOAD_DYLIB = 0xc;
constexpr uint32_t LC_ID_DYLIB = 0xd;
constexpr uint32_t LC_LAZY_LOAD_DYLIB = 0x20;
constexpr uint32_t LC_ENCRYPTION_INFO_64 = 0x2c;
constexpr uint32_t LC_BUILD_VERSION = 0x32;
constexpr uint32_t LC_LOAD_WEAK_DYLIB = 0x80000018;
constexpr uint32_t LC_REEXPORT_DYLIB = 0x8000001f;
constexpr uint32_t LC_DYLD_INFO = 0x22;
constexpr uint32_t LC_DYLD_INFO_ONLY = 0x80000022;
constexpr uint32_t LC_LOAD_UPWARD_DYLIB = 0x80000023;
constexpr uint32_t LC_MAIN = 0x80000028;
constexpr uint32_t LC_DYLD_EXPORTS_TRIE = 0x80000033;
constexpr uint32_t LC_DYLD_CHAINED_FIXUPS = 0x80000034;

class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    template <typename T> T le(size_t off) const
    {
        check(off, sizeof(T));
        T v;
        std::memcpy(&v, data_.data() + off, sizeof(T));
        return v;
    }

    uint32_t be32(size_t off) const
    {
        check(off, 4);
        const uint8_t* p = data_.data() + off;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    uint64_t be64(size_t off) const { return (uint64_t(be32(off)) << 32) | be32(off + 4); }

    std::string cstr(size_t off) const
    {
        check(off, 1);
        const char* p = reinterpret_cast<const char*>(data_.data() + off);
        size_t n = strnlen(p, data_.size() - off);
        return std::string(p, n);
    }

    std::string fixed(size_t off, size_t len) const
    {
        check(off, len);
        const char* p = reinterpret_cast<const char*>(data_.data() + off);
        return std::string(p, strnlen(p, len));
    }

    uint64_t uleb(size_t& off) const
    {
        uint64_t result = 0;
        unsigned shift = 0;
        for (;;)
        {
            uint8_t b = le<uint8_t>(off++);
            if (shift < 64) result |= uint64_t(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) return result;
        }
    }

    int64_t sleb(size_t& off) const
    {
        int64_t result = 0;
        unsigned shift = 0;
        uint8_t b;
        do
        {
            b = le<uint8_t>(off++);
            if (shift < 64) result |= int64_t(b & 0x7f) << shift;
            shift += 7;
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) result |= -(int64_t(1) << shift);
        return result;
    }

    std::span<const uint8_t> sub(size_t off, size_t len) const
    {
        check(off, len);
        return data_.subspan(off, len);
    }

    size_t size() const { return data_.size(); }

private:
    void check(size_t off, size_t len) const
    {
        if (off > data_.size() || len > data_.size() - off) throw std::runtime_error("mach-o: read out of bounds");
    }

    std::span<const uint8_t> data_;
};

std::span<const uint8_t> arm64_slice(std::span<const uint8_t> file)
{
    Reader r(file);
    uint32_t magic = r.be32(0);
    if (magic != kFatMagic && magic != kFatMagic64) return file;

    bool is64 = magic == kFatMagic64;
    uint32_t count = r.be32(4);
    size_t stride = is64 ? 32 : 20;
    for (uint32_t i = 0; i < count; ++i)
    {
        size_t at = 8 + i * stride;
        if (r.be32(at) != kCpuArm64) continue;
        uint64_t off = is64 ? r.be64(at + 8) : r.be32(at + 8);
        uint64_t size = is64 ? r.be64(at + 16) : r.be32(at + 12);
        return r.sub(size_t(off), size_t(size));
    }
    throw std::runtime_error("mach-o: fat binary has no arm64 slice");
}

int sign_extend_special(uint32_t ordinal, uint32_t max)
{
    if (ordinal > max - 3) return int(ordinal) - int(max) - 1;
    return int(ordinal);
}

void parse_chained_fixups(const Reader& r, uint32_t dataoff, Image& img)
{
    uint32_t imports_offset = r.le<uint32_t>(dataoff + 8);
    uint32_t symbols_offset = r.le<uint32_t>(dataoff + 12);
    uint32_t imports_count = r.le<uint32_t>(dataoff + 16);
    uint32_t imports_format = r.le<uint32_t>(dataoff + 20);
    uint32_t symbols_format = r.le<uint32_t>(dataoff + 24);
    if (symbols_format != 0) throw std::runtime_error("mach-o: compressed chained-fixup symbols");

    size_t imports = size_t(dataoff) + imports_offset;
    size_t symbols = size_t(dataoff) + symbols_offset;
    for (uint32_t i = 0; i < imports_count; ++i)
    {
        Import imp;
        uint64_t name_offset;
        switch (imports_format)
        {
        case 1:
        case 2: {
            size_t at = imports + size_t(i) * (imports_format == 1 ? 4 : 8);
            uint32_t v = r.le<uint32_t>(at);
            if (imports_format == 2) imp.addend = r.le<int32_t>(at + 4);
            imp.ordinal = sign_extend_special(v & 0xff, 0xff);
            imp.weak = (v >> 8) & 1;
            name_offset = v >> 9;
            break;
        }
        case 3: {
            uint64_t v = r.le<uint64_t>(imports + size_t(i) * 16);
            imp.addend = r.le<int64_t>(imports + size_t(i) * 16 + 8);
            imp.ordinal = sign_extend_special(uint32_t(v & 0xffff), 0xffff);
            imp.weak = (v >> 16) & 1;
            name_offset = v >> 32;
            break;
        }
        default: throw std::runtime_error("mach-o: unknown chained import format " + std::to_string(imports_format));
        }
        imp.name = r.cstr(symbols + size_t(name_offset));
        img.imports.push_back(std::move(imp));
    }
}

void parse_bind_opcodes(const Reader& r, uint32_t off, uint32_t size, bool lazy, Image& img, std::set<std::pair<int, std::string>>& seen)
{
    size_t i = off;
    size_t end = size_t(off) + size;
    int ordinal = 0;
    bool weak = false;
    std::string symbol;

    auto emit = [&] {
        if (seen.emplace(ordinal, symbol).second) img.imports.push_back({symbol, ordinal, weak});
    };

    while (i < end)
    {
        uint8_t byte = r.le<uint8_t>(i++);
        uint8_t imm = byte & 0x0f;
        switch (byte & 0xf0)
        {
        case 0x00:
            if (!lazy) return;
            break;
        case 0x10: ordinal = imm; break;
        case 0x20: ordinal = int(r.uleb(i)); break;
        case 0x30: ordinal = imm ? int(int8_t(0xf0 | imm)) : 0; break;
        case 0x40:
            symbol = r.cstr(i);
            i += symbol.size() + 1;
            weak = imm & 1;
            break;
        case 0x50: break;
        case 0x60: r.sleb(i); break;
        case 0x70: r.uleb(i); break;
        case 0x80: r.uleb(i); break;
        case 0x90: emit(); break;
        case 0xa0:
            r.uleb(i);
            emit();
            break;
        case 0xb0: emit(); break;
        case 0xc0:
            r.uleb(i);
            r.uleb(i);
            emit();
            break;
        case 0xd0:
            if (imm == 0) r.uleb(i);
            break;
        default: throw std::runtime_error("mach-o: bad bind opcode");
        }
    }
}

}

std::vector<uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::string version_string(uint32_t packed)
{
    return std::to_string(packed >> 16) + "." + std::to_string((packed >> 8) & 0xff);
}

uint64_t read_uleb(std::span<const uint8_t> data, size_t& off)
{
    return Reader(data).uleb(off);
}
int64_t read_sleb(std::span<const uint8_t> data, size_t& off)
{
    return Reader(data).sleb(off);
}

std::span<const uint8_t> slice(std::span<const uint8_t> file, const Image& img)
{
    return file.subspan(img.slice_offset, img.slice_size);
}

std::vector<Export> parse_exports(std::span<const uint8_t> slice_data, const Image& img)
{
    std::vector<Export> out;
    if (!img.exports.size) return out;
    Reader r(slice_data.subspan(img.exports.offset, img.exports.size));

    struct Pending
    {
        size_t node;
        std::string prefix;
    };
    std::vector<Pending> stack{{0, {}}};
    size_t visited = 0;
    while (!stack.empty())
    {
        Pending p = std::move(stack.back());
        stack.pop_back();
        if (++visited > r.size()) throw std::runtime_error("mach-o: export trie loops");

        size_t at = p.node;
        uint64_t terminal_size = r.uleb(at);
        size_t children = at + terminal_size;
        if (terminal_size)
        {
            Export e;
            e.name = p.prefix;
            e.flags = uint32_t(r.uleb(at));
            if (e.flags & kExportReexport)
            {
                e.reexport_ordinal = int(r.uleb(at));
                e.reexport_name = r.cstr(at);
            }
            else
            {
                e.address = r.uleb(at);
            }
            out.push_back(std::move(e));
        }
        at = children;
        uint8_t count = r.le<uint8_t>(at++);
        for (uint8_t i = 0; i < count; ++i)
        {
            std::string edge = r.cstr(at);
            at += edge.size() + 1;
            size_t child = size_t(r.uleb(at));
            stack.push_back({child, p.prefix + edge});
        }
    }
    return out;
}

std::vector<Symbol> parse_symbols(std::span<const uint8_t> slice_data, const Image& img)
{
    std::vector<Symbol> out;
    if (!img.symtab.size) return out;
    Reader r(slice_data);
    for (uint32_t i = 0; i < img.symtab.size / 16; ++i)
    {
        size_t at = size_t(img.symtab.offset) + size_t(i) * 16;
        uint32_t strx = r.le<uint32_t>(at);
        uint8_t type = r.le<uint8_t>(at + 4);
        if (type & 0xe0) continue;
        if ((type & 0x0e) != 0x0e) continue;
        if (strx >= img.strtab.size) continue;
        out.push_back({r.cstr(size_t(img.strtab.offset) + strx), r.le<uint64_t>(at + 8)});
    }
    return out;
}

Image parse(std::span<const uint8_t> file, bool with_imports)
{
    auto slice_data = arm64_slice(file);
    Reader r(slice_data);
    if (r.le<uint32_t>(0) != kMagic64) throw std::runtime_error("mach-o: not a 64-bit Mach-O");
    if (r.le<uint32_t>(4) != kCpuArm64) throw std::runtime_error("mach-o: not arm64");

    Image img;
    img.slice_offset = size_t(slice_data.data() - file.data());
    img.slice_size = slice_data.size();
    img.filetype = r.le<uint32_t>(12);
    uint32_t ncmds = r.le<uint32_t>(16);

    std::optional<uint32_t> chained_off;
    struct
    {
        uint32_t bind_off = 0, bind_size = 0, lazy_off = 0, lazy_size = 0;
    } dyld_info;
    bool have_dyld_info = false;

    size_t at = 32;
    for (uint32_t c = 0; c < ncmds; ++c)
    {
        uint32_t cmd = r.le<uint32_t>(at);
        uint32_t cmdsize = r.le<uint32_t>(at + 4);
        if (cmdsize < 8) throw std::runtime_error("mach-o: bad load command size");

        switch (cmd)
        {
        case LC_SEGMENT_64: {
            Segment seg;
            seg.name = r.fixed(at + 8, 16);
            seg.vmaddr = r.le<uint64_t>(at + 24);
            seg.vmsize = r.le<uint64_t>(at + 32);
            seg.fileoff = r.le<uint64_t>(at + 40);
            seg.filesize = r.le<uint64_t>(at + 48);
            seg.maxprot = r.le<uint32_t>(at + 56);
            seg.initprot = r.le<uint32_t>(at + 60);
            uint32_t nsects = r.le<uint32_t>(at + 64);
            for (uint32_t s = 0; s < nsects; ++s)
            {
                size_t sa = at + 72 + size_t(s) * 80;
                Section sec;
                sec.sectname = r.fixed(sa, 16);
                sec.segname = r.fixed(sa + 16, 16);
                sec.addr = r.le<uint64_t>(sa + 32);
                sec.size = r.le<uint64_t>(sa + 40);
                sec.offset = r.le<uint32_t>(sa + 48);
                sec.flags = r.le<uint32_t>(sa + 64);
                seg.sections.push_back(std::move(sec));
            }
            img.segments.push_back(std::move(seg));
            break;
        }
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB:
        case LC_LAZY_LOAD_DYLIB: {
            DylibKind kind = cmd == LC_LOAD_WEAK_DYLIB     ? DylibKind::Weak
                             : cmd == LC_REEXPORT_DYLIB    ? DylibKind::Reexport
                             : cmd == LC_LOAD_UPWARD_DYLIB ? DylibKind::Upward
                             : cmd == LC_LAZY_LOAD_DYLIB   ? DylibKind::Lazy
                                                           : DylibKind::Normal;
            img.dylibs.push_back({r.cstr(at + r.le<uint32_t>(at + 8)), kind});
            break;
        }
        case LC_SYMTAB:
            img.symtab = {r.le<uint32_t>(at + 8), r.le<uint32_t>(at + 12) * 16};
            img.strtab = {r.le<uint32_t>(at + 16), r.le<uint32_t>(at + 20)};
            break;
        case LC_ID_DYLIB: img.install_name = r.cstr(at + r.le<uint32_t>(at + 8)); break;
        case LC_ENCRYPTION_INFO_64: img.cryptid = r.le<uint32_t>(at + 16); break;
        case LC_BUILD_VERSION:
            img.minos = r.le<uint32_t>(at + 12);
            img.sdk = r.le<uint32_t>(at + 16);
            break;
        case LC_MAIN: img.entryoff = r.le<uint64_t>(at + 8); break;
        case LC_DYLD_CHAINED_FIXUPS:
            chained_off = r.le<uint32_t>(at + 8);
            img.chained_fixups = {r.le<uint32_t>(at + 8), r.le<uint32_t>(at + 12)};
            break;
        case LC_DYLD_EXPORTS_TRIE: img.exports = {r.le<uint32_t>(at + 8), r.le<uint32_t>(at + 12)}; break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY:
            have_dyld_info = true;
            img.rebase = {r.le<uint32_t>(at + 8), r.le<uint32_t>(at + 12)};
            img.bind = {r.le<uint32_t>(at + 16), r.le<uint32_t>(at + 20)};
            img.weak_bind = {r.le<uint32_t>(at + 24), r.le<uint32_t>(at + 28)};
            img.lazy_bind = {r.le<uint32_t>(at + 32), r.le<uint32_t>(at + 36)};
            if (r.le<uint32_t>(at + 44)) img.exports = {r.le<uint32_t>(at + 40), r.le<uint32_t>(at + 44)};
            dyld_info.bind_off = img.bind.offset;
            dyld_info.bind_size = img.bind.size;
            dyld_info.lazy_off = img.lazy_bind.offset;
            dyld_info.lazy_size = img.lazy_bind.size;
            break;
        default: break;
        }
        at += cmdsize;
    }

    if (!with_imports) return img;
    if (chained_off)
    {
        img.fixups = FixupKind::ChainedFixups;
        parse_chained_fixups(r, *chained_off, img);
    }
    else if (have_dyld_info)
    {
        img.fixups = FixupKind::DyldInfo;
        std::set<std::pair<int, std::string>> seen;
        parse_bind_opcodes(r, dyld_info.bind_off, dyld_info.bind_size, false, img, seen);
        parse_bind_opcodes(r, dyld_info.lazy_off, dyld_info.lazy_size, true, img, seen);
    }
    return img;
}

}
