#include <chrono>
#include <cstring>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/libSystem/format.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard::cf
{
using foundation::Id;
using objc::objc;

namespace
{
Id cls(Cpu& c, const char* name)
{
    return objc(c).host_class(name)->addr;
}
Id send(Cpu& c, Id o, const char* sel, std::initializer_list<uint64_t> args = {})
{
    return objc(c).send(c, o, sel, args);
}
Id owned(Cpu& c, Id autoreleased)
{
    return objc(c).retain(autoreleased);
}
std::u16string text(Cpu& c, Id s)
{
    return foundation::to_utf16(c, s);
}

double reference_now()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - 978307200.0;
}

std::string percent_encode(const std::string& s, const std::string& leave)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s)
    {
        if (std::isalnum(ch) || std::strchr("-._~", ch) || leave.find(char(ch)) != std::string::npos)
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
    return out;
}

std::string percent_decode(const std::string& s)
{
    std::string out;
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
    return out;
}

Id format_string(Cpu& c, Id fmt, VarArgs args)
{
    std::string out = guest_format(c, foundation::to_utf8(c, fmt), args, [&](uint64_t o) { return foundation::describe(c, o); });
    return foundation::new_string(c, foundation::utf8_to_16(out));
}

Id new_uuid(Cpu& c, const uint8_t* bytes)
{
    Id u = objc(c).alloc_instance(objc(c).host_class("__NSCFUUID"), 16);
    c.mem.write_bytes(u + 8, bytes, 16);
    return u;
}

void register_basics(Hle& h)
{
    h.fn("_CFRetain", [](Cpu& c) { c.ret(objc(c).retain_entry(c, c.arg(0))); });
    h.fn("_CFRelease", [](Cpu& c) { objc(c).release_entry(c, c.arg(0)); });
    h.fn("_CFAutorelease", [](Cpu& c) { c.ret(objc(c).autorelease(c, c.arg(0))); });
    h.fn("_CFAbsoluteTimeGetCurrent", [](Cpu& c) { c.set_d(0, reference_now()); });
    h.data("_kCFAbsoluteTimeIntervalSince1970", [](Runtime& rt) {
        GuestAddr a = rt.mem.alloc_system(8, 8);
        rt.mem.write<double>(a, 978307200.0);
        return a;
    });
    h.fn("_CFBooleanGetTypeID", [](Cpu& c) { c.ret(21); });
    h.fn("_CFGetTypeID", [](Cpu& c) {
        Id o = c.arg(0);
        foundation::NumberData n;
        if (foundation::number_of(c, o, n)) return c.ret(n.type == 'B' ? 21 : 22);
        if (foundation::is_string(c, o)) return c.ret(7);
        auto kind = [&](const char* k) { return objc(c).is_kind_of(o, objc(c).class_named(k)); };
        if (kind("NSArray")) return c.ret(19);
        if (kind("NSDictionary")) return c.ret(18);
        if (kind("NSData")) return c.ret(20);
        if (kind("NSDate")) return c.ret(42);
        c.ret(1);
    });
    auto boolean = [](Runtime& rt, bool v) {
        GuestAddr obj = rt.mem.alloc_system(16, 16);
        rt.mem.write<uint64_t>(obj, rt.objc->host_class("__NSCFBoolean")->addr);
        auto& n = foundation::store().get<foundation::NumberData>(obj);
        n.type = 'B';
        n.i = v;
        return obj;
    };
    static decltype(boolean) s_bool = boolean;
    h.data("___kCFBooleanTrue", [](Runtime& rt) { return s_bool(rt, true); });
    h.data("___kCFBooleanFalse", [](Runtime& rt) { return s_bool(rt, false); });
    h.data("_kCFBooleanTrue", [](Runtime& rt) {
        GuestAddr var = rt.mem.alloc_system(8, 8);
        rt.mem.write<uint64_t>(var, rt.hle.resolve("___kCFBooleanTrue", "CoreFoundation"));
        return var;
    });
    h.data("_kCFBooleanFalse", [](Runtime& rt) {
        GuestAddr var = rt.mem.alloc_system(8, 8);
        rt.mem.write<uint64_t>(var, rt.hle.resolve("___kCFBooleanFalse", "CoreFoundation"));
        return var;
    });
    h.fn("_CFBooleanGetValue", [](Cpu& c) {
        foundation::NumberData n;
        c.ret(foundation::number_of(c, c.arg(0), n) && n.as_int() != 0);
    });
    h.fn("_CFNumberGetType", [](Cpu& c) {
        foundation::NumberData n;
        foundation::number_of(c, c.arg(0), n);
        c.ret(n.type == 'd' ? 13 : n.type == 'f' ? 12 : n.type == 'i' ? 9 : 11);
    });
    h.fn("_CFNumberIsFloatType", [](Cpu& c) {
        foundation::NumberData n;
        c.ret(foundation::number_of(c, c.arg(0), n) && n.is_float());
    });
}

