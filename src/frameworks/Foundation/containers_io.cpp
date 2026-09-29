#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "core/plist.h"
#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/Foundation/plist_bridge.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::Class;
using objc::objc;

bool is_mutable_class(Cpu& c);

namespace
{
Id send(Cpu& c, Id o, const char* sel, std::initializer_list<uint64_t> args = {})
{
    return objc(c).send(c, o, sel, args);
}

struct Cache
{
    Id dict = 0, name = 0, delegate = 0;
    uint64_t count_limit = 0, cost_limit = 0;
    bool evicts = true;
};

std::mutex caches_lock;
std::unordered_map<Id, Cache> caches;

Id cache_dict(Cpu& c, Id self)
{
    {
        std::lock_guard g(caches_lock);
        auto it = caches.find(self);
        if (it != caches.end() && it->second.dict) return it->second.dict;
    }
    Id d = objc(c).retain(make_dict(c, {}, true));
    std::lock_guard g(caches_lock);
    caches[self].dict = d;
    return d;
}

template <typename F> auto with_cache(Id self, F&& f)
{
    std::lock_guard g(caches_lock);
    return f(caches[self]);
}

void register_cache(objc::ObjcRuntime& o)
{
    o.define("NSCache", "NSObject");
    const char* C = "NSCache";
    o.method(C, "objectForKey:", [](Cpu& c) { c.ret(send(c, cache_dict(c, c.arg(0)), "objectForKey:", {c.arg(2)})); });
    auto set = [](Cpu& c) {
        if (!c.arg(2)) return;
        send(c, cache_dict(c, c.arg(0)), "setObject:forKey:", {c.arg(2), c.arg(3)});
    };
    static decltype(set) s_set = set;
    o.method(C, "setObject:forKey:", [](Cpu& c) { s_set(c); });
    o.method(C, "setObject:forKey:cost:", [](Cpu& c) { s_set(c); });
    o.method(C, "removeObjectForKey:", [](Cpu& c) { send(c, cache_dict(c, c.arg(0)), "removeObjectForKey:", {c.arg(2)}); });
    o.method(C, "removeAllObjects", [](Cpu& c) { send(c, cache_dict(c, c.arg(0)), "removeAllObjects"); });
    o.method(C, "countLimit", [](Cpu& c) { c.ret(with_cache(c.arg(0), [](Cache& k) { return k.count_limit; })); });
    o.method(C, "setCountLimit:", [](Cpu& c) { with_cache(c.arg(0), [&](Cache& k) { return k.count_limit = c.arg(2); }); });
    o.method(C, "totalCostLimit", [](Cpu& c) { c.ret(with_cache(c.arg(0), [](Cache& k) { return k.cost_limit; })); });
    o.method(C, "setTotalCostLimit:", [](Cpu& c) { with_cache(c.arg(0), [&](Cache& k) { return k.cost_limit = c.arg(2); }); });
    o.method(C, "evictsObjectsWithDiscardedContent", [](Cpu& c) { c.ret(with_cache(c.arg(0), [](Cache& k) { return k.evicts; })); });
    o.method(C, "setEvictsObjectsWithDiscardedContent:", [](Cpu& c) {
        with_cache(c.arg(0), [&](Cache& k) { return k.evicts = c.arg(2) & 1; });
    });
    o.method(C, "delegate", [](Cpu& c) { c.ret(with_cache(c.arg(0), [](Cache& k) { return k.delegate; })); });
    o.method(C, "setDelegate:", [](Cpu& c) { with_cache(c.arg(0), [&](Cache& k) { return k.delegate = c.arg(2); }); });
    o.method(C, "name", [](Cpu& c) { c.ret(with_cache(c.arg(0), [](Cache& k) { return k.name; })); });
    o.method(C, "setName:", [](Cpu& c) {
        Id copy = c.arg(2) ? send(c, c.arg(2), "copy") : 0;
        Id old = with_cache(c.arg(0), [&](Cache& k) { return std::exchange(k.name, copy); });
        if (old) objc(c).release(c, old);
    });
    o.method(C, "dealloc", [](Cpu& c) {
        Cache k;
        {
            std::lock_guard g(caches_lock);
            auto it = caches.find(c.arg(0));
            if (it != caches.end())
            {
                k = it->second;
                caches.erase(it);
            }
        }
        for (Id obj : {k.dict, k.name})
            if (obj) objc(c).release(c, obj);
        objc(c).dispose(c.arg(0));
    });
}

Id plist_file_object(Cpu& c, const std::string& guest_path, bool want_dict, bool mutable_)
{
    auto host = c.rt.vfs.to_host(guest_path);
    if (!host) return 0;
    std::ifstream f(*host, std::ios::binary);
    if (!f) return 0;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    auto p = parse_plist(bytes);
    if (!p) return 0;
    Id obj = plist_to_object(c, *p, mutable_);
    if (!obj) return 0;
    Class* want = objc(c).class_named(want_dict ? "NSDictionary" : "NSArray");
    return objc(c).is_kind_of(obj, want) ? obj : 0;
}

void register_contents_loading(objc::ObjcRuntime& o)
{
    struct Entry
    {
        const char* cls;
        bool dict;
    };
    for (Entry e : {Entry{"NSDictionary", true}, Entry{"NSArray", false}})
    {
        std::string noun = e.dict ? "dictionary" : "array";
        if (e.dict)
        {
            o.class_method(e.cls, noun + "WithContentsOfFile:", [](Cpu& c) {
                c.ret(plist_file_object(c, to_utf8(c, c.arg(2)), true, is_mutable_class(c)));
            });
            o.class_method(e.cls, noun + "WithContentsOfURL:", [](Cpu& c) {
                c.ret(plist_file_object(c, url_string_path(c, c.arg(2)), true, is_mutable_class(c)));
            });
            o.class_method(e.cls, noun + "WithContentsOfURL:error:", [](Cpu& c) {
                c.ret(plist_file_object(c, url_string_path(c, c.arg(2)), true, is_mutable_class(c)));
            });
            o.method(e.cls, "initWithContentsOfFile:", [](Cpu& c) {
                Id obj = plist_file_object(c, to_utf8(c, c.arg(2)), true, false);
                objc(c).dispose(c.arg(0));
                c.ret(obj ? objc(c).retain(obj) : 0);
            });
            o.method(e.cls, "initWithContentsOfURL:", [](Cpu& c) {
                Id obj = plist_file_object(c, url_string_path(c, c.arg(2)), true, false);
                objc(c).dispose(c.arg(0));
                c.ret(obj ? objc(c).retain(obj) : 0);
            });
        }
        else
        {
            o.class_method(e.cls, noun + "WithContentsOfFile:", [](Cpu& c) {
                c.ret(plist_file_object(c, to_utf8(c, c.arg(2)), false, is_mutable_class(c)));
            });
            o.class_method(e.cls, noun + "WithContentsOfURL:", [](Cpu& c) {
                c.ret(plist_file_object(c, url_string_path(c, c.arg(2)), false, is_mutable_class(c)));
            });
            o.class_method(e.cls, noun + "WithContentsOfURL:error:", [](Cpu& c) {
                c.ret(plist_file_object(c, url_string_path(c, c.arg(2)), false, is_mutable_class(c)));
            });
        }
    }
}

struct Components
{
    std::string scheme, user, password, host, path, query, fragment;
    int64_t port = -1;
    bool has_query = false, has_fragment = false;
};

std::mutex components_lock;
std::unordered_map<Id, Components> components;

Components parse_url(const std::string& text)
{
    Components u;
    std::string rest = text;
    if (auto hash = rest.find('#'); hash != std::string::npos)
    {
        u.fragment = rest.substr(hash + 1);
        u.has_fragment = true;
        rest.resize(hash);
    }
    if (auto q = rest.find('?'); q != std::string::npos)
    {
        u.query = rest.substr(q + 1);
        u.has_query = true;
        rest.resize(q);
    }
    if (auto colon = rest.find(':'); colon != std::string::npos && rest.find('/') > colon)
    {
        u.scheme = rest.substr(0, colon);
        rest = rest.substr(colon + 1);
    }
    if (rest.rfind("//", 0) == 0)
    {
        rest = rest.substr(2);
        size_t slash = rest.find('/');
        std::string authority = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash);
        if (auto at = authority.rfind('@'); at != std::string::npos)
        {
            std::string info = authority.substr(0, at);
            authority = authority.substr(at + 1);
            auto colon = info.find(':');
            u.user = info.substr(0, colon);
            if (colon != std::string::npos) u.password = info.substr(colon + 1);
        }
        auto colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(']') == std::string::npos && colon + 1 < authority.size())
        {
            u.port = std::atoll(authority.c_str() + colon + 1);
            authority.resize(colon);
        }
        u.host = authority;
    }
    u.path = rest;
    return u;
}

