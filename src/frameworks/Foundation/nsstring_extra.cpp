#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::objc;

namespace
{
constexpr uint64_t NSCaseInsensitiveSearch = 1, NSBackwardsSearch = 4, NSAnchoredSearch = 8, NSNumericSearch = 64;
constexpr uint64_t kNotFound = 0x7fffffffffffffffull;

std::u16string arg16(Cpu& c, int i)
{
    return to_utf16(c, c.arg(i));
}
Id make16(Cpu& c, std::u16string s)
{
    return string16_autoreleased(c, std::move(s));
}

bool is_digit(char16_t ch)
{
    return ch >= u'0' && ch <= u'9';
}

char16_t fold(char16_t ch)
{
    return ch < 128 ? char16_t(std::tolower(ch)) : ch;
}

int compare(std::u16string a, std::u16string b, uint64_t options)
{
    if (options & NSCaseInsensitiveSearch)
    {
        for (auto& ch : a)
            ch = fold(ch);
        for (auto& ch : b)
            ch = fold(ch);
    }
    if (!(options & NSNumericSearch)) return a < b ? -1 : a > b ? 1 : 0;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        if (is_digit(a[i]) && is_digit(b[j]))
        {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && is_digit(a[i2]))
                ++i2;
            while (j2 < b.size() && is_digit(b[j2]))
                ++j2;
            auto na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
            na.erase(0, std::min(na.find_first_not_of(u'0'), na.size()));
            nb.erase(0, std::min(nb.find_first_not_of(u'0'), nb.size()));
            if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
            if (na != nb) return na < nb ? -1 : 1;
            i = i2;
            j = j2;
        }
        else
        {
            if (a[i] != b[j]) return a[i] < b[j] ? -1 : 1;
            ++i;
            ++j;
        }
    }
    return i < a.size() ? 1 : j < b.size() ? -1 : 0;
}

size_t find_in(const std::u16string& hay, std::u16string needle, uint64_t options, size_t from = 0, size_t len = std::u16string::npos)
{
    std::u16string h = hay.substr(std::min(from, hay.size()), len);
    if (options & NSCaseInsensitiveSearch)
    {
        for (auto& ch : h)
            ch = fold(ch);
        for (auto& ch : needle)
            ch = fold(ch);
    }
    size_t p;
    if (options & NSAnchoredSearch)
        p = (options & NSBackwardsSearch) ? (h.ends_with(needle) ? h.size() - needle.size() : std::u16string::npos)
                                          : (h.starts_with(needle) ? 0 : std::u16string::npos);
    else
        p = (options & NSBackwardsSearch) ? h.rfind(needle) : h.find(needle);
    return p == std::u16string::npos || needle.empty() ? std::u16string::npos : p + std::min(from, hay.size());
}

void ret_range(Cpu& c, size_t pos, size_t len)
{
    c.set_x(0, pos == std::u16string::npos ? kNotFound : pos);
    c.set_x(1, pos == std::u16string::npos ? 0 : len);
}

enum SetKind : uint64_t
{
    kWhitespace = 1,
    kWhitespaceNewline,
    kNewline,
    kDecimalDigit,
    kAlphanumeric,
    kLetter,
    kPunctuation,
    kURLQueryAllowed,
    kURLPathAllowed,
    kURLHostAllowed,
    kURLFragmentAllowed,
    kExplicit
};

bool member(Cpu& c, Id set, char16_t ch)
{
    uint64_t kind = c.mem.read<uint64_t>(set + 8);
    bool alnum = ch < 128 ? std::isalnum(ch) != 0 : ch > 127;
    switch (kind)
    {
    case kWhitespace: return ch == u' ' || ch == u'\t';
    case kWhitespaceNewline: return ch == u' ' || ch == u'\t' || ch == u'\n' || ch == u'\r';
    case kNewline: return ch == u'\n' || ch == u'\r';
    case kDecimalDigit: return ch >= u'0' && ch <= u'9';
    case kAlphanumeric: return alnum;
    case kLetter: return ch < 128 ? std::isalpha(ch) != 0 : ch > 127;
    case kPunctuation: return ch < 128 && std::ispunct(ch);
    case kURLQueryAllowed:
    case kURLFragmentAllowed: return alnum || std::u16string_view(u"-._~!$&'()*+,;=:@/?").find(ch) != std::u16string_view::npos;
    case kURLPathAllowed: return alnum || std::u16string_view(u"-._~!$&'()*+,;=:@/").find(ch) != std::u16string_view::npos;
    case kURLHostAllowed: return alnum || std::u16string_view(u"-._~!$&'()*+,;=:[]").find(ch) != std::u16string_view::npos;
    case kExplicit: return to_utf16(c, c.mem.read<uint64_t>(set + 16)).find(ch) != std::u16string::npos;
    default: return false;
    }
}