void register_collections(Hle& h)
{
    h.fn("_CFArrayCreateMutable", [](Cpu& c) { c.ret(owned(c, foundation::make_array(c, {}, true))); });
    h.fn("_CFArrayAppendValue", [](Cpu& c) { send(c, c.arg(0), "addObject:", {c.arg(1)}); });
    h.fn("_CFArrayGetCount", [](Cpu& c) { c.ret(foundation::array_items(c, c.arg(0)).size()); });
    h.fn("_CFArrayGetValueAtIndex", [](Cpu& c) {
        auto items = foundation::array_items(c, c.arg(0));
        c.ret(c.arg(1) < items.size() ? items[c.arg(1)] : 0);
    });
    h.fn("_CFDictionaryGetValue", [](Cpu& c) { c.ret(foundation::dict_lookup(c, c.arg(0), c.arg(1))); });
    h.fn("_CFDictionaryGetValueIfPresent", [](Cpu& c) {
        Id v = foundation::dict_lookup(c, c.arg(0), c.arg(1));
        if (v && c.arg(2)) c.mem.write<uint64_t>(c.arg(2), v);
        c.ret(v != 0);
    });
    h.fn("_CFDictionaryGetCount", [](Cpu& c) { c.ret(foundation::dict_entries(c, c.arg(0)).size()); });
    h.fn("_CFDataCreateWithBytesNoCopy", [](Cpu& c) {
        std::vector<uint8_t> b(c.arg(2));
        if (!b.empty()) c.mem.read_bytes(c.arg(1), b.data(), b.size());
        c.ret(owned(c, foundation::make_data(c, std::move(b))));
    });
    h.fn("_CFDataCreate", [](Cpu& c) {
        std::vector<uint8_t> b(c.arg(2));
        if (!b.empty()) c.mem.read_bytes(c.arg(1), b.data(), b.size());
        c.ret(owned(c, foundation::make_data(c, std::move(b))));
    });
    h.fn("_CFDataGetLength", [](Cpu& c) { c.ret(send(c, c.arg(0), "length")); });
    h.fn("_CFDataGetBytePtr", [](Cpu& c) { c.ret(send(c, c.arg(0), "bytes")); });
    h.fn("_CFDataGetMutableBytePtr", [](Cpu& c) { c.ret(send(c, c.arg(0), "mutableBytes")); });
}

