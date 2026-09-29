#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orchard::macho
{
struct Section
{
    std::string segname;
    std::string sectname;
    uint64_t addr = 0;
    uint64_t size = 0;
    uint32_t offset = 0;
    uint32_t flags = 0;
};

struct Segment
{
    std::string name;
    uint64_t vmaddr = 0;
    uint64_t vmsize = 0;
    uint64_t fileoff = 0;
    uint64_t filesize = 0;
    uint32_t maxprot = 0;
    uint32_t initprot = 0;
    std::vector<Section> sections;
};

enum class DylibKind
{
    Normal,
    Weak,
    Reexport,
    Upward,
    Lazy
};

struct Dylib
{
    std::string path;
    DylibKind kind = DylibKind::Normal;
};

constexpr int kOrdinalSelf = 0;
constexpr int kOrdinalMainExecutable = -1;
constexpr int kOrdinalFlatLookup = -2;
constexpr int kOrdinalWeakLookup = -3;

struct Import
{
    std::string name;
    int ordinal = 0;
    bool weak = false;
    int64_t addend = 0;
};

enum class FixupKind
{
    None,
    ChainedFixups,
    DyldInfo
};

struct LinkeditRange
{
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct Image
{
    uint32_t filetype = 0;
    std::string install_name;
    std::vector<Segment> segments;
    std::vector<Dylib> dylibs;
    std::vector<Import> imports;
    FixupKind fixups = FixupKind::None;
    std::optional<uint32_t> cryptid;
    std::optional<uint64_t> entryoff;
    uint32_t minos = 0;
    uint32_t sdk = 0;

    size_t slice_offset = 0;
    size_t slice_size = 0;
    LinkeditRange chained_fixups;
    LinkeditRange rebase, bind, weak_bind, lazy_bind;
    LinkeditRange exports;
    LinkeditRange symtab;
    LinkeditRange strtab;
};

constexpr uint32_t kFileTypeExecute = 2;
constexpr uint32_t kFileTypeDylib = 6;

constexpr uint32_t kExportKindMask = 0x03;
constexpr uint32_t kExportKindRegular = 0x00;
constexpr uint32_t kExportKindThreadLocal = 0x01;
constexpr uint32_t kExportKindAbsolute = 0x02;
constexpr uint32_t kExportWeakDefinition = 0x04;
constexpr uint32_t kExportReexport = 0x08;
constexpr uint32_t kExportStubAndResolver = 0x10;

struct Export
{
    std::string name;
    uint32_t flags = 0;
    uint64_t address = 0;
    int reexport_ordinal = 0;
    std::string reexport_name;
};

std::vector<uint8_t> read_file(const std::filesystem::path& path);

Image parse(std::span<const uint8_t> file, bool with_imports = true);

std::span<const uint8_t> slice(std::span<const uint8_t> file, const Image& img);

std::vector<Export> parse_exports(std::span<const uint8_t> slice, const Image& img);

struct Symbol
{
    std::string name;
    uint64_t address = 0;
};

std::vector<Symbol> parse_symbols(std::span<const uint8_t> slice, const Image& img);

uint64_t read_uleb(std::span<const uint8_t> data, size_t& off);
int64_t read_sleb(std::span<const uint8_t> data, size_t& off);

std::string version_string(uint32_t packed);

}
