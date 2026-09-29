#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orchard
{
struct Plist
{
    enum class Kind
    {
        Null,
        Bool,
        Int,
        Real,
        String,
        Data,
        Date,
        Array,
        Dict
    };
    Kind kind = Kind::Null;
    bool b = false;
    int64_t i = 0;
    double r = 0;
    std::string s;
    std::vector<uint8_t> data;
    std::vector<Plist> array;
    std::vector<std::pair<std::string, Plist>> dict;

    const Plist* get(std::string_view key) const
    {
        for (auto& [k, v] : dict)
            if (k == key) return &v;
        return nullptr;
    }
    std::string string_or(std::string_view key, std::string fallback = {}) const
    {
        const Plist* v = get(key);
        return v && v->kind == Kind::String ? v->s : fallback;
    }
};

std::optional<Plist> parse_plist(std::span<const uint8_t> bytes);
std::string write_xml_plist(const Plist& root);

}
