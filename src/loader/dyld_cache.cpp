#include "loader/dyld_cache.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

namespace orchard
{
struct DyldCache::File
{
    fs::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t size = 0;

    void read(uint64_t offset, void* dst, uint64_t len) const
    {
        auto* out = static_cast<uint8_t*>(dst);
        while (len)
        {
            OVERLAPPED ov{};
            ov.Offset = DWORD(offset);
            ov.OffsetHigh = DWORD(offset >> 32);
            DWORD chunk = DWORD(std::min<uint64_t>(len, 1u << 30)), got = 0;
            if (!ReadFile(handle, out, chunk, &got, &ov) || got == 0)
            {
                std::memset(out, 0, len);
                return;
            }
            out += got;
            offset += got;
            len -= got;
        }
    }
    template <typename T> T at(uint64_t offset) const
    {
        T v{};
        read(offset, &v, sizeof(T));
        return v;
    }
};

namespace
{
constexpr uint16_t V2_PAGE_ATTR_EXTRA = 0x8000;
constexpr uint16_t V2_PAGE_ATTR_NO_REBASE = 0x4000;
constexpr uint16_t V2_PAGE_ATTR_END = 0x8000;
constexpr uint16_t V3_PAGE_ATTR_NO_REBASE = 0xffff;

}

DyldCache::DyldCache(Memory& mem, const fs::path& main_file) : mem_(mem)
{
    load_file(main_file, true);
    std::string stem = main_file.filename().string();
    std::vector<fs::path> subs;
    for (auto& e : fs::directory_iterator(main_file.parent_path()))
    {
        std::string n = e.path().filename().string();
        if (n.size() <= stem.size() + 1 || !n.starts_with(stem + ".")) continue;
        std::string rest = n.substr(stem.size() + 1);
        size_t digits = 0;
        while (digits < rest.size() && std::isdigit(uint8_t(rest[digits])))
            ++digits;
        if (!digits || (digits < rest.size() && rest[digits] != '.')) continue;
        if (fs::file_size(e.path()) == 0) continue;
        subs.push_back(e.path());
    }
    std::sort(subs.begin(), subs.end());
    for (auto& s : subs)
        load_file(s, false);

    for (auto& m : mappings_)
    {
        const Mapping* mp = &m;
        mem_.map_lazy(m.address, m.size, "dyld_shared_cache",
                      [this, mp](GuestAddr page, uint8_t* host, uint64_t size) { fill_page(*mp, page, host, size); });
    }
}

DyldCache::~DyldCache()
{
    for (auto& f : files_)
        if (f->handle != INVALID_HANDLE_VALUE) CloseHandle(f->handle);
}

void DyldCache::load_file(const fs::path& path, bool main)
{
    auto f = std::make_unique<File>();
    f->path = path;
    f->handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f->handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open " + path.string());
    LARGE_INTEGER size;
    GetFileSizeEx(f->handle, &size);
    f->size = uint64_t(size.QuadPart);

    char magic[16];
    f->read(0, magic, 16);
    if (std::memcmp(magic, "dyld_v1", 7) != 0) throw std::runtime_error(path.string() + " is not a dyld cache");
    if (!std::strstr(magic, "arm64") || std::strstr(magic, "arm64e"))
        throw std::runtime_error(path.string() + ": need a plain arm64 cache (got '" + std::string(magic, 15) + "')");

    uint32_t mapping_offset = f->at<uint32_t>(0x10), mapping_count = f->at<uint32_t>(0x14);
    bool has_slide_mappings = mapping_offset > 0x138;
    if (has_slide_mappings && f->at<uint32_t>(0x13c))
    {
        uint32_t off = f->at<uint32_t>(0x138), n = f->at<uint32_t>(0x13c);
        for (uint32_t i = 0; i < n; ++i)
        {
            uint64_t e = off + uint64_t(i) * 56;
            Mapping m{f->at<uint64_t>(e),      f->at<uint64_t>(e + 8), f->at<uint64_t>(e + 16), f.get(),
                      f->at<uint64_t>(e + 24), f->at<uint64_t>(e + 32)};
            mappings_.push_back(m);
        }
    }
    else
    {
        for (uint32_t i = 0; i < mapping_count; ++i)
        {
            uint64_t e = mapping_offset + uint64_t(i) * 32;
            mappings_.push_back({f->at<uint64_t>(e), f->at<uint64_t>(e + 8), f->at<uint64_t>(e + 16), f.get()});
        }
    }
    for (auto& m : mappings_)
    {
        start_ = std::min(start_, m.address);
        end_ = std::max(end_, m.address + m.size);
        if (m.slide_info_size && !slide_version_) slide_version_ = int(m.file->at<uint32_t>(m.slide_info_offset));
    }

    if (main)
    {
        uint32_t images_offset, images_count;
        if (mapping_offset > 0x1c4)
        {
            images_offset = f->at<uint32_t>(0x1c0);
            images_count = f->at<uint32_t>(0x1c4);
        }
        else
        {
            images_offset = f->at<uint32_t>(0x18);
            images_count = f->at<uint32_t>(0x1c);
        }
        for (uint32_t i = 0; i < images_count; ++i)
        {
            uint64_t e = images_offset + uint64_t(i) * 32;
            Image img;
            img.header = f->at<uint64_t>(e);
            uint32_t path_off = f->at<uint32_t>(e + 24);
            char buf[512];
            f->read(path_off, buf, sizeof buf);
            buf[sizeof buf - 1] = 0;
            img.path = buf;
            image_index_[img.path] = images_.size();
            images_.push_back(std::move(img));
        }
    }
    files_.push_back(std::move(f));
}

std::optional<GuestAddr> DyldCache::image_header(const std::string& install_path) const
{
    auto it = image_index_.find(install_path);
    if (it == image_index_.end()) return std::nullopt;
    return images_[it->second].header;
}

const DyldCache::Mapping* DyldCache::mapping_for(GuestAddr addr) const
{
    for (auto& m : mappings_)
        if (addr >= m.address && addr < m.address + m.size) return &m;
    return nullptr;
}

std::vector<uint8_t> DyldCache::read(GuestAddr addr, uint64_t size) const
{
    std::vector<uint8_t> out(size);
    const Mapping* m = mapping_for(addr);
    if (!m) throw std::runtime_error("dyld cache read outside mappings");
    m->file->read(m->file_offset + (addr - m->address), out.data(), size);
    return out;
}

void DyldCache::fill_page(const Mapping& m, GuestAddr page, uint8_t* host, uint64_t size) const
{
    uint64_t off = page - m.address;
    uint64_t n = std::min<uint64_t>(size, m.size > off ? m.size - off : 0);
    m.file->read(m.file_offset + off, host, n);
    if (!m.slide_info_size) return;
    if (slide_version_ == 2)
        apply_slide_v2(m, page, host);
    else if (slide_version_ == 3)
        apply_slide_v3(m, page, host);
}

void DyldCache::apply_slide_v2(const Mapping& m, GuestAddr page, uint8_t* host) const
{
    const File& f = *m.file;
    uint64_t si = m.slide_info_offset;
    uint32_t page_size = f.at<uint32_t>(si + 4);
    uint32_t starts_off = f.at<uint32_t>(si + 8), starts_count = f.at<uint32_t>(si + 12);
    uint32_t extras_off = f.at<uint32_t>(si + 16);
    uint64_t delta_mask = f.at<uint64_t>(si + 24), value_add = f.at<uint64_t>(si + 32);
    uint64_t value_mask = ~delta_mask;
    unsigned delta_shift = unsigned(std::countr_zero(delta_mask)) - 2;

    auto rebase_chain = [&](uint8_t* base, uint64_t offset) {
        for (;;)
        {
            uint64_t raw;
            std::memcpy(&raw, base + offset, 8);
            uint64_t delta = (raw & delta_mask) >> delta_shift;
            uint64_t value = raw & value_mask;
            if (value) value += value_add;
            std::memcpy(base + offset, &value, 8);
            if (!delta) break;
            offset += delta;
        }
    };

    for (uint64_t sub = 0; sub < layout::kPageSize; sub += page_size)
    {
        uint64_t index = (page + sub - m.address) / page_size;
        if (index >= starts_count) return;
        uint16_t start = f.at<uint16_t>(si + starts_off + index * 2);
        if (start == V2_PAGE_ATTR_NO_REBASE) continue;
        uint8_t* base = host + sub;
        if (start & V2_PAGE_ATTR_EXTRA)
        {
            for (uint32_t j = start & 0x3fff;; ++j)
            {
                uint16_t extra = f.at<uint16_t>(si + extras_off + uint64_t(j) * 2);
                rebase_chain(base, uint64_t(extra & 0x3fff) * 4);
                if (extra & V2_PAGE_ATTR_END) break;
            }
        }
        else
        {
            rebase_chain(base, uint64_t(start) * 4);
        }
    }
}

void DyldCache::apply_slide_v3(const Mapping& m, GuestAddr page, uint8_t* host) const
{
    const File& f = *m.file;
    uint64_t si = m.slide_info_offset;
    uint32_t page_size = f.at<uint32_t>(si + 4), starts_count = f.at<uint32_t>(si + 8);
    uint64_t auth_value_add = f.at<uint64_t>(si + 16);
    for (uint64_t sub = 0; sub < layout::kPageSize; sub += page_size)
    {
        uint64_t index = (page + sub - m.address) / page_size;
        if (index >= starts_count) return;
        uint16_t start = f.at<uint16_t>(si + 24 + index * 2);
        if (start == V3_PAGE_ATTR_NO_REBASE) continue;
        uint8_t* base = host + sub;
        uint64_t offset = start;
        for (;;)
        {
            uint64_t raw;
            std::memcpy(&raw, base + offset, 8);
            uint64_t next = (raw >> 51) & 0x7ff;
            uint64_t value;
            if (raw >> 63)
                value = (raw & 0xffffffff) + auth_value_add;
            else
                value = (raw & 0x7ffffffffffull) | (((raw >> 43) & 0xff) << 56);
            std::memcpy(base + offset, &value, 8);
            if (!next) break;
            offset += next * 8;
        }
    }
}

}