std::string build_url(const Components& u)
{
    std::string s;
    if (!u.scheme.empty()) s += u.scheme + ":";
    if (!u.host.empty() || !u.user.empty())
    {
        s += "//";
        if (!u.user.empty()) s += u.user + (u.password.empty() ? "" : ":" + u.password) + "@";
        s += u.host;
        if (u.port >= 0) s += ":" + std::to_string(u.port);
    }
    s += u.path;
    if (u.has_query) s += "?" + u.query;
    if (u.has_fragment) s += "#" + u.fragment;
    return s;
}

template <typename F> auto with_components(Id self, F&& f)
{
    std::lock_guard g(components_lock);
    return f(components[self]);
}

Id optional_string(Cpu& c, const std::string& s, bool present = true)
{
    return present && !s.empty() ? string_autoreleased(c, s) : 0;
}

void register_url_components(objc::ObjcRuntime& o)
{
    o.define("NSURLComponents", "NSObject");
    o.define("NSURLQueryItem", "NSObject");
    const char* U = "NSURLComponents";
    o.method(U, "init", [](Cpu& c) {
        with_components(c.arg(0), [](Components& u) { return u = {}, 0; });
        c.ret(c.arg(0));
    });
    o.method(U, "initWithString:", [](Cpu& c) {
        std::string text = c.arg(2) ? to_utf8(c, c.arg(2)) : "";
        with_components(c.arg(0), [&](Components& u) { return u = parse_url(text), 0; });
        c.ret(c.arg(0));
    });
    o.method(U, "initWithURL:resolvingAgainstBaseURL:", [](Cpu& c) {
        std::string text = c.arg(2) ? to_utf8(c, send(c, c.arg(2), "absoluteString")) : "";
        with_components(c.arg(0), [&](Components& u) { return u = parse_url(text), 0; });
        c.ret(c.arg(0));
    });
    o.class_method(U, "componentsWithString:", [](Cpu& c) {
        Id obj = send(c, send(c, c.arg(0), "alloc"), "initWithString:", {c.arg(2)});
        c.ret(objc(c).autorelease(c, obj));
    });
    o.class_method(U, "componentsWithURL:resolvingAgainstBaseURL:", [](Cpu& c) {
        Id obj = send(c, send(c, c.arg(0), "alloc"), "initWithURL:resolvingAgainstBaseURL:", {c.arg(2), c.arg(3)});
        c.ret(objc(c).autorelease(c, obj));
    });
    o.method(U, "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(components_lock);
            components.erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });
    o.method(U, "copyWithZone:", [](Cpu& c) {
        Components copy = with_components(c.arg(0), [](Components& u) { return u; });
        Id obj = objc(c).alloc_instance(objc(c).class_of(c.arg(0)));
        with_components(obj, [&](Components& u) { return u = copy, 0; });
        c.ret(obj);
    });
    o.method(U, "string",
             [](Cpu& c) { c.ret(string_autoreleased(c, with_components(c.arg(0), [](Components& u) { return build_url(u); }))); });
    o.method(U, "URL", [](Cpu& c) {
        std::string s = with_components(c.arg(0), [](Components& u) { return build_url(u); });
        c.ret(send(c, objc(c).host_class("NSURL")->addr, "URLWithString:", {string_autoreleased(c, s)}));
    });
    struct Field
    {
        const char* getter;
        const char* setter;
        std::string Components::* member;
    };
    static const Field fields[] = {{"scheme", "setScheme:", &Components::scheme},
                                   {"user", "setUser:", &Components::user},
                                   {"password", "setPassword:", &Components::password},
                                   {"host", "setHost:", &Components::host},
                                   {"path", "setPath:", &Components::path},
                                   {"percentEncodedPath", "setPercentEncodedPath:", &Components::path},
                                   {"percentEncodedHost", "setPercentEncodedHost:", &Components::host}};
    static std::unordered_map<std::string, std::string Components::*> by_selector;
    for (const Field& f : fields)
    {
        by_selector[f.getter] = f.member;
        by_selector[f.setter] = f.member;
        o.method(U, f.getter, [](Cpu& c) {
            auto member = by_selector[objc(c).sel_name(objc::SEL(c.arg(1)))];
            c.ret(optional_string(c, with_components(c.arg(0), [&](Components& u) { return u.*member; })));
        });
        o.method(U, f.setter, [](Cpu& c) {
            auto member = by_selector[objc(c).sel_name(objc::SEL(c.arg(1)))];
            std::string v = c.arg(2) ? to_utf8(c, c.arg(2)) : "";
            with_components(c.arg(0), [&](Components& u) { return u.*member = v, 0; });
        });
    }
    for (const char* sel : {"query", "percentEncodedQuery"})
        o.method(U, sel, [](Cpu& c) {
            auto [q, has] = with_components(c.arg(0), [](Components& u) { return std::pair{u.query, u.has_query}; });
            c.ret(has ? string_autoreleased(c, q) : 0);
        });
    for (const char* sel : {"setQuery:", "setPercentEncodedQuery:"})
        o.method(U, sel, [](Cpu& c) {
            std::string v = c.arg(2) ? to_utf8(c, c.arg(2)) : "";
            bool has = c.arg(2) != 0;
            with_components(c.arg(0), [&](Components& u) { return u.query = v, u.has_query = has, 0; });
        });
    o.method(U, "fragment", [](Cpu& c) {
        auto [f, has] = with_components(c.arg(0), [](Components& u) { return std::pair{u.fragment, u.has_fragment}; });
        c.ret(has ? string_autoreleased(c, f) : 0);
    });
    o.method(U, "setFragment:", [](Cpu& c) {
        std::string v = c.arg(2) ? to_utf8(c, c.arg(2)) : "";
        bool has = c.arg(2) != 0;
        with_components(c.arg(0), [&](Components& u) { return u.fragment = v, u.has_fragment = has, 0; });
    });
    o.method(U, "port", [](Cpu& c) {
        int64_t port = with_components(c.arg(0), [](Components& u) { return u.port; });
        c.ret(port >= 0 ? make_number(c, NumberData{'q', port}) : 0);
    });
    o.method(U, "setPort:", [](Cpu& c) {
        NumberData n;
        int64_t port = c.arg(2) && number_of(c, c.arg(2), n) ? n.as_int() : -1;
        with_components(c.arg(0), [&](Components& u) { return u.port = port, 0; });
    });
    o.method(U, "queryItems", [](Cpu& c) {
        auto [q, has] = with_components(c.arg(0), [](Components& u) { return std::pair{u.query, u.has_query}; });
        if (!has) return c.ret(0);
        std::vector<Id> items;
        size_t at = 0;
        while (at <= q.size())
        {
            size_t amp = q.find('&', at);
            std::string pair = q.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
            if (!pair.empty())
            {
                auto eq = pair.find('=');
                Id name = string_autoreleased(c, pair.substr(0, eq));
                Id value = eq == std::string::npos ? 0 : string_autoreleased(c, pair.substr(eq + 1));
                items.push_back(send(c, objc(c).host_class("NSURLQueryItem")->addr, "queryItemWithName:value:", {name, value}));
            }
            if (amp == std::string::npos) break;
            at = amp + 1;
        }
        c.ret(make_array(c, items));
    });
    o.method(U, "setQueryItems:", [](Cpu& c) {
        std::string q;
        bool has = c.arg(2) != 0;
        if (has)
            for (Id item : array_items(c, c.arg(2)))
            {
                if (!q.empty()) q += "&";
                q += to_utf8(c, send(c, item, "name"));
                if (Id v = send(c, item, "value")) q += "=" + to_utf8(c, v);
            }
        with_components(c.arg(0), [&](Components& u) { return u.query = q, u.has_query = has, 0; });
    });

    const char* Q = "NSURLQueryItem";
    o.class_method(Q, "queryItemWithName:value:", [](Cpu& c) {
        Id obj = objc(c).alloc_instance(objc(c).host_class("NSURLQueryItem"), 16);
        c.mem.write<uint64_t>(obj + 8, c.arg(2) ? objc(c).retain(send(c, c.arg(2), "copy")) : 0);
        c.mem.write<uint64_t>(obj + 16, c.arg(3) ? objc(c).retain(send(c, c.arg(3), "copy")) : 0);
        c.ret(objc(c).autorelease(c, obj));
    });
    o.method(Q, "initWithName:value:", [](Cpu& c) {
        Id obj = send(c, objc(c).host_class("NSURLQueryItem")->addr, "queryItemWithName:value:", {c.arg(2), c.arg(3)});
        objc(c).dispose(c.arg(0));
        c.ret(objc(c).retain(obj));
    });
    o.method(Q, "name", [](Cpu& c) { c.ret(c.mem.read<uint64_t>(c.arg(0) + 8)); });
    o.method(Q, "value", [](Cpu& c) { c.ret(c.mem.read<uint64_t>(c.arg(0) + 16)); });
    o.method(Q, "dealloc", [](Cpu& c) {
        for (uint64_t off : {8, 16})
            if (Id v = c.mem.read<uint64_t>(c.arg(0) + off)) objc(c).release(c, v);
        objc(c).dispose(c.arg(0));
    });
}
}

