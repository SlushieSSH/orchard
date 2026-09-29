#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/memory.h"

namespace orchard
{
class DyldCache
{
public:
    struct Image
    {
        std::string path;
        GuestAddr header = 0;
    };

    DyldCache(Memory& mem, const std::filesystem::path& main_file);
    ~DyldCache();

    const std::vector<Image>& images() const { return images_; }
    std::optional<GuestAddr> image_header(const std::string& install_path) const;
    bool contains(GuestAddr addr) const { return addr >= start_ && addr < end_; }
    GuestAddr start() const { return start_; }
    GuestAddr end() const { return end_; }
    int slide_version() const { return slide_version_; }

    std::vector<uint8_t> read(GuestAddr addr, uint64_t size) const;

private:
    struct File;
    struct Mapping
    {
        GuestAddr address;
        uint64_t size;
        uint64_t file_offset;
        File* file;
        uint64_t slide_info_offset = 0;
        uint64_t slide_info_size = 0;
    };

    void load_file(const std::filesystem::path& path, bool main);
    void fill_page(const Mapping& m, GuestAddr page, uint8_t* host, uint64_t size) const;
    void apply_slide_v2(const Mapping& m, GuestAddr page, uint8_t* host) const;
    void apply_slide_v3(const Mapping& m, GuestAddr page, uint8_t* host) const;
    const Mapping* mapping_for(GuestAddr addr) const;

    Memory& mem_;
    std::vector<std::unique_ptr<File>> files_;
    std::vector<Mapping> mappings_;
    std::vector<Image> images_;
    std::unordered_map<std::string, size_t> image_index_;
    GuestAddr start_ = ~GuestAddr(0), end_ = 0;
    int slide_version_ = 0;
};

}
