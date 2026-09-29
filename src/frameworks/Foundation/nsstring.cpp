#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/libSystem/format.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::Class;
using objc::objc;
using objc::SEL;

namespace
{
struct StringData
{
    std::u16string text;
    bool mutable_ = false;
    GuestAddr utf8 = 0;
};

std::recursive_mutex& strings_lock()
{
    static std::recursive_mutex m;
    return m;
}

std::unordered_map<Id, StringData>& strings()
{
    static std::unordered_map<Id, StringData> s;
    return s;
}

constexpr uint64_t kNSUTF8StringEncoding = 4, kNSUTF16StringEncoding = 10, kNSUnicodeStringEncoding = 10;

bool is_constant(Cpu& c, Id obj)
{
    Class* k = objc(c).class_of(obj);
    return k && k->name == "__NSCFConstantString";
}

std::u16string constant_text(Cpu& c, Id obj)
{
    uint32_t flags = c.mem.read<uint32_t>(obj + 8);
    GuestAddr data = c.mem.read<uint64_t>(obj + 16);
    uint64_t len = c.mem.read<uint64_t>(obj + 24);
    std::u16string out;
    if (flags & 0x10)
    {
        out.resize(len);
        if (len) c.mem.read_bytes(data, out.data(), len * 2);
    }
    else
    {
        std::string bytes(len, '\0');
        if (len) c.mem.read_bytes(data, bytes.data(), len);
        out = utf8_to_16(bytes);
    }
    return out;
}

StringData* data_of(Id obj)
{
    std::lock_guard g(strings_lock());
    auto it = strings().find(obj);
    return it == strings().end() ? nullptr : &it->second;
}

std::u16string& mutable_text(Cpu& c, Id self)
{
    StringData* d = data_of(self);
    if (!d)
    {
        auto text = to_utf16(c, self);
        std::lock_guard g(strings_lock());
        strings()[self].text = std::move(text);
        d = data_of(self);
    }
    if (d->utf8)
    {
        c.rt.heap.free(d->utf8);
        d->utf8 = 0;
    }
    return d->text;
}

std::u16string arg_text(Cpu& c, int i)
{
    return to_utf16(c, c.arg(i));
}

Id make(Cpu& c, std::u16string text)
{
    return objc(c).autorelease(c, new_string(c, std::move(text)));
}

void set_text(Cpu& c, Id self, std::u16string text)
{
    std::lock_guard g(strings_lock());
    auto& d = strings()[self];
    d.text = std::move(text);
}

std::u16string format_args(Cpu& c, Id fmt, VarArgs args)
{
    std::string out = guest_format(c, to_utf8(c, fmt), args, [&](uint64_t obj) { return describe(c, obj); });
    return utf8_to_16(out);
}

GuestAddr utf8_buffer(Cpu& c, Id self)
{
    std::string s = to_utf8(c, self);
    GuestAddr buf = c.rt.heap.alloc(s.size() + 1);
    c.mem.write_bytes(buf, s.c_str(), s.size() + 1);
    if (StringData* d = data_of(self))
    {
        if (d->utf8) c.rt.heap.free(d->utf8);
        d->utf8 = buf;
    }
    return buf;
}

double parse_double(const std::string& s)
{
    size_t i = s.find_first_not_of(" \t\n");
    if (i == std::string::npos) return 0;
    return std::strtod(s.c_str() + i, nullptr);
}

int64_t parse_int(const std::string& s)
{
    size_t i = s.find_first_not_of(" \t\n");
    if (i == std::string::npos) return 0;
    return std::strtoll(s.c_str() + i, nullptr, 10);
}

bool is_host_string_class(Class* k)
{
    return k && k->host && (k->name == "NSString" || k->name == "NSMutableString" || k->name == "__NSCFString");
}

std::u16string path_join(std::u16string a, const std::u16string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() != u'/') a += u'/';
    return a + (b.front() == u'/' ? b.substr(1) : b);
}

}