Id char_set(Cpu& c, uint64_t kind, Id explicit_chars = 0)
{
    Id s = objc(c).alloc_instance(objc(c).host_class("NSCharacterSet"), 16);
    c.mem.write<uint64_t>(s + 8, kind);
    c.mem.write<uint64_t>(s + 16, explicit_chars ? objc(c).retain(explicit_chars) : 0);
    return objc(c).autorelease(c, s);
}

std::optional<std::filesystem::path> host_path(Cpu& c, Id path)
{
    return c.rt.vfs.to_host(to_utf8(c, path));
}

void register_comparison(objc::ObjcRuntime& o)
{
    o.method("NSString",
             "compare:options:", [](Cpu& c) { c.ret(uint64_t(int64_t(compare(to_utf16(c, c.arg(0)), arg16(c, 2), c.arg(3))))); });
    o.method("NSString", "compare:options:range:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0)).substr(std::min<uint64_t>(c.arg(4), UINT32_MAX), c.arg(5));
        c.ret(uint64_t(int64_t(compare(s, arg16(c, 2), c.arg(3)))));
    });
    o.method("NSString",
             "caseInsensitiveCompare:", [](Cpu& c) { c.ret(uint64_t(int64_t(compare(to_utf16(c, c.arg(0)), arg16(c, 2), 1)))); });
    o.method("NSString", "localizedCompare:", [](Cpu& c) { c.ret(uint64_t(int64_t(compare(to_utf16(c, c.arg(0)), arg16(c, 2), 0)))); });
    o.method("NSString",
             "localizedCaseInsensitiveCompare:", [](Cpu& c) { c.ret(uint64_t(int64_t(compare(to_utf16(c, c.arg(0)), arg16(c, 2), 1)))); });
    o.method("NSString",
             "localizedStandardCompare:", [](Cpu& c) { c.ret(uint64_t(int64_t(compare(to_utf16(c, c.arg(0)), arg16(c, 2), 65)))); });
    o.method("NSString", "rangeOfString:options:", [](Cpu& c) {
        auto n = arg16(c, 2);
        ret_range(c, find_in(to_utf16(c, c.arg(0)), n, c.arg(3)), n.size());
    });
    o.method("NSString", "rangeOfString:options:range:", [](Cpu& c) {
        auto n = arg16(c, 2);
        ret_range(c, find_in(to_utf16(c, c.arg(0)), n, c.arg(3), c.arg(4), c.arg(5)), n.size());
    });
    o.method("NSString", "localizedCaseInsensitiveContainsString:", [](Cpu& c) {
        c.ret(find_in(to_utf16(c, c.arg(0)), arg16(c, 2), 1) != std::u16string::npos);
    });
    o.method("NSString", "rangeOfCharacterFromSet:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        for (size_t i = 0; i < s.size(); ++i)
            if (member(c, c.arg(2), s[i])) return ret_range(c, i, 1);
        ret_range(c, std::u16string::npos, 0);
    });
}