namespace
{
Id resource_value(Cpu& c, const std::filesystem::path& host, const std::string& key)
{
    std::error_code ec;
    auto status = std::filesystem::status(host, ec);
    bool exists = !ec && std::filesystem::exists(status);
    auto number = [&](int64_t v) { return make_number(c, NumberData{'q', v}); };
    auto flag = [&](bool v) { return make_number(c, NumberData{'B', int64_t(v)}); };
    if (key == "NSURLIsDirectoryKey") return exists ? flag(std::filesystem::is_directory(status)) : 0;
    if (key == "NSURLIsRegularFileKey") return exists ? flag(std::filesystem::is_regular_file(status)) : 0;
    if (key == "NSURLIsHiddenKey") return flag(host.filename().string().starts_with("."));
    if (key == "NSURLIsExcludedFromBackupKey") return flag(false);
    if (key == "NSURLNameKey" || key == "NSURLLocalizedNameKey") return string_autoreleased(c, host.filename().string());
    if (key == "NSURLFileSizeKey" || key == "NSURLFileAllocatedSizeKey" || key == "NSURLTotalFileSizeKey" ||
        key == "NSURLTotalFileAllocatedSizeKey")
    {
        if (!exists || !std::filesystem::is_regular_file(status)) return 0;
        return number(int64_t(std::filesystem::file_size(host, ec)));
    }
    if (key == "NSURLContentModificationDateKey" || key == "NSURLCreationDateKey" || key == "NSURLContentAccessDateKey" ||
        key == "NSURLAttributeModificationDateKey")
    {
        if (!exists) return 0;
        auto t = std::filesystem::last_write_time(host, ec);
        auto since = std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch();
        double unix_seconds = std::chrono::duration<double>(since).count();
        return date_with_reference_seconds(c, unix_seconds - 978307200.0);
    }
    if (key.starts_with("NSURLVolumeAvailableCapacity") || key == "NSURLVolumeTotalCapacityKey")
    {
        auto space = std::filesystem::space(host.root_path(), ec);
        if (ec) return 0;
        return number(int64_t(key == "NSURLVolumeTotalCapacityKey" ? space.capacity : space.available));
    }
    return 0;
}

std::filesystem::path url_host_path(Cpu& c, Id url)
{
    auto host = c.rt.vfs.to_host(url_string_path(c, url));
    return host ? *host : std::filesystem::path();
}

void register_url_resources(objc::ObjcRuntime& o)
{
    o.method("NSURL", "resourceValuesForKeys:error:", [](Cpu& c) {
        auto host = url_host_path(c, c.arg(0));
        std::vector<std::pair<Id, Id>> entries;
        if (!host.empty())
            for (Id key : array_items(c, c.arg(2)))
                if (Id v = resource_value(c, host, to_utf8(c, key))) entries.push_back({key, v});
        c.ret(make_dict(c, entries));
    });
    o.method("NSURL", "getResourceValue:forKey:error:", [](Cpu& c) {
        auto host = url_host_path(c, c.arg(0));
        Id v = host.empty() ? 0 : resource_value(c, host, to_utf8(c, c.arg(3)));
        if (c.arg(2)) c.mem.write<uint64_t>(c.arg(2), v);
        c.ret(1);
    });
    o.method("NSURL", "setResourceValues:error:", [](Cpu& c) { c.ret(1); });
    Hle& h = o.rt.hle;
    h.fn("_setxattr", [](Cpu& c) { c.ret(0); });
    h.fn("_removexattr", [](Cpu& c) { c.ret(0); });
    h.fn("_getxattr", [](Cpu& c) { c.ret(uint64_t(-1)); });
}
}

bool is_mutable_class(Cpu& c)
{
    Class* k = objc(c).class_at(c.arg(0));
    for (const char* m : {"NSMutableDictionary", "NSMutableArray"})
        if (k && objc(c).is_subclass(k, objc(c).class_named(m))) return true;
    return false;
}

void register_containers_io(objc::ObjcRuntime& o)
{
    register_cache(o);
    register_contents_loading(o);
    register_url_components(o);
    register_url_resources(o);
}

}