std::u16string utf8_to_16(std::string_view s)
{
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        uint8_t b = uint8_t(s[i]);
        uint32_t cp;
        int extra;
        if (b < 0x80)
            cp = b, extra = 0;
        else if ((b >> 5) == 6)
            cp = b & 0x1f, extra = 1;
        else if ((b >> 4) == 14)
            cp = b & 0x0f, extra = 2;
        else if ((b >> 3) == 30)
            cp = b & 0x07, extra = 3;
        else
        {
            ++i;
            out += u'\uFFFD';
            continue;
        }
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i)
            cp = (cp << 6) | (uint8_t(s[i]) & 0x3f);
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out += char16_t(0xd800 + (cp >> 10));
            out += char16_t(0xdc00 + (cp & 0x3ff));
        }
        else
        {
            out += char16_t(cp);
        }
    }
    return out;
}

std::string utf16_to_8(std::u16string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        uint32_t cp = s[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (cp < 0x80)
            out += char(cp);
        else if (cp < 0x800)
        {
            out += char(0xc0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000)
        {
            out += char(0xe0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
        else
        {
            out += char(0xf0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3f));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

bool is_string(Cpu& c, Id obj)
{
    Class* ns = objc(c).class_named("NSString");
    return obj && objc(c).is_kind_of(obj, ns);
}

std::u16string to_utf16(Cpu& c, Id str)
{
    if (!str) return {};
    if (StringData* d = data_of(str)) return d->text;
    if (is_constant(c, str)) return constant_text(c, str);
    Class* k = objc(c).class_of(str);
    SEL len_sel = objc(c).sel("length");
    GuestAddr len_imp = objc(c).lookup(k, len_sel);
    if (!len_imp || c.rt.hle.is_stub(len_imp)) return {};
    uint64_t n = c.call(len_imp, {str, len_sel});
    std::u16string out;
    SEL at = objc(c).sel("characterAtIndex:");
    for (uint64_t i = 0; i < n && !c.stopped(); ++i)
        out += char16_t(objc(c).send(c, str, at, {i}));
    return out;
}

std::string to_utf8(Cpu& c, Id str)
{
    return utf16_to_8(to_utf16(c, str));
}

Id new_string(Cpu& c, std::u16string text, bool mutable_)
{
    return new_string(c.rt, std::move(text), mutable_);
}

Id new_string(Runtime& rt, std::u16string text, bool mutable_)
{
    Id obj = rt.objc->alloc_instance(rt.objc->host_class("__NSCFString"));
    std::lock_guard g(strings_lock());
    auto& d = strings()[obj];
    d.text = std::move(text);
    d.mutable_ = mutable_;
    return obj;
}

Id string_autoreleased(Cpu& c, std::string_view utf8)
{
    return make(c, utf8_to_16(utf8));
}
Id string16_autoreleased(Cpu& c, std::u16string text)
{
    return make(c, std::move(text));
}

std::string describe(Cpu& c, Id obj)
{
    if (!obj) return "(null)";
    if (is_string(c, obj)) return to_utf8(c, obj);
    Id d = objc(c).send(c, obj, "description");
    return d ? to_utf8(c, d) : "(null)";
}

void register_nsstring(objc::ObjcRuntime& o)
{
    o.define("NSString", "NSObject");
    o.define("NSMutableString", "NSString");
    o.define("__NSCFString", "NSMutableString");
    o.define("__NSCFConstantString", "__NSCFString");
    o.rt.hle.data("___CFConstantStringClassReference", [](Runtime& rt) { return rt.objc->host_class("__NSCFConstantString")->addr; });

    auto alloc = [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        if (is_host_string_class(k))
        {
            Id obj = objc(c).alloc_instance(objc(c).host_class("__NSCFString"));
            std::lock_guard g(strings_lock());
            strings()[obj].mutable_ = k->name == "NSMutableString";
            c.ret(obj);
        }
        else
        {
            c.ret(objc(c).alloc_instance(k));
        }
    };
    o.class_method("NSString", "alloc", alloc);
    o.class_method("NSString", "allocWithZone:", alloc);

    o.class_method("NSString", "string", [](Cpu& c) { c.ret(make(c, {})); });
    o.class_method("NSString", "stringWithString:", [](Cpu& c) { c.ret(make(c, arg_text(c, 2))); });
    o.class_method("NSString",
                   "stringWithUTF8String:", [](Cpu& c) { c.ret(c.arg(2) ? make(c, utf8_to_16(c.mem.read_cstr(c.arg(2)))) : 0); });
    o.class_method("NSString",
                   "stringWithCString:encoding:", [](Cpu& c) { c.ret(c.arg(2) ? make(c, utf8_to_16(c.mem.read_cstr(c.arg(2)))) : 0); });
    o.class_method("NSString", "stringWithFormat:", [](Cpu& c) { c.ret(make(c, format_args(c, c.arg(2), {c.mem, c.sp()}))); });
    o.class_method("NSString", "stringWithCharacters:length:", [](Cpu& c) {
        std::u16string s(c.arg(3), u'\0');
        if (c.arg(3)) c.mem.read_bytes(c.arg(2), s.data(), c.arg(3) * 2);
        c.ret(make(c, std::move(s)));
    });
    o.class_method("NSMutableString", "stringWithCapacity:", [](Cpu& c) { c.ret(objc(c).autorelease(c, new_string(c, {}, true))); });
    o.class_method("NSMutableString", "string", [](Cpu& c) { c.ret(objc(c).autorelease(c, new_string(c, {}, true))); });

    o.method("NSString", "init", [](Cpu& c) { set_text(c, c.arg(0), {}); });
    o.method("NSString", "initWithString:", [](Cpu& c) { set_text(c, c.arg(0), arg_text(c, 2)); });
    o.method("NSString", "initWithUTF8String:", [](Cpu& c) {
        if (!c.arg(2)) return c.ret(0);
        set_text(c, c.arg(0), utf8_to_16(c.mem.read_cstr(c.arg(2))));
    });
    o.method("NSString", "initWithCString:encoding:", [](Cpu& c) {
        if (!c.arg(2)) return c.ret(0);
        set_text(c, c.arg(0), utf8_to_16(c.mem.read_cstr(c.arg(2))));
    });
    o.method("NSString", "initWithFormat:", [](Cpu& c) { set_text(c, c.arg(0), format_args(c, c.arg(2), {c.mem, c.sp()})); });
    o.method("NSString", "initWithFormat:arguments:", [](Cpu& c) { set_text(c, c.arg(0), format_args(c, c.arg(2), {c.mem, c.arg(3)})); });
    o.method("NSString", "initWithBytes:length:encoding:", [](Cpu& c) {
        uint64_t enc = c.arg(4), len = c.arg(3);
        if (enc == kNSUTF16StringEncoding || enc == 0x94000100)
        {
            std::u16string s(len / 2, u'\0');
            if (len) c.mem.read_bytes(c.arg(2), s.data(), len & ~uint64_t(1));
            set_text(c, c.arg(0), std::move(s));
        }
        else
        {
            std::string s(len, '\0');
            if (len) c.mem.read_bytes(c.arg(2), s.data(), len);
            set_text(c, c.arg(0), utf8_to_16(s));
        }
    });
    o.method("NSString", "initWithCharacters:length:", [](Cpu& c) {
        std::u16string s(c.arg(3), u'\0');
        if (c.arg(3)) c.mem.read_bytes(c.arg(2), s.data(), c.arg(3) * 2);
        set_text(c, c.arg(0), std::move(s));
    });
    o.method("NSMutableString", "initWithCapacity:", [](Cpu& c) { set_text(c, c.arg(0), {}); });

    o.method("__NSCFString", "dealloc", [](Cpu& c) {
        if (StringData* d = data_of(c.arg(0)))
        {
            if (d->utf8) c.rt.heap.free(d->utf8);
            std::lock_guard g(strings_lock());
            strings().erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });

    o.method("NSString", "length", [](Cpu& c) { c.ret(to_utf16(c, c.arg(0)).size()); });
    o.add_fallback([](Cpu& c, objc::Class* cls, objc::SEL s) -> GuestAddr {
        if (!cls->is_meta || objc(c).sel_name(objc(c).canonical_sel(s)) != "newTaggedNSStringWithASCIIBytes_:length_:") return 0;
        static GuestAddr stub = c.rt.hle.make_stub("+[__StringStorage newTaggedNSStringWithASCIIBytes_:length_:]", [](Cpu& k) {
            std::u16string text(k.arg(3), u'\0');
            const uint8_t* bytes = k.mem.host(k.arg(2));
            for (size_t i = 0; i < text.size(); ++i)
                text[i] = char16_t(bytes[i]);
            k.ret(new_string(k, std::move(text)));
        });
        return stub;
    });
    for (const char* sel : {"_fastCStringContents:", "_fastCharacterContents", "_fastUTF8StringContents:"})
        o.method("NSString", sel, [](Cpu& c) { c.ret(0); });
    o.method("NSString", "_fastestEncodingInCFStringEncoding", [](Cpu& c) { c.ret(0x08000100); });
    o.method("NSString", "fastestEncoding", [](Cpu& c) { c.ret(4); });
    o.method("NSString", "smallestEncoding", [](Cpu& c) { c.ret(4); });
    o.method("NSString", "characterAtIndex:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        c.ret(c.arg(2) < s.size() ? s[c.arg(2)] : 0);
    });
    o.method("NSString", "getCharacters:range:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        uint64_t loc = c.arg(3), len = c.arg(4);
        if (loc + len <= s.size() && len) c.mem.write_bytes(c.arg(2), s.data() + loc, len * 2);
    });
    o.method("NSString", "UTF8String", [](Cpu& c) { c.ret(utf8_buffer(c, c.arg(0))); });
    o.method("NSString", "fileSystemRepresentation", [](Cpu& c) { c.ret(utf8_buffer(c, c.arg(0))); });
    o.method("NSString", "cStringUsingEncoding:", [](Cpu& c) { c.ret(utf8_buffer(c, c.arg(0))); });
    o.method("NSString", "getCString:maxLength:encoding:", [](Cpu& c) {
        std::string s = to_utf8(c, c.arg(0));
        if (s.size() + 1 > c.arg(3)) return c.ret(0);
        c.mem.write_bytes(c.arg(2), s.c_str(), s.size() + 1);
        c.ret(1);
    });
    o.method("NSString", "lengthOfBytesUsingEncoding:", [](Cpu& c) {
        c.ret(c.arg(2) == kNSUTF16StringEncoding ? to_utf16(c, c.arg(0)).size() * 2 : to_utf8(c, c.arg(0)).size());
    });
    o.method("NSString", "maximumLengthOfBytesUsingEncoding:", [](Cpu& c) { c.ret(to_utf16(c, c.arg(0)).size() * 3); });
    o.method("NSString", "description", [](Cpu& c) {});
    o.method("NSString", "copyWithZone:", [](Cpu& c) {
        StringData* d = data_of(c.arg(0));
        if (d && d->mutable_) return c.ret(new_string(c, d->text));
        c.ret(objc(c).retain(c.arg(0)));
    });
    o.method("NSString", "mutableCopyWithZone:", [](Cpu& c) { c.ret(new_string(c, to_utf16(c, c.arg(0)), true)); });
    o.method("NSString", "isEqualToString:", [](Cpu& c) { c.ret(c.arg(2) && to_utf16(c, c.arg(0)) == arg_text(c, 2)); });
    o.method("NSString", "isEqual:", [](Cpu& c) {
        c.ret(c.arg(0) == c.arg(2) || (is_string(c, c.arg(2)) && to_utf16(c, c.arg(0)) == arg_text(c, 2)));
    });
    o.method("NSString", "hash", [](Cpu& c) {
        uint64_t h = 1469598103934665603ull;
        for (char16_t ch : to_utf16(c, c.arg(0)))
            h = (h ^ ch) * 1099511628211ull;
        c.ret(h);
    });
    o.method("NSString", "compare:", [](Cpu& c) {
        auto a = to_utf16(c, c.arg(0)), b = arg_text(c, 2);
        c.ret(uint64_t(a < b ? -1 : a > b ? 1 : 0));
    });
    o.method("NSString", "hasPrefix:", [](Cpu& c) {
        auto p = arg_text(c, 2);
        c.ret(!p.empty() && to_utf16(c, c.arg(0)).starts_with(p));
    });
    o.method("NSString", "hasSuffix:", [](Cpu& c) {
        auto p = arg_text(c, 2);
        c.ret(!p.empty() && to_utf16(c, c.arg(0)).ends_with(p));
    });
    o.method("NSString", "containsString:", [](Cpu& c) { c.ret(to_utf16(c, c.arg(0)).find(arg_text(c, 2)) != std::u16string::npos); });
    o.method("NSString", "rangeOfString:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        auto pos = s.find(arg_text(c, 2));
        c.set_x(0, pos == std::u16string::npos ? 0x7fffffffffffffffull : pos);
        c.set_x(1, pos == std::u16string::npos ? 0 : arg_text(c, 2).size());
    });
    o.method("NSString", "stringByAppendingString:", [](Cpu& c) { c.ret(make(c, to_utf16(c, c.arg(0)) + arg_text(c, 2))); });
    o.method("NSString",
             "stringByAppendingFormat:", [](Cpu& c) { c.ret(make(c, to_utf16(c, c.arg(0)) + format_args(c, c.arg(2), {c.mem, c.sp()}))); });
    o.method("NSString", "substringFromIndex:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        c.ret(make(c, c.arg(2) <= s.size() ? s.substr(c.arg(2)) : u""));
    });
    o.method("NSString", "substringToIndex:", [](Cpu& c) { c.ret(make(c, to_utf16(c, c.arg(0)).substr(0, c.arg(2)))); });
    o.method("NSString", "substringWithRange:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        c.ret(make(c, c.arg(2) <= s.size() ? s.substr(c.arg(2), c.arg(3)) : u""));
    });
    o.method("NSString", "lowercaseString", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        for (auto& ch : s)
            if (ch < 128) ch = char16_t(std::tolower(ch));
        c.ret(make(c, s));
    });
    o.method("NSString", "uppercaseString", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        for (auto& ch : s)
            if (ch < 128) ch = char16_t(std::toupper(ch));
        c.ret(make(c, s));
    });
    o.method("NSString", "stringByReplacingOccurrencesOfString:withString:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0)), from = arg_text(c, 2), to = arg_text(c, 3);
        if (!from.empty())
            for (size_t p = 0; (p = s.find(from, p)) != std::u16string::npos; p += to.size())
                s.replace(p, from.size(), to);
        c.ret(make(c, s));
    });
    o.method("NSString", "stringByTrimmingCharactersInSet:", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        size_t a = s.find_first_not_of(u" \t\r\n"), b = s.find_last_not_of(u" \t\r\n");
        c.ret(make(c, a == std::u16string::npos ? u"" : s.substr(a, b - a + 1)));
    });
    o.method("NSString", "intValue", [](Cpu& c) { c.ret(uint64_t(int64_t(int32_t(parse_int(to_utf8(c, c.arg(0))))))); });
    o.method("NSString", "integerValue", [](Cpu& c) { c.ret(uint64_t(parse_int(to_utf8(c, c.arg(0))))); });
    o.method("NSString", "longLongValue", [](Cpu& c) { c.ret(uint64_t(parse_int(to_utf8(c, c.arg(0))))); });
    o.method("NSString", "doubleValue", [](Cpu& c) { c.set_d(0, parse_double(to_utf8(c, c.arg(0)))); });
    o.method("NSString", "floatValue", [](Cpu& c) { c.set_s(0, float(parse_double(to_utf8(c, c.arg(0))))); });
    o.method("NSString", "boolValue", [](Cpu& c) {
        std::string s = to_utf8(c, c.arg(0));
        size_t i = s.find_first_not_of(" \t+-0");
        c.ret(i != std::string::npos && std::strchr("YyTt123456789", s[i]) != nullptr);
    });

    o.method("NSString",
             "stringByAppendingPathComponent:", [](Cpu& c) { c.ret(make(c, path_join(to_utf16(c, c.arg(0)), arg_text(c, 2)))); });
    o.method("NSString", "lastPathComponent", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        while (s.size() > 1 && s.back() == u'/')
            s.pop_back();
        auto p = s.rfind(u'/');
        c.ret(make(c, p == std::u16string::npos || s.size() == 1 ? s : s.substr(p + 1)));
    });
    o.method("NSString", "stringByDeletingLastPathComponent", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        auto p = s.rfind(u'/');
        c.ret(make(c, p == std::u16string::npos ? u"" : p == 0 ? u"/" : s.substr(0, p)));
    });
    o.method("NSString", "pathExtension", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        auto slash = s.rfind(u'/'), dot = s.rfind(u'.');
        c.ret(make(c, dot == std::u16string::npos || (slash != std::u16string::npos && dot < slash) ? u"" : s.substr(dot + 1)));
    });
    o.method("NSString", "stringByDeletingPathExtension", [](Cpu& c) {
        auto s = to_utf16(c, c.arg(0));
        auto slash = s.rfind(u'/'), dot = s.rfind(u'.');
        c.ret(make(c, dot == std::u16string::npos || (slash != std::u16string::npos && dot < slash) ? s : s.substr(0, dot)));
    });
    o.method("NSString", "stringByAppendingPathExtension:", [](Cpu& c) { c.ret(make(c, to_utf16(c, c.arg(0)) + u"." + arg_text(c, 2))); });
    o.method("NSString", "stringByStandardizingPath", [](Cpu& c) { c.ret(objc(c).autorelease(c, objc(c).retain(c.arg(0)))); });
    o.method("NSString", "stringByResolvingSymlinksInPath", [](Cpu& c) { c.ret(objc(c).autorelease(c, objc(c).retain(c.arg(0)))); });
    o.method("NSString", "isAbsolutePath", [](Cpu& c) { c.ret(to_utf16(c, c.arg(0)).starts_with(u"/")); });

    o.method("NSMutableString", "appendString:", [](Cpu& c) { mutable_text(c, c.arg(0)) += arg_text(c, 2); });
    o.method("NSMutableString", "appendFormat:", [](Cpu& c) {
        auto add = format_args(c, c.arg(2), {c.mem, c.sp()});
        mutable_text(c, c.arg(0)) += add;
    });
    o.method("NSMutableString", "setString:", [](Cpu& c) { mutable_text(c, c.arg(0)) = arg_text(c, 2); });
    o.method("NSMutableString", "insertString:atIndex:", [](Cpu& c) {
        auto add = arg_text(c, 2);
        auto& s = mutable_text(c, c.arg(0));
        s.insert(std::min<size_t>(c.arg(3), s.size()), add);
    });
    o.method("NSMutableString", "deleteCharactersInRange:", [](Cpu& c) {
        auto& s = mutable_text(c, c.arg(0));
        if (c.arg(2) <= s.size()) s.erase(c.arg(2), c.arg(3));
    });
    o.method("NSMutableString", "replaceCharactersInRange:withString:", [](Cpu& c) {
        auto add = arg_text(c, 4);
        auto& s = mutable_text(c, c.arg(0));
        if (c.arg(2) <= s.size()) s.replace(c.arg(2), c.arg(3), add);
    });
}

}