void register_strings(Hle& h)
{
    h.fn("_CFStringGetLength", [](Cpu& c) { c.ret(text(c, c.arg(0)).size()); });
    h.fn("_CFStringGetCharacterAtIndex", [](Cpu& c) {
        auto t = text(c, c.arg(0));
        c.ret(c.arg(1) < t.size() ? t[c.arg(1)] : 0);
    });
    h.fn("_CFStringGetCString", [](Cpu& c) {
        std::string s = foundation::to_utf8(c, c.arg(0));
        if (s.size() + 1 > c.arg(2)) return c.ret(0);
        c.mem.write_bytes(c.arg(1), s.c_str(), s.size() + 1);
        c.ret(1);
    });
    h.fn("_CFStringGetCStringPtr", [](Cpu& c) { c.ret(0); });
    h.fn("_CFStringGetMaximumSizeForEncoding", [](Cpu& c) { c.ret(c.arg(0) * 3 + 1); });
    h.fn("_CFStringGetBytes", [](Cpu& c) {
        auto t = text(c, c.arg(0));
        uint64_t loc = std::min<uint64_t>(c.arg(1), t.size()), len = std::min<uint64_t>(c.arg(2), t.size() - loc);
        std::string s = foundation::utf16_to_8(t.substr(loc, len));
        uint64_t cap = c.arg(6) ? c.arg(7) : s.size();
        uint64_t n = std::min<uint64_t>(cap, s.size());
        if (c.arg(6) && n) c.mem.write_bytes(c.arg(6), s.data(), n);
        GuestAddr used = c.mem.read<uint64_t>(c.sp());
        if (used) c.mem.write<uint64_t>(used, n);
        c.ret(len);
    });
    h.fn("_CFStringCreateWithCString",
         [](Cpu& c) { c.ret(c.arg(1) ? foundation::new_string(c, foundation::utf8_to_16(c.mem.read_cstr(c.arg(1)))) : 0); });
    h.fn("_CFStringCreateWithCharacters", [](Cpu& c) {
        std::u16string s(c.arg(2), u'\0');
        if (c.arg(2)) c.mem.read_bytes(c.arg(1), s.data(), c.arg(2) * 2);
        c.ret(foundation::new_string(c, std::move(s)));
    });
    h.fn("_CFStringCreateCopy", [](Cpu& c) { c.ret(send(c, c.arg(1), "copy")); });
    h.fn("_CFStringCreateMutable", [](Cpu& c) { c.ret(foundation::new_string(c, {}, true)); });
    h.fn("_CFStringCreateMutableCopy", [](Cpu& c) { c.ret(foundation::new_string(c, text(c, c.arg(2)), true)); });
    h.fn("_CFStringCreateWithSubstring", [](Cpu& c) {
        auto t = text(c, c.arg(1));
        uint64_t loc = std::min<uint64_t>(c.arg(2), t.size());
        c.ret(foundation::new_string(c, t.substr(loc, c.arg(3))));
    });
    h.fn("_CFStringCreateWithFormat", [](Cpu& c) { c.ret(format_string(c, c.arg(2), {c.mem, c.sp()})); });
    h.fn("_CFStringCreateWithFormatAndArguments", [](Cpu& c) { c.ret(format_string(c, c.arg(2), {c.mem, c.arg(3)})); });
    h.fn("_CFStringAppend", [](Cpu& c) { send(c, c.arg(0), "appendString:", {c.arg(1)}); });
    h.fn("_CFStringAppendCString",
         [](Cpu& c) { send(c, c.arg(0), "appendString:", {foundation::string_autoreleased(c, c.mem.read_cstr(c.arg(1)))}); });
    h.fn("_CFStringAppendFormat", [](Cpu& c) {
        Id s = format_string(c, c.arg(2), {c.mem, c.sp()});
        send(c, c.arg(0), "appendString:", {s});
        objc(c).release(c, s);
    });
    h.fn("_CFStringHasPrefix", [](Cpu& c) { c.ret(text(c, c.arg(0)).starts_with(text(c, c.arg(1)))); });
    h.fn("_CFStringHasSuffix", [](Cpu& c) { c.ret(text(c, c.arg(0)).ends_with(text(c, c.arg(1)))); });
    h.fn("_CFStringCompare", [](Cpu& c) {
        auto a = text(c, c.arg(0)), b = text(c, c.arg(1));
        c.ret(uint64_t(a < b ? -1 : a > b ? 1 : 0));
    });
    h.fn("_CFStringTransform", [](Cpu& c) { c.ret(0); });
    h.fn("_CFStringConvertNSStringEncodingToEncoding", [](Cpu& c) { c.ret(c.arg(0) == 4 ? 0x08000100 : c.arg(0)); });
    h.fn("_CFStringConvertEncodingToIANACharSetName", [](Cpu& c) { c.ret(foundation::string_autoreleased(c, "utf-8")); });
    h.fn("_CFURLCreateStringByAddingPercentEscapes", [](Cpu& c) {
        std::string leave = c.arg(2) ? foundation::to_utf8(c, c.arg(2)) : "";
        c.ret(foundation::new_string(c, foundation::utf8_to_16(percent_encode(foundation::to_utf8(c, c.arg(1)), leave))));
    });
    auto decode = [](Cpu& c) {
        c.ret(foundation::new_string(c, foundation::utf8_to_16(percent_decode(foundation::to_utf8(c, c.arg(1))))));
    };
    h.fn("_CFURLCreateStringByReplacingPercentEscapes", decode);
    h.fn("_CFURLCreateStringByReplacingPercentEscapesUsingEncoding", decode);
    h.fn("_CFURLGetFileSystemRepresentation", [](Cpu& c) {
        std::string p = foundation::url_string_path(c, c.arg(0));
        if (p.size() + 1 > c.arg(3)) return c.ret(0);
        c.mem.write_bytes(c.arg(2), p.c_str(), p.size() + 1);
        c.ret(1);
    });
}

