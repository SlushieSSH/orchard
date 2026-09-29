#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/memory.h"
#include "loader/macho.h"

namespace orchard
{
struct Runtime;
class Cpu;

struct LoadedImage
{
    std::string name;
    std::filesystem::path path;
    std::vector<uint8_t> file;
    macho::Image macho;
    GuestAddr base = 0;
    GuestAddr end = 0;
    int64_t slide = 0;
    std::unordered_map<std::string, macho::Export> exports;
    std::vector<LoadedImage*> deps;
    std::vector<GuestAddr> initializers;
    bool initialized = false;
    bool inert = false;
    bool from_cache = false;
    std::string install_path;
    std::vector<std::pair<GuestAddr, GuestAddr>> ranges;

    GuestAddr tlv_start = 0;
    uint64_t tlv_init_size = 0;
    uint64_t tlv_total_size = 0;

    std::span<const uint8_t> slice() const { return macho::slice(file, macho); }
};

class Linker
{
public:
    explicit Linker(Runtime& rt);

    void load_bundle(const std::filesystem::path& app);

    void run_initializers(Cpu& cpu, GuestAddr argc, GuestAddr argv, GuestAddr envp, GuestAddr apple);

    LoadedImage& main_image() { return *images_.front(); }
    const std::vector<std::unique_ptr<LoadedImage>>& images() const { return images_; }
    GuestAddr main_entry() const;

    const LoadedImage* image_containing(GuestAddr addr) const;

    std::string symbolicate(GuestAddr addr) const;

    GuestAddr tlv_address(Cpu& cpu, GuestAddr descriptor);

    struct Stats
    {
        size_t rebases = 0;
        size_t binds = 0;
        size_t system_binds = 0;
        size_t unresolved = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    LoadedImage& load_image(const std::filesystem::path& path, GuestAddr& next_base);
    void link(LoadedImage& img);
    void apply_chained_fixups(LoadedImage& img);
    void apply_dyld_info(LoadedImage& img);
    void collect_initializers(LoadedImage& img);
    void setup_tlv(LoadedImage& img);
    void init_image(Cpu& cpu, LoadedImage& img, GuestAddr argc, GuestAddr argv, GuestAddr envp, GuestAddr apple);
    void attach_cache_dylibs();
    std::optional<GuestAddr> resolve_in_cache(const std::string& lib, const std::string& name);

    GuestAddr resolve_import(LoadedImage& img, const macho::Import& imp);
    std::optional<GuestAddr> lookup_export(LoadedImage& img, const std::string& name, int depth = 0);
    LoadedImage* bundled_image(const std::string& install_path);

    Runtime& rt_;
    std::vector<std::unique_ptr<LoadedImage>> images_;
    std::unordered_map<std::string, LoadedImage*> by_name_;
    std::map<GuestAddr, LoadedImage*> by_base_;
    mutable std::map<const LoadedImage*, std::map<uint64_t, std::string>> symbols_;
    std::map<uint64_t, LoadedImage*> tlv_keys_;
    Stats stats_;
    std::vector<std::string> cache_roots_;
};

}