void register_transforms(objc::ObjcRuntime& o)
{
    o.method("NSString", "componentsSeparatedByString:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0)), sep = arg16(c, 2);
        std::vector<Id> parts;
        size_t start = 0;
        for (size_t p; !sep.empty() && (p = s.find(sep, start)) != std::u16string::npos; start = p + sep.size())
            parts.push_back(make16(c, s.substr(start, p - start)));
        parts.push_back(make16(c, s.substr(start)));
        c.ret(make_array(c, parts));
    });
    o.method("NSString", "componentsSeparatedByCharactersInSet:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        std::vector<Id> parts;
        std::u16string cur;
        for (char16_t ch : s)
        {
            if (member(c, c.arg(2), ch))
            {
                parts.push_back(make16(c, cur));
                cur.clear();
            }
            else
            {
                cur += ch;
            }
        }
        parts.push_back(make16(c, cur));
        c.ret(make_array(c, parts));
    });
    o.method("NSString", "stringByTrimmingCharactersInSet:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        size_t a = 0, b = s.size();
        while (a < b && member(c, c.arg(2), s[a]))
            ++a;
        while (b > a && member(c, c.arg(2), s[b - 1]))
            --b;
        c.ret(make16(c, s.substr(a, b - a)));
    });
    o.method("NSString", "capitalizedString", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        bool start = true;
        for (auto& ch : s)
        {
            if (ch < 128 && std::isalpha(ch))
            {
                ch = char16_t(start ? std::toupper(ch) : std::tolower(ch));
                start = false;
            }
            else
            {
                start = true;
            }
        }
        c.ret(make16(c, s));
    });
    o.method("NSString", "lowercaseStringWithLocale:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "lowercaseString")); });
    o.method("NSString", "uppercaseStringWithLocale:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "uppercaseString")); });
    o.method("NSString", "stringByReplacingCharactersInRange:withString:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        if (c.arg(2) <= s.size()) s.replace(c.arg(2), c.arg(3), arg16(c, 4));
        c.ret(make16(c, s));
    });
    o.method("NSString", "stringByReplacingOccurrencesOfString:withString:options:range:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0)), from = arg16(c, 2), to = arg16(c, 3);
        if (!from.empty())
            for (size_t p = 0; (p = s.find(from, p)) != std::u16string::npos; p += to.size())
                s.replace(p, from.size(), to);
        c.ret(make16(c, s));
    });
    o.method("NSString", "stringByPaddingToLength:withString:startingAtIndex:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0)), pad = arg16(c, 3);
        size_t n = c.arg(2);
        if (s.size() > n) s.resize(n);
        for (size_t i = c.arg(4); s.size() < n && !pad.empty(); ++i)
            s += pad[i % pad.size()];
        c.ret(make16(c, s));
    });
    o.method("NSString", "pathComponents", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        std::vector<Id> parts;
        if (s.starts_with(u"/")) parts.push_back(make16(c, u"/"));
        size_t start = 0;
        while (start < s.size())
        {
            size_t p = s.find(u'/', start);
            if (p == std::u16string::npos) p = s.size();
            if (p > start) parts.push_back(make16(c, s.substr(start, p - start)));
            start = p + 1;
        }
        c.ret(make_array(c, parts));
    });
    o.method("NSString", "stringByAddingPercentEncodingWithAllowedCharacters:", [](Cpu& c) {
        std::string s = to_utf8(c, c.arg(0)), out;
        static const char* hex = "0123456789ABCDEF";
        for (unsigned char ch : s)
        {
            if (ch < 128 && member(c, c.arg(2), ch))
            {
                out += char(ch);
            }
            else
            {
                out += '%';
                out += hex[ch >> 4];
                out += hex[ch & 15];
            }
        }
        c.ret(string_autoreleased(c, out));
    });
    o.method("NSString", "stringByRemovingPercentEncoding", [](Cpu& c) {
        std::string s = to_utf8(c, c.arg(0)), out;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(uint8_t(s[i + 1])) && std::isxdigit(uint8_t(s[i + 2])))
            {
                out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            }
            else
            {
                out += s[i];
            }
        }
        c.ret(string_autoreleased(c, out));
    });
    for (const char* sel : {"precomposedStringWithCanonicalMapping", "decomposedStringWithCanonicalMapping", "stringByExpandingTildeInPath",
                            "stringByAbbreviatingWithTildeInPath"})
        o.method("NSString", sel, [](Cpu& c) { c.ret(objc(c).autorelease(c, objc(c).retain(c.arg(0)))); });
    o.method("NSString", "unsignedLongLongValue", [](Cpu& c) { c.ret(std::strtoull(to_utf8(c, c.arg(0)).c_str(), nullptr, 10)); });
    o.method("NSString", "canBeConvertedToEncoding:", [](Cpu& c) { c.ret(1); });
    o.method("NSString", "fastestEncoding", [](Cpu& c) { c.ret(4); });
    o.method("NSString", "smallestEncoding", [](Cpu& c) { c.ret(4); });
}

