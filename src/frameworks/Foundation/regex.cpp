#include <memory>
#include <mutex>
#include <regex>
#include <unordered_map>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/libSystem/blocks.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::objc;

namespace
{
constexpr uint64_t kNotFound = 0x7fffffffffffffffull;

struct Pattern
{
    std::wregex re;
    Id source = 0;
};

std::mutex lock;
std::unordered_map<Id, std::shared_ptr<Pattern>> patterns;
std::unordered_map<Id, std::vector<std::pair<uint64_t, uint64_t>>> results;

std::wstring wide(const std::u16string& s)
{
    return std::wstring(s.begin(), s.end());
}

std::shared_ptr<Pattern> pattern_of(Id obj)
{
    std::lock_guard g(lock);
    auto it = patterns.find(obj);
    return it == patterns.end() ? nullptr : it->second;
}

bool compile(Cpu& c, Id self, Id source, uint64_t options)
{
    auto p = std::make_shared<Pattern>();
    auto flags = std::regex::ECMAScript;
    if (options & 1) flags |= std::regex::icase;
    try
    {
        p->re = std::wregex(wide(to_utf16(c, source)), flags);
    }
    catch (const std::regex_error&)
    {
        return false;
    }
    p->source = objc(c).retain(source);
    std::lock_guard g(lock);
    patterns[self] = p;
    return true;
}

Id make_result(Cpu& c, const std::wsmatch& m, size_t base)
{
    Id r = objc(c).alloc_instance(objc(c).host_class("NSTextCheckingResult"));
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (size_t i = 0; i < m.size(); ++i)
        ranges.emplace_back(m[i].matched ? base + uint64_t(m.position(i)) : kNotFound, m[i].matched ? uint64_t(m.length(i)) : 0);
    std::lock_guard g(lock);
    results[r] = std::move(ranges);
    return objc(c).autorelease(c, r);
}

std::vector<Id> all_matches(Cpu& c, size_t limit = SIZE_MAX)
{
    auto p = pattern_of(c.arg(0));
    std::vector<Id> out;
    if (!p) return out;
    auto text = to_utf16(c, c.arg(2));
    size_t loc = std::min<size_t>(c.arg(4), text.size()), len = std::min<size_t>(c.arg(5), text.size() - loc);
    std::wstring w = wide(text.substr(loc, len));
    for (auto it = std::wsregex_iterator(w.begin(), w.end(), p->re); it != std::wsregex_iterator() && out.size() < limit; ++it)
        out.push_back(make_result(c, *it, loc));
    return out;
}

}

void register_regex(objc::ObjcRuntime& o)
{
    o.define("NSRegularExpression", "NSObject");
    o.define("NSTextCheckingResult", "NSObject");
    o.method("NSRegularExpression", "initWithPattern:options:error:", [](Cpu& c) {
        if (!compile(c, c.arg(0), c.arg(2), c.arg(3)))
        {
            if (c.arg(4)) c.mem.write<uint64_t>(c.arg(4), 0);
            return c.ret(0);
        }
        c.ret(c.arg(0));
    });
    o.class_method("NSRegularExpression", "regularExpressionWithPattern:options:error:", [](Cpu& c) {
        Id r = objc(c).alloc_instance(objc(c).host_class("NSRegularExpression"));
        c.ret(compile(c, r, c.arg(2), c.arg(3)) ? objc(c).autorelease(c, r) : 0);
    });
    o.method("NSRegularExpression", "pattern", [](Cpu& c) {
        auto p = pattern_of(c.arg(0));
        c.ret(p ? p->source : 0);
    });
    o.method("NSRegularExpression", "numberOfMatchesInString:options:range:", [](Cpu& c) { c.ret(all_matches(c).size()); });
    o.method("NSRegularExpression", "matchesInString:options:range:", [](Cpu& c) { c.ret(make_array(c, all_matches(c))); });
    o.method("NSRegularExpression", "firstMatchInString:options:range:", [](Cpu& c) {
        auto m = all_matches(c, 1);
        c.ret(m.empty() ? 0 : m[0]);
    });
    o.method("NSRegularExpression", "rangeOfFirstMatchInString:options:range:", [](Cpu& c) {
        auto m = all_matches(c, 1);
        std::lock_guard g(lock);
        auto r = m.empty() ? std::pair<uint64_t, uint64_t>{kNotFound, 0} : results[m[0]][0];
        c.set_x(0, r.first);
        c.set_x(1, r.second);
    });
    o.method("NSRegularExpression", "enumerateMatchesInString:options:range:usingBlock:", [](Cpu& c) {
        GuestAddr block = c.arg(6), stop = c.rt.heap.calloc(8);
        for (Id m : all_matches(c))
        {
            call_block(c, block, {m, 0, stop});
            if (c.stopped() || c.mem.read<uint8_t>(stop)) break;
        }
        c.rt.heap.free(stop);
    });
    o.method("NSRegularExpression", "stringByReplacingMatchesInString:options:range:withTemplate:", [](Cpu& c) {
        auto p = pattern_of(c.arg(0));
        auto text = to_utf16(c, c.arg(2));
        if (!p) return c.ret(c.arg(2));
        std::wstring w = wide(text), tmpl = wide(to_utf16(c, c.arg(6)));
        std::wstring out = std::regex_replace(w, p->re, tmpl);
        c.ret(string16_autoreleased(c, std::u16string(out.begin(), out.end())));
    });

    o.method("NSTextCheckingResult", "range", [](Cpu& c) {
        std::lock_guard g(lock);
        auto& r = results[c.arg(0)];
        c.set_x(0, r.empty() ? kNotFound : r[0].first);
        c.set_x(1, r.empty() ? 0 : r[0].second);
    });
    o.method("NSTextCheckingResult", "rangeAtIndex:", [](Cpu& c) {
        std::lock_guard g(lock);
        auto& r = results[c.arg(0)];
        bool ok = c.arg(2) < r.size();
        c.set_x(0, ok ? r[c.arg(2)].first : kNotFound);
        c.set_x(1, ok ? r[c.arg(2)].second : 0);
    });
    o.method("NSTextCheckingResult", "numberOfRanges", [](Cpu& c) {
        std::lock_guard g(lock);
        c.ret(results[c.arg(0)].size());
    });
    o.method("NSTextCheckingResult", "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(lock);
            results.erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });
}

}