void register_misc(Hle& h)
{
    h.fn("_CFBundleGetMainBundle", [](Cpu& c) { c.ret(send(c, cls(c, "NSBundle"), "mainBundle")); });
    h.fn("_CFBundleGetInfoDictionary", [](Cpu& c) { c.ret(send(c, c.arg(0), "infoDictionary")); });
    h.fn("_CFBundleGetIdentifier", [](Cpu& c) { c.ret(send(c, c.arg(0), "bundleIdentifier")); });
    h.fn("_CFBundleGetValueForInfoDictionaryKey", [](Cpu& c) { c.ret(send(c, c.arg(0), "objectForInfoDictionaryKey:", {c.arg(1)})); });
    h.fn("_CFLocaleCopyCurrent", [](Cpu& c) { c.ret(owned(c, send(c, cls(c, "NSLocale"), "currentLocale"))); });
    h.fn("_CFLocaleGetIdentifier", [](Cpu& c) { c.ret(send(c, c.arg(0), "localeIdentifier")); });
    h.fn("_CFLocaleGetValue", [](Cpu& c) { c.ret(send(c, c.arg(0), "objectForKey:", {c.arg(1)})); });
    h.fn("_CFNotificationCenterGetLocalCenter", [](Cpu& c) { c.ret(send(c, cls(c, "NSNotificationCenter"), "defaultCenter")); });
    h.fn("_CFNotificationCenterGetDarwinNotifyCenter", [](Cpu& c) { c.ret(send(c, cls(c, "NSNotificationCenter"), "defaultCenter")); });
    for (const char* n :
         {"_CFNotificationCenterAddObserver", "_CFNotificationCenterRemoveObserver", "_CFNotificationCenterRemoveEveryObserver",
          "_CFNotificationCenterPostNotification", "_CFPreferencesRemoveSuitePreferencesFromApp"})
        h.fn(n, [](Cpu& c) {});
    h.fn("_CFRunLoopCopyAllModes",
         [](Cpu& c) { c.ret(owned(c, foundation::make_array(c, {foundation::string_autoreleased(c, "kCFRunLoopDefaultMode")}))); });

    auto defaults = [](Cpu& c) { return send(c, cls(c, "NSUserDefaults"), "standardUserDefaults"); };
    static decltype(defaults) s_defaults = defaults;
    h.fn("_CFPreferencesCopyAppValue", [](Cpu& c) { c.ret(objc(c).retain(send(c, s_defaults(c), "objectForKey:", {c.arg(0)}))); });
    h.fn("_CFPreferencesSetAppValue", [](Cpu& c) {
        if (c.arg(1))
            send(c, s_defaults(c), "setObject:forKey:", {c.arg(1), c.arg(0)});
        else
            send(c, s_defaults(c), "removeObjectForKey:", {c.arg(0)});
    });
    h.fn("_CFPreferencesSetMultiple", [](Cpu& c) {
        for (auto& [k, v] : foundation::dict_entries(c, c.arg(0)))
            send(c, s_defaults(c), "setObject:forKey:", {v, k});
        for (Id k : foundation::array_items(c, c.arg(1)))
            send(c, s_defaults(c), "removeObjectForKey:", {k});
    });
    h.fn("_CFPreferencesCopyKeyList", [](Cpu& c) { c.ret(0); });
    h.fn("_CFPreferencesAppSynchronize", [](Cpu& c) { c.ret(1); });

    h.fn("_CFUUIDCreate", [](Cpu& c) {
        Id u = send(c, cls(c, "NSUUID"), "UUID");
        uint8_t b[16];
        c.mem.read_bytes(u + 8, b, 16);
        c.ret(new_uuid(c, b));
    });
    h.fn("_CFUUIDCreateFromUUIDBytes", [](Cpu& c) {
        uint64_t lo = c.arg(1), hi = c.arg(2);
        uint8_t b[16];
        std::memcpy(b, &lo, 8);
        std::memcpy(b + 8, &hi, 8);
        c.ret(new_uuid(c, b));
    });
    h.fn("_CFUUIDGetUUIDBytes", [](Cpu& c) {
        c.set_x(1, c.mem.read<uint64_t>(c.arg(0) + 16));
        c.ret(c.mem.read<uint64_t>(c.arg(0) + 8));
    });
    h.fn("_CFUUIDCreateString", [](Cpu& c) {
        std::string out;
        char buf[4];
        for (int i = 0; i < 16; ++i)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
            std::snprintf(buf, sizeof buf, "%02X", c.mem.read<uint8_t>(c.arg(1) + 8 + i));
            out += buf;
        }
        c.ret(foundation::new_string(c, foundation::utf8_to_16(out)));
    });
}

}

void register_corefoundation(objc::ObjcRuntime& o)
{
    o.define("__NSCFUUID", "NSObject");
    Hle& h = o.rt.hle;
    register_basics(h);
    register_collections(h);
    register_strings(h);
    register_misc(h);
}

}