void register_encoding_and_files(objc::ObjcRuntime& o)
{
    o.method("NSString", "dataUsingEncoding:", [](Cpu& c) {
        if (c.arg(2) == 10 || c.arg(2) == 0x94000100)
        {
            auto s = to_utf16(c, c.arg(0));
            std::vector<uint8_t> b(s.size() * 2);
            std::memcpy(b.data(), s.data(), b.size());
            return c.ret(make_data(c, std::move(b)));
        }
        std::string s = to_utf8(c, c.arg(0));
        c.ret(make_data(c, std::vector<uint8_t>(s.begin(), s.end())));
    });
    o.method("NSString",
             "dataUsingEncoding:allowLossyConversion:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "dataUsingEncoding:", {c.arg(2)})); });
    o.method("NSString", "initWithBytesNoCopy:length:encoding:freeWhenDone:", [](Cpu& c) {
        Id s = objc(c).send(c, c.arg(0), "initWithBytes:length:encoding:", {c.arg(2), c.arg(3), c.arg(4)});
        if (c.arg(5) & 1) c.rt.heap.free(c.arg(2));
        c.ret(s);
    });
    o.method("NSString", "initWithData:encoding:", [](Cpu& c) {
        Id data = c.arg(2);
        uint64_t len = objc(c).send(c, data, "length");
        GuestAddr bytes = objc(c).send(c, data, "bytes");
        Id s = objc(c).send(c, c.arg(0), "initWithBytes:length:encoding:", {bytes, len, c.arg(3)});
        c.ret(s);
    });
    auto read_file = [](Cpu& c, Id path, std::string& out) {
        auto host = host_path(c, path);
        std::ifstream f(host ? *host : std::filesystem::path(), std::ios::binary);
        if (!host || !f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    static decltype(read_file) s_read = read_file;
    o.class_method("NSString", "stringWithContentsOfFile:encoding:error:", [](Cpu& c) {
        std::string s;
        c.ret(s_read(c, c.arg(2), s) ? string_autoreleased(c, s) : 0);
    });
    o.class_method("NSString", "stringWithContentsOfFile:usedEncoding:error:", [](Cpu& c) {
        std::string s;
        if (c.arg(3)) c.mem.write<uint64_t>(c.arg(3), 4);
        c.ret(s_read(c, c.arg(2), s) ? string_autoreleased(c, s) : 0);
    });
    o.method("NSString", "initWithContentsOfFile:encoding:error:", [](Cpu& c) {
        std::string s;
        if (!s_read(c, c.arg(2), s)) return c.ret(0);
        c.ret(objc(c).retain(string_autoreleased(c, s)));
    });
    o.method("NSString", "writeToFile:atomically:encoding:error:", [](Cpu& c) {
        auto host = host_path(c, c.arg(2));
        if (!host) return c.ret(0);
        std::string s = to_utf8(c, c.arg(0));
        std::ofstream(*host, std::ios::binary) << s;
        c.ret(1);
    });
}

void register_character_sets(objc::ObjcRuntime& o)
{
    o.define("NSCharacterSet", "NSObject");
    o.define("NSMutableCharacterSet", "NSCharacterSet");
    struct Named
    {
        const char* sel;
        SetKind kind;
    };
    static const Named kSets[] = {{"whitespaceCharacterSet", kWhitespace},
                                  {"whitespaceAndNewlineCharacterSet", kWhitespaceNewline},
                                  {"newlineCharacterSet", kNewline},
                                  {"decimalDigitCharacterSet", kDecimalDigit},
                                  {"alphanumericCharacterSet", kAlphanumeric},
                                  {"letterCharacterSet", kLetter},
                                  {"punctuationCharacterSet", kPunctuation},
                                  {"URLQueryAllowedCharacterSet", kURLQueryAllowed},
                                  {"URLPathAllowedCharacterSet", kURLPathAllowed},
                                  {"URLHostAllowedCharacterSet", kURLHostAllowed},
                                  {"URLFragmentAllowedCharacterSet", kURLFragmentAllowed}};
    for (auto& n : kSets)
    {
        o.class_method("NSCharacterSet", n.sel, [](Cpu& c) {
            std::string sel = objc(c).sel_name(c.arg(1));
            for (auto& m : kSets)
                if (sel == m.sel) return c.ret(char_set(c, m.kind));
            c.ret(0);
        });
    }
    o.class_method("NSCharacterSet", "characterSetWithCharactersInString:", [](Cpu& c) { c.ret(char_set(c, kExplicit, c.arg(2))); });
    o.method("NSCharacterSet", "characterIsMember:", [](Cpu& c) { c.ret(member(c, c.arg(0), char16_t(c.arg(2)))); });
    o.method("NSCharacterSet", "invertedSet", [](Cpu& c) { c.ret(c.arg(0)); });
    o.method("NSCharacterSet", "mutableCopyWithZone:", [](Cpu& c) { c.ret(objc(c).retain(c.arg(0))); });
    o.method("NSCharacterSet", "copyWithZone:", [](Cpu& c) { c.ret(objc(c).retain(c.arg(0))); });
    for (const char* sel : {"addCharactersInString:", "removeCharactersInString:", "formUnionWithCharacterSet:"})
        o.method("NSCharacterSet", sel, [](Cpu& c) {});
}

}

void register_nsstring_extra(objc::ObjcRuntime& o)
{
    register_comparison(o);
    register_transforms(o);
    register_encoding_and_files(o);
    register_character_sets(o);
}

}
