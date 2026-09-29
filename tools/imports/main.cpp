#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "loader/macho.h"

namespace fs = std::filesystem;
using namespace orchard;

namespace
{
struct Binary
{
    std::string name;
    macho::Image image;
};

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

const char* fixup_name(macho::FixupKind k)
{
    switch (k)
    {
    case macho::FixupKind::ChainedFixups: return "chained-fixups";
    case macho::FixupKind::DyldInfo: return "dyld-info";
    default: return "none";
    }
}

std::vector<fs::path> bundle_binaries(const fs::path& app)
{
    std::vector<fs::path> out;
    out.push_back(app / app.stem());
    fs::path frameworks = app / "Frameworks";
    if (!fs::is_directory(frameworks)) return out;

    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(frameworks))
        entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    for (auto& p : entries)
    {
        if (p.extension() == ".framework")
            out.push_back(p / p.stem());
        else if (p.extension() == ".dylib")
            out.push_back(p);
    }
    return out;
}

std::string users_string(const std::set<std::string>& users)
{
    std::string s;
    int shown = 0;
    for (auto& u : users)
    {
        if (shown == 3)
        {
            s += ", +" + std::to_string(users.size() - 3);
            break;
        }
        if (shown++) s += ", ";
        s += u;
    }
    return s;
}

}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: orchard-imports <Name.app> [report.txt]\n");
        return 2;
    }
    fs::path app = fs::path(argv[1]);
    if (!fs::is_directory(app))
    {
        std::fprintf(stderr, "not a directory: %s\n", app.string().c_str());
        return 2;
    }

    std::vector<Binary> binaries;
    int failures = 0;
    for (auto& path : bundle_binaries(app))
    {
        try
        {
            auto data = macho::read_file(path);
            binaries.push_back({path.filename().string(), macho::parse(data)});
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", path.filename().string().c_str(), e.what());
            ++failures;
        }
    }

    std::set<std::string> bundled;
    for (auto& b : binaries)
        bundled.insert(b.name);

    std::map<std::string, std::map<std::string, std::set<std::string>>> system;
    std::map<std::string, std::set<std::string>> missing_bundled;
    std::set<std::string> weak_lookups;

    for (auto& b : binaries)
    {
        for (auto& imp : b.image.imports)
        {
            std::string lib;
            if (imp.ordinal == macho::kOrdinalWeakLookup)
            {
                weak_lookups.insert(imp.name);
                continue;
            }
            if (imp.ordinal == macho::kOrdinalFlatLookup)
                lib = "<flat lookup>";
            else if (imp.ordinal > 0 && size_t(imp.ordinal) <= b.image.dylibs.size())
                lib = canonical_library(b.image.dylibs[imp.ordinal - 1].path);
            else
                continue;

            if (lib.starts_with("<") || is_system_path(lib))
            {
                system[lib][imp.name].insert(b.name);
            }
            else if (!bundled.contains(basename(lib)))
            {
                missing_bundled[lib].insert(b.name);
            }
        }
    }

    size_t total_symbols = 0, total_classes = 0;
    for (auto& [lib, syms] : system)
    {
        total_symbols += syms.size();
        for (auto& [s, _] : syms)
            if (s.starts_with("_OBJC_CLASS_$_")) ++total_classes;
    }

    std::printf("%-36s %-5s %-15s %-6s %-6s %s\n", "binary", "type", "fixups", "crypt", "libs", "imports");
    for (auto& b : binaries)
    {
        const auto& i = b.image;
        std::printf("%-36s %-5s %-15s %-6s %-6zu %zu\n", b.name.c_str(), i.filetype == macho::kFileTypeExecute ? "exe" : "dylib",
                    fixup_name(i.fixups), i.cryptid ? std::to_string(*i.cryptid).c_str() : "-", i.dylibs.size(), i.imports.size());
    }

    std::printf("\n%zu system libraries, %zu distinct symbols (%zu Objective-C classes)\n", system.size(), total_symbols, total_classes);
    std::printf("%zu C++ weak symbols resolved between the app's own images (not listed)\n\n", weak_lookups.size());

    std::vector<std::pair<std::string, size_t>> by_size;
    for (auto& [lib, syms] : system)
        by_size.emplace_back(lib, syms.size());
    std::sort(by_size.begin(), by_size.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [lib, n] : by_size)
        std::printf("  %6zu  %s\n", n, lib.c_str());

    for (auto& [lib, users] : missing_bundled)
        std::printf("MISSING bundled library %s (needed by %s)\n", lib.c_str(), users_string(users).c_str());

    if (argc >= 3)
    {
        std::ofstream out(argv[2]);
        out << "# orchard import report for " << app.filename().string() << "\n";
        out << "# " << total_symbols << " symbols from " << system.size() << " system libraries\n";
        for (auto& [lib, n] : by_size)
        {
            out << "\n== " << lib << "  (" << n << ")\n";
            for (auto& [sym, users] : system[lib])
                out << "  " << sym << "    [" << users_string(users) << "]\n";
        }
        std::printf("\nreport written to %s\n", argv[2]);
    }
    return failures ? 1 : 0;
}
