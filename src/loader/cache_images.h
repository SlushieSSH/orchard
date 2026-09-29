#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/memory.h"
#include "loader/dyld_cache.h"
#include "loader/macho.h"

namespace orchard
{
struct Runtime;

class CacheImages
{
public:
    struct Dylib
    {
        std::string path;
        GuestAddr header = 0;
        macho::Image macho;
        bool real = false;
        bool exports_loaded = false;
        std::unordered_map<std::string, macho::Export> exports;
        std::vector<std::pair<GuestAddr, GuestAddr>> text_ranges;
    };

    CacheImages(Runtime& rt, DyldCache& cache);

    DyldCache& cache() { return cache_; }
    Dylib* dylib(const std::string& path);
    Dylib* dylib_containing(GuestAddr addr);

    struct Found
    {
        GuestAddr addr;
        Dylib* owner;
    };
    std::optional<Found> lookup(const std::string& path, const std::string& name, int depth = 0);

    bool is_function(Dylib& d, GuestAddr addr) const;

    void patch_reachable(const std::vector<std::string>& roots);

    std::optional<GuestAddr> patch_target(GuestAddr entry) const;

    const std::vector<Dylib*>& real_dylibs() const { return real_order_; }
    void collect_real(const std::string& root);

    static bool is_real_path(const std::string& path);

    GuestAddr relative_selector_base();

private:
    void load_exports(Dylib& d);

    Runtime& rt_;
    DyldCache& cache_;
    std::unordered_map<std::string, std::unique_ptr<Dylib>> dylibs_;
    std::unordered_map<GuestAddr, GuestAddr> patches_;
    std::unordered_set<std::string> patched_dylibs_;
    std::vector<Dylib*> real_order_;
    std::unordered_set<std::string> real_seen_;
    GuestAddr selector_base_ = 0;
};

}
