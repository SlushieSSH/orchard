#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/Foundation/plist_bridge.h"
#include "frameworks/libSystem/mach_time.h"
#include "objc/runtime.h"

namespace fs = std::filesystem;

namespace orchard::foundation
{
using objc::Class;
using objc::objc;

namespace
{
std::optional<Plist> read_plist_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    return parse_plist(bytes);
}

struct Bundle
{
    std::string path;
    Plist info;
    Id info_object = 0;
};

std::mutex bundles_lock;
std::unordered_map<Id, Bundle> bundles;
std::unordered_map<std::string, Id> bundles_by_path;

Id bundle_for_path(Cpu& c, const std::string& guest_path)
{
    std::string path = Vfs::normalize(guest_path);
    std::lock_guard g(bundles_lock);
    if (auto it = bundles_by_path.find(path); it != bundles_by_path.end()) return it->second;
    auto host = c.rt.vfs.to_host(path);
    if (!host || !fs::is_directory(*host)) return 0;
    Id obj = objc(c).alloc_instance(objc(c).host_class("NSBundle"), 8);
    Bundle& b = bundles[obj];
    b.path = path;
    if (auto info = read_plist_file(*host / "Info.plist")) b.info = std::move(*info);
    bundles_by_path[path] = obj;
    return obj;
}

Bundle* bundle_of(Id obj)
{
    std::lock_guard g(bundles_lock);
    auto it = bundles.find(obj);
    return it == bundles.end() ? nullptr : &it->second;
}

Id main_bundle(Cpu& c)
{
    return bundle_for_path(c, c.rt.vfs.bundle_path);
}

Id str(Cpu& c, const std::string& s)
{
    return string_autoreleased(c, s);
}

bool guest_exists(Cpu& c, const std::string& path, bool* is_dir = nullptr)
{
    auto host = c.rt.vfs.to_host(path);
    std::error_code ec;
    if (!host || !fs::exists(*host, ec)) return false;
    if (is_dir) *is_dir = fs::is_directory(*host, ec);
    return true;
}

std::string find_resource(Cpu& c, Bundle& b, const std::string& name, const std::string& ext, const std::string& subdir)
{
    std::string file = ext.empty() ? name : name + "." + (ext[0] == '.' ? ext.substr(1) : ext);
    std::vector<std::string> dirs;
    std::string base = subdir.empty() ? b.path : b.path + "/" + subdir;
    dirs.push_back(base);
    dirs.push_back(base + "/en.lproj");
    dirs.push_back(base + "/Base.lproj");
    for (auto& d : dirs)
    {
        std::string candidate = d + "/" + file;
        if (guest_exists(c, candidate)) return candidate;
    }
    return {};
}

Id make_url(Cpu& c, const std::string& text, bool file)
{
    Id url = objc(c).alloc_instance(objc(c).host_class("NSURL"), 16);
    c.mem.write<uint64_t>(url + 8, new_string(c, utf8_to_16(text)));
    c.mem.write<uint64_t>(url + 16, file);
    return objc(c).autorelease(c, url);
}

std::string url_text(Cpu& c, Id url)
{
    return url ? to_utf8(c, c.mem.read<uint64_t>(url + 8)) : "";
}
bool url_is_file(Cpu& c, Id url)
{
    return url && c.mem.read<uint64_t>(url + 16);
}

std::string url_path(Cpu& c, Id url)
{
    std::string t = url_text(c, url);
    if (url_is_file(c, url)) return t;
    auto scheme = t.find("://");
    if (scheme == std::string::npos) return t;
    auto slash = t.find('/', scheme + 3);
    if (slash == std::string::npos) return "";
    auto end = t.find_first_of("?#", slash);
    return t.substr(slash, end == std::string::npos ? std::string::npos : end - slash);
}

struct Defaults
{
    std::mutex m;
    Plist values;
    Plist registered;
    bool loaded = false;
    fs::path file;
};

Defaults& defaults()
{
    static Defaults d;
    return d;
}

void load_defaults(Cpu& c)
{
    Defaults& d = defaults();
    if (d.loaded) return;
    d.loaded = true;
    d.values.kind = Plist::Kind::Dict;
    d.registered.kind = Plist::Kind::Dict;
    auto prefs = c.rt.vfs.to_host(c.rt.vfs.home + "/Library/Preferences/" + c.rt.vfs.bundle_id + ".plist");
    if (!prefs) return;
    d.file = *prefs;
    if (auto p = read_plist_file(d.file); p && p->kind == Plist::Kind::Dict) d.values = std::move(*p);
}

void save_defaults()
{
    Defaults& d = defaults();
    if (d.file.empty()) return;
    std::error_code ec;
    fs::create_directories(d.file.parent_path(), ec);
    std::ofstream(d.file, std::ios::binary) << write_xml_plist(d.values);
}

const Plist* defaults_get(const std::string& key)
{
    Defaults& d = defaults();
    if (const Plist* v = d.values.get(key)) return v;
    return d.registered.get(key);
}

void defaults_set(const std::string& key, std::optional<Plist> value)
{
    Defaults& d = defaults();
    auto& dict = d.values.dict;
    for (auto it = dict.begin(); it != dict.end(); ++it)
    {
        if (it->first == key)
        {
            dict.erase(it);
            break;
        }
    }
    if (value) dict.emplace_back(key, std::move(*value));
    save_defaults();
}

double plist_number(const Plist* p)
{
    if (!p) return 0;
    switch (p->kind)
    {
    case Plist::Kind::Bool: return p->b;
    case Plist::Kind::Int: return double(p->i);
    case Plist::Kind::Real: return p->r;
    case Plist::Kind::String: return std::strtod(p->s.c_str(), nullptr);
    default: return 0;
    }
}

Plist number_plist(char kind, double v)
{
    Plist p;
    if (kind == 'B')
    {
        p.kind = Plist::Kind::Bool;
        p.b = v != 0;
    }
    else if (kind == 'q')
    {
        p.kind = Plist::Kind::Int;
        p.i = int64_t(v);
    }
    else
    {
        p.kind = Plist::Kind::Real;
        p.r = v;
    }
    return p;
}

std::string key_arg(Cpu& c, int i)
{
    return to_utf8(c, c.arg(i));
}

void register_bundle(objc::ObjcRuntime& o)
{
    o.class_method("NSBundle", "mainBundle", [](Cpu& c) { c.ret(main_bundle(c)); });
    o.class_method("NSBundle", "bundleWithPath:", [](Cpu& c) { c.ret(bundle_for_path(c, to_utf8(c, c.arg(2)))); });
    o.class_method("NSBundle", "bundleWithURL:", [](Cpu& c) { c.ret(bundle_for_path(c, url_path(c, c.arg(2)))); });
    o.class_method("NSBundle", "bundleForClass:", [](Cpu& c) { c.ret(main_bundle(c)); });
    o.class_method("NSBundle", "bundleWithIdentifier:", [](Cpu& c) {
        std::string id = to_utf8(c, c.arg(2));
        Id mb = main_bundle(c);
        c.ret(bundle_of(mb)->info.string_or("CFBundleIdentifier") == id ? mb : 0);
    });
    o.class_method("NSBundle", "allBundles", [](Cpu& c) { c.ret(make_array(c, {main_bundle(c)})); });
    o.class_method("NSBundle", "allFrameworks", [](Cpu& c) { c.ret(make_array(c, {})); });
    o.method("NSBundle", "initWithPath:", [](Cpu& c) { c.ret(objc(c).retain(bundle_for_path(c, to_utf8(c, c.arg(2))))); });

    o.method("NSBundle", "bundlePath", [](Cpu& c) { c.ret(str(c, bundle_of(c.arg(0))->path)); });
    o.method("NSBundle", "resourcePath", [](Cpu& c) { c.ret(str(c, bundle_of(c.arg(0))->path)); });
    o.method("NSBundle", "bundleURL", [](Cpu& c) { c.ret(make_url(c, bundle_of(c.arg(0))->path, true)); });
    o.method("NSBundle", "resourceURL", [](Cpu& c) { c.ret(make_url(c, bundle_of(c.arg(0))->path, true)); });
    o.method("NSBundle", "privateFrameworksPath", [](Cpu& c) { c.ret(str(c, bundle_of(c.arg(0))->path + "/Frameworks")); });
    o.method("NSBundle", "builtInPlugInsPath", [](Cpu& c) { c.ret(str(c, bundle_of(c.arg(0))->path + "/PlugIns")); });
    o.method("NSBundle", "executablePath", [](Cpu& c) {
        Bundle* b = bundle_of(c.arg(0));
        c.ret(str(c, b->path + "/" + b->info.string_or("CFBundleExecutable")));
    });
    o.method("NSBundle", "bundleIdentifier", [](Cpu& c) {
        std::string id = bundle_of(c.arg(0))->info.string_or("CFBundleIdentifier");
        c.ret(id.empty() ? 0 : str(c, id));
    });
    o.method("NSBundle", "infoDictionary", [](Cpu& c) {
        Bundle* b = bundle_of(c.arg(0));
        if (!b->info_object) b->info_object = objc(c).retain(plist_to_object(c, b->info));
        c.ret(b->info_object);
    });
    o.method("NSBundle", "localizedInfoDictionary", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "infoDictionary")); });
    o.method("NSBundle", "objectForInfoDictionaryKey:", [](Cpu& c) {
        Bundle* b = bundle_of(c.arg(0));
        const Plist* v = b->info.get(key_arg(c, 2));
        c.ret(v ? plist_to_object(c, *v) : 0);
    });
    o.method("NSBundle", "isLoaded", [](Cpu& c) { c.ret(1); });
    o.method("NSBundle", "load", [](Cpu& c) { c.ret(1); });
    o.method("NSBundle", "loadAndReturnError:", [](Cpu& c) { c.ret(1); });
    o.method("NSBundle", "principalClass", [](Cpu& c) {
        Class* k = objc(c).class_named(bundle_of(c.arg(0))->info.string_or("NSPrincipalClass"));
        c.ret(k ? k->addr : 0);
    });
    o.method("NSBundle", "classNamed:", [](Cpu& c) {
        Class* k = objc(c).class_named(key_arg(c, 2));
        c.ret(k ? k->addr : 0);
    });
    o.method("NSBundle", "preferredLocalizations", [](Cpu& c) { c.ret(make_array(c, {str(c, "en")})); });
    o.method("NSBundle", "localizations", [](Cpu& c) { c.ret(make_array(c, {str(c, "en")})); });
    o.method("NSBundle", "developmentLocalization", [](Cpu& c) { c.ret(str(c, "en")); });
    o.method("NSBundle", "appStoreReceiptURL",
             [](Cpu& c) { c.ret(make_url(c, bundle_of(c.arg(0))->path + "/_MASReceipt/receipt", true)); });
    o.method("NSBundle", "localizedStringForKey:value:table:", [](Cpu& c) {
        Id value = c.arg(3);
        c.ret(value && !to_utf16(c, value).empty() ? value : c.arg(2));
    });
    auto path_for = [](Cpu& c, Id name, Id ext, Id dir) -> std::string {
        Bundle* b = bundle_of(c.arg(0));
        return b ? find_resource(c, *b, to_utf8(c, name), to_utf8(c, ext), to_utf8(c, dir)) : "";
    };
    static decltype(path_for) s_path_for = path_for;
    o.method("NSBundle", "pathForResource:ofType:", [](Cpu& c) {
        std::string p = s_path_for(c, c.arg(2), c.arg(3), 0);
        c.ret(p.empty() ? 0 : str(c, p));
    });
    o.method("NSBundle", "pathForResource:ofType:inDirectory:", [](Cpu& c) {
        std::string p = s_path_for(c, c.arg(2), c.arg(3), c.arg(4));
        c.ret(p.empty() ? 0 : str(c, p));
    });
    o.method("NSBundle", "URLForResource:withExtension:", [](Cpu& c) {
        std::string p = s_path_for(c, c.arg(2), c.arg(3), 0);
        c.ret(p.empty() ? 0 : make_url(c, p, true));
    });
    o.method("NSBundle", "URLForResource:withExtension:subdirectory:", [](Cpu& c) {
        std::string p = s_path_for(c, c.arg(2), c.arg(3), c.arg(4));
        c.ret(p.empty() ? 0 : make_url(c, p, true));
    });
}

void register_process_info(objc::ObjcRuntime& o)
{
    o.class_method("NSProcessInfo", "processInfo", [](Cpu& c) {
        static Id info = objc(c).alloc_instance(objc(c).host_class("NSProcessInfo"));
        c.ret(info);
    });
    o.method("NSProcessInfo", "processName", [](Cpu& c) { c.ret(str(c, fs::path(c.rt.vfs.bundle_path).stem().string())); });
    o.method("NSProcessInfo", "processIdentifier", [](Cpu& c) { c.ret(4242); });
    o.method("NSProcessInfo", "arguments", [](Cpu& c) {
        std::string exe = c.rt.vfs.bundle_path + "/" + fs::path(c.rt.vfs.bundle_path).stem().string();
        c.ret(make_array(c, {str(c, exe)}));
    });
    o.method("NSProcessInfo", "environment", [](Cpu& c) {
        c.ret(make_dict(c, {{str(c, "HOME"), str(c, c.rt.vfs.home)}, {str(c, "TMPDIR"), str(c, c.rt.vfs.home + "/tmp/")}}));
    });
    o.method("NSProcessInfo", "globallyUniqueString", [](Cpu& c) {
        static std::atomic<uint64_t> n{1};
        char buf[64];
        std::snprintf(buf, sizeof buf, "ORCHARD-%016llX-%llu", (unsigned long long)mach_now(), (unsigned long long)n++);
        c.ret(str(c, buf));
    });
    o.method("NSProcessInfo", "hostName", [](Cpu& c) { c.ret(str(c, "iPhone")); });
    o.method("NSProcessInfo", "operatingSystemVersionString", [](Cpu& c) { c.ret(str(c, "Version 18.5 (Build 22F76)")); });
    o.method("NSProcessInfo", "operatingSystemVersion", [](Cpu& c) {
        GuestAddr out = c.x(8);
        c.mem.write<int64_t>(out, 18);
        c.mem.write<int64_t>(out + 8, 5);
        c.mem.write<int64_t>(out + 16, 0);
    });
    o.method("NSProcessInfo", "isOperatingSystemAtLeastVersion:", [](Cpu& c) {
        GuestAddr v = c.arg(2);
        int64_t major = c.mem.read<int64_t>(v), minor = c.mem.read<int64_t>(v + 8);
        c.ret(major < 18 || (major == 18 && minor <= 5));
    });
    o.method("NSProcessInfo", "physicalMemory", [](Cpu& c) { c.ret(6ull << 30); });
    o.method("NSProcessInfo", "processorCount", [](Cpu& c) { c.ret(6); });
    o.method("NSProcessInfo", "activeProcessorCount", [](Cpu& c) { c.ret(6); });
    o.method("NSProcessInfo", "systemUptime",
             [](Cpu& c) { c.set_d(0, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()); });
    o.method("NSProcessInfo", "thermalState", [](Cpu& c) { c.ret(0); });
    o.method("NSProcessInfo", "isLowPowerModeEnabled", [](Cpu& c) { c.ret(0); });
    o.method("NSProcessInfo", "isiOSAppOnMac", [](Cpu& c) { c.ret(0); });
    o.method("NSProcessInfo", "isMacCatalystApp", [](Cpu& c) { c.ret(0); });
    o.method("NSProcessInfo",
             "beginActivityWithOptions:reason:", [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("NSObject")->addr, "new")); });
    o.method("NSProcessInfo", "endActivity:", [](Cpu& c) {});
}

void register_defaults(objc::ObjcRuntime& o)
{
    o.class_method("NSUserDefaults", "standardUserDefaults", [](Cpu& c) {
        static Id d = objc(c).alloc_instance(objc(c).host_class("NSUserDefaults"));
        c.ret(d);
    });
    o.method("NSUserDefaults", "initWithSuiteName:", [](Cpu& c) {});
    o.method("NSUserDefaults", "objectForKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        const Plist* v = defaults_get(key_arg(c, 2));
        c.ret(v ? plist_to_object(c, *v) : 0);
    });
    o.method("NSUserDefaults", "stringForKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        const Plist* v = defaults_get(key_arg(c, 2));
        c.ret(v && v->kind == Plist::Kind::String ? str(c, v->s) : 0);
    });
    for (const char* sel : {"arrayForKey:", "dictionaryForKey:", "dataForKey:", "stringArrayForKey:"})
    {
        o.method("NSUserDefaults", sel, [](Cpu& c) {
            std::lock_guard g(defaults().m);
            load_defaults(c);
            const Plist* v = defaults_get(key_arg(c, 2));
            c.ret(v ? plist_to_object(c, *v) : 0);
        });
    }
    o.method("NSUserDefaults", "integerForKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        c.ret(uint64_t(int64_t(plist_number(defaults_get(key_arg(c, 2))))));
    });
    o.method("NSUserDefaults", "boolForKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        c.ret(plist_number(defaults_get(key_arg(c, 2))) != 0);
    });
    o.method("NSUserDefaults", "floatForKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        c.set_s(0, float(plist_number(defaults_get(key_arg(c, 2)))));
    });
    o.method("NSUserDefaults", "doubleForKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        c.set_d(0, plist_number(defaults_get(key_arg(c, 2))));
    });
    o.method("NSUserDefaults", "setObject:forKey:", [](Cpu& c) {
        auto value = c.arg(2) ? object_to_plist(c, c.arg(2)) : std::nullopt;
        std::lock_guard g(defaults().m);
        load_defaults(c);
        defaults_set(key_arg(c, 3), std::move(value));
    });
    o.method("NSUserDefaults", "valueForKey:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "objectForKey:", {c.arg(2)})); });
    o.method("NSUserDefaults", "setValue:forKey:", [](Cpu& c) { objc(c).send(c, c.arg(0), "setObject:forKey:", {c.arg(2), c.arg(3)}); });
    o.method("NSUserDefaults", "setInteger:forKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        defaults_set(key_arg(c, 3), number_plist('q', double(int64_t(c.arg(2)))));
    });
    o.method("NSUserDefaults", "setBool:forKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        defaults_set(key_arg(c, 3), number_plist('B', double(c.arg(2) & 1)));
    });
    o.method("NSUserDefaults", "setFloat:forKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        defaults_set(key_arg(c, 2), number_plist('d', c.s(0)));
    });
    o.method("NSUserDefaults", "setDouble:forKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        defaults_set(key_arg(c, 2), number_plist('d', c.d(0)));
    });
    o.method("NSUserDefaults", "removeObjectForKey:", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        defaults_set(key_arg(c, 2), std::nullopt);
    });
    o.method("NSUserDefaults", "registerDefaults:", [](Cpu& c) {
        auto p = object_to_plist(c, c.arg(2));
        std::lock_guard g(defaults().m);
        load_defaults(c);
        if (p && p->kind == Plist::Kind::Dict)
            for (auto& [k, v] : p->dict)
                defaults().registered.dict.emplace_back(k, v);
    });
    o.method("NSUserDefaults", "dictionaryRepresentation", [](Cpu& c) {
        std::lock_guard g(defaults().m);
        load_defaults(c);
        c.ret(plist_to_object(c, defaults().values));
    });
    o.method("NSUserDefaults", "synchronize", [](Cpu& c) { c.ret(1); });
}

std::vector<std::string> directory_paths(Cpu& c, uint64_t which)
{
    const std::string& home = c.rt.vfs.home;
    switch (which)
    {
    case 9: return {home + "/Documents"};
    case 5: return {home + "/Library"};
    case 13: return {home + "/Library/Caches"};
    case 14: return {home + "/Library/Application Support"};
    default: return {home + "/Documents"};
    }
}

void register_file_manager(objc::ObjcRuntime& o)
{
    o.class_method("NSFileManager", "defaultManager", [](Cpu& c) {
        static Id fm = objc(c).alloc_instance(objc(c).host_class("NSFileManager"));
        c.ret(fm);
    });
    o.method("NSFileManager", "fileExistsAtPath:", [](Cpu& c) { c.ret(guest_exists(c, key_arg(c, 2))); });
    o.method("NSFileManager", "fileExistsAtPath:isDirectory:", [](Cpu& c) {
        bool dir = false;
        bool exists = guest_exists(c, key_arg(c, 2), &dir);
        if (c.arg(3)) c.mem.write<uint8_t>(c.arg(3), dir);
        c.ret(exists);
    });
    for (const char* sel : {"isReadableFileAtPath:", "isWritableFileAtPath:", "isDeletableFileAtPath:"})
        o.method("NSFileManager", sel, [](Cpu& c) { c.ret(guest_exists(c, key_arg(c, 2))); });
    o.method("NSFileManager", "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(key_arg(c, 2));
        std::error_code ec;
        if (host) fs::create_directories(*host, ec);
        c.ret(host.has_value());
    });
    o.method("NSFileManager", "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(url_path(c, c.arg(2)));
        std::error_code ec;
        if (host) fs::create_directories(*host, ec);
        c.ret(host.has_value());
    });
    o.method("NSFileManager", "removeItemAtPath:error:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(key_arg(c, 2));
        std::error_code ec;
        c.ret(host && fs::remove_all(*host, ec) > 0);
    });
    o.method("NSFileManager", "removeItemAtURL:error:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(url_path(c, c.arg(2)));
        std::error_code ec;
        c.ret(host && fs::remove_all(*host, ec) > 0);
    });
    o.method("NSFileManager", "moveItemAtPath:toPath:error:", [](Cpu& c) {
        auto a = c.rt.vfs.to_host(key_arg(c, 2)), b = c.rt.vfs.to_host(key_arg(c, 3));
        std::error_code ec;
        if (a && b) fs::rename(*a, *b, ec);
        c.ret(a && b && !ec);
    });
    o.method("NSFileManager", "copyItemAtPath:toPath:error:", [](Cpu& c) {
        auto a = c.rt.vfs.to_host(key_arg(c, 2)), b = c.rt.vfs.to_host(key_arg(c, 3));
        std::error_code ec;
        if (a && b) fs::copy(*a, *b, fs::copy_options::recursive, ec);
        c.ret(a && b && !ec);
    });
    o.method("NSFileManager", "contentsOfDirectoryAtPath:error:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(key_arg(c, 2));
        std::error_code ec;
        if (!host || !fs::is_directory(*host, ec)) return c.ret(0);
        std::vector<Id> names;
        for (auto& e : fs::directory_iterator(*host, ec))
        {
            auto n = e.path().filename().u8string();
            names.push_back(str(c, std::string(n.begin(), n.end())));
        }
        c.ret(make_array(c, names));
    });
    o.method("NSFileManager", "contentsAtPath:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(key_arg(c, 2));
        std::ifstream f(host ? *host : fs::path(), std::ios::binary);
        if (!host || !f) return c.ret(0);
        c.ret(make_data(c, std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {})));
    });
    o.method("NSFileManager", "createFileAtPath:contents:attributes:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(key_arg(c, 2));
        if (!host) return c.ret(0);
        std::ofstream f(*host, std::ios::binary);
        if (c.arg(3))
        {
            auto p = object_to_plist(c, c.arg(3));
            if (p && p->kind == Plist::Kind::Data) f.write(reinterpret_cast<const char*>(p->data.data()), p->data.size());
        }
        c.ret(bool(f));
    });
    o.method("NSFileManager", "attributesOfItemAtPath:error:", [](Cpu& c) {
        auto host = c.rt.vfs.to_host(key_arg(c, 2));
        std::error_code ec;
        if (!host || !fs::exists(*host, ec)) return c.ret(0);
        NumberData size;
        size.type = 'Q';
        size.u = fs::is_regular_file(*host, ec) ? fs::file_size(*host, ec) : 0;
        Id type = str(c, fs::is_directory(*host, ec) ? "NSFileTypeDirectory" : "NSFileTypeRegular");
        c.ret(make_dict(c, {{str(c, "NSFileSize"), make_number(c, size)}, {str(c, "NSFileType"), type}}));
    });
    o.method("NSFileManager", "attributesOfFileSystemForPath:error:", [](Cpu& c) {
        NumberData total, free_space;
        total.type = free_space.type = 'Q';
        total.u = 64ull << 30;
        free_space.u = 32ull << 30;
        c.ret(make_dict(
            c, {{str(c, "NSFileSystemSize"), make_number(c, total)}, {str(c, "NSFileSystemFreeSize"), make_number(c, free_space)}}));
    });
    o.method("NSFileManager", "URLsForDirectory:inDomains:", [](Cpu& c) {
        std::vector<Id> urls;
        for (auto& p : directory_paths(c, c.arg(2)))
            urls.push_back(make_url(c, p, true));
        c.ret(make_array(c, urls));
    });
    o.method("NSFileManager", "URLForDirectory:inDomain:appropriateForURL:create:error:", [](Cpu& c) {
        std::string p = directory_paths(c, c.arg(2)).front();
        if (c.arg(5) & 1)
        {
            std::error_code ec;
            if (auto host = c.rt.vfs.to_host(p)) fs::create_directories(*host, ec);
        }
        c.ret(make_url(c, p, true));
    });
    o.method("NSFileManager", "temporaryDirectory", [](Cpu& c) { c.ret(make_url(c, c.rt.vfs.home + "/tmp/", true)); });
    o.method("NSFileManager", "currentDirectoryPath", [](Cpu& c) { c.ret(str(c, c.rt.vfs.cwd)); });
    o.method("NSFileManager", "containerURLForSecurityApplicationGroupIdentifier:", [](Cpu& c) { c.ret(0); });
    o.method("NSFileManager", "fileSystemRepresentationWithPath:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(2), "UTF8String")); });
    o.method("NSFileManager", "ubiquityIdentityToken", [](Cpu& c) { c.ret(0); });
}

void register_url(objc::ObjcRuntime& o)
{
    o.class_method("NSURL", "fileURLWithPath:", [](Cpu& c) { c.ret(make_url(c, key_arg(c, 2), true)); });
    o.class_method("NSURL", "fileURLWithPath:isDirectory:", [](Cpu& c) { c.ret(make_url(c, key_arg(c, 2), true)); });
    o.class_method("NSURL", "URLWithString:", [](Cpu& c) {
        if (!c.arg(2)) return c.ret(0);
        std::string s = key_arg(c, 2);
        if (s.starts_with("file://")) return c.ret(make_url(c, s.substr(7), true));
        c.ret(make_url(c, s, false));
    });
    o.class_method("NSURL", "URLWithString:relativeToURL:", [](Cpu& c) {
        std::string base = url_text(c, c.arg(3)), rel = key_arg(c, 2);
        if (!base.empty() && base.back() != '/' && !rel.empty() && rel.front() != '/') base += '/';
        c.ret(make_url(c, base + rel, url_is_file(c, c.arg(3))));
    });
    o.method("NSURL", "initFileURLWithPath:", [](Cpu& c) { c.ret(objc(c).retain(make_url(c, key_arg(c, 2), true))); });
    o.method("NSURL", "initWithString:", [](Cpu& c) { c.ret(objc(c).retain(make_url(c, key_arg(c, 2), false))); });
    o.method("NSURL", "dealloc", [](Cpu& c) {
        objc(c).release(c, c.mem.read<uint64_t>(c.arg(0) + 8));
        objc(c).dispose(c.arg(0));
    });
    o.method("NSURL", "isFileURL", [](Cpu& c) { c.ret(url_is_file(c, c.arg(0))); });
    o.method("NSURL", "path", [](Cpu& c) { c.ret(str(c, url_path(c, c.arg(0)))); });
    o.method("NSURL", "relativePath", [](Cpu& c) { c.ret(str(c, url_path(c, c.arg(0)))); });
    o.method("NSURL", "absoluteString", [](Cpu& c) {
        std::string t = url_text(c, c.arg(0));
        c.ret(str(c, url_is_file(c, c.arg(0)) ? "file://" + t : t));
    });
    o.method("NSURL", "absoluteURL", [](Cpu& c) {});
    o.method("NSURL", "description", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "absoluteString")); });
    o.method("NSURL", "scheme", [](Cpu& c) {
        if (url_is_file(c, c.arg(0))) return c.ret(str(c, "file"));
        std::string t = url_text(c, c.arg(0));
        auto p = t.find(':');
        c.ret(p == std::string::npos ? 0 : str(c, t.substr(0, p)));
    });
    o.method("NSURL", "host", [](Cpu& c) {
        std::string t = url_text(c, c.arg(0));
        auto p = t.find("://");
        if (p == std::string::npos || url_is_file(c, c.arg(0))) return c.ret(0);
        auto end = t.find_first_of("/:?#", p + 3);
        c.ret(str(c, t.substr(p + 3, end == std::string::npos ? std::string::npos : end - p - 3)));
    });
    o.method("NSURL", "query", [](Cpu& c) {
        std::string t = url_text(c, c.arg(0));
        auto q = t.find('?');
        c.ret(q == std::string::npos ? 0 : str(c, t.substr(q + 1)));
    });
    o.method("NSURL", "lastPathComponent", [](Cpu& c) { c.ret(objc(c).send(c, str(c, url_path(c, c.arg(0))), "lastPathComponent")); });
    o.method("NSURL", "pathExtension", [](Cpu& c) { c.ret(objc(c).send(c, str(c, url_path(c, c.arg(0))), "pathExtension")); });
    o.method("NSURL", "URLByAppendingPathComponent:", [](Cpu& c) {
        std::string t = url_text(c, c.arg(0)), add = key_arg(c, 2);
        if (!t.empty() && t.back() != '/') t += '/';
        c.ret(make_url(c, t + add, url_is_file(c, c.arg(0))));
    });
    o.method("NSURL", "URLByAppendingPathComponent:isDirectory:", [](Cpu& c) {
        std::string t = url_text(c, c.arg(0)), add = key_arg(c, 2);
        if (!t.empty() && t.back() != '/') t += '/';
        c.ret(make_url(c, t + add, url_is_file(c, c.arg(0))));
    });
    o.method("NSURL", "URLByAppendingPathExtension:", [](Cpu& c) {
        c.ret(make_url(c, url_text(c, c.arg(0)) + "." + key_arg(c, 2), url_is_file(c, c.arg(0))));
    });
    o.method("NSURL", "URLByDeletingLastPathComponent", [](Cpu& c) {
        std::string t = url_text(c, c.arg(0));
        while (t.size() > 1 && t.back() == '/')
            t.pop_back();
        auto p = t.rfind('/');
        c.ret(make_url(c, p == std::string::npos ? t : t.substr(0, p + 1), url_is_file(c, c.arg(0))));
    });
    o.method("NSURL", "fileSystemRepresentation", [](Cpu& c) { c.ret(objc(c).send(c, str(c, url_path(c, c.arg(0))), "UTF8String")); });
    o.method("NSURL", "getFileSystemRepresentation:maxLength:", [](Cpu& c) {
        std::string p = url_path(c, c.arg(0));
        if (p.size() + 1 > c.arg(3)) return c.ret(0);
        c.mem.write_bytes(c.arg(2), p.c_str(), p.size() + 1);
        c.ret(1);
    });
    o.method("NSURL", "checkResourceIsReachableAndReturnError:", [](Cpu& c) { c.ret(guest_exists(c, url_path(c, c.arg(0)))); });
    o.method("NSURL", "setResourceValue:forKey:error:", [](Cpu& c) { c.ret(1); });
    o.method("NSURL", "isEqual:", [](Cpu& c) {
        Class* k = objc(c).class_of(c.arg(2));
        c.ret(k && k->name == "NSURL" && url_text(c, c.arg(0)) == url_text(c, c.arg(2)));
    });
    o.method("NSURL", "copyWithZone:", [](Cpu& c) { c.ret(objc(c).retain(c.arg(0))); });
}

void register_path_functions(Hle& h)
{
    h.fn("_NSHomeDirectory", [](Cpu& c) { c.ret(str(c, c.rt.vfs.home)); });
    h.fn("_NSTemporaryDirectory", [](Cpu& c) { c.ret(str(c, c.rt.vfs.home + "/tmp/")); });
    h.fn("_NSSearchPathForDirectoriesInDomains", [](Cpu& c) {
        std::vector<Id> out;
        for (auto& p : directory_paths(c, c.arg(0)))
            out.push_back(str(c, p));
        c.ret(make_array(c, out));
    });
    h.fn("_NSStringFromClass", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        c.ret(k ? str(c, k->name) : 0);
    });
    h.fn("_NSClassFromString", [](Cpu& c) {
        Class* k = c.arg(0) ? objc(c).class_named(to_utf8(c, c.arg(0))) : nullptr;
        c.ret(k ? k->addr : 0);
    });
    h.fn("_NSStringFromSelector", [](Cpu& c) { c.ret(c.arg(0) ? str(c, objc(c).sel_name(c.arg(0))) : 0); });
    h.fn("_NSSelectorFromString", [](Cpu& c) { c.ret(c.arg(0) ? objc(c).sel(to_utf8(c, c.arg(0))) : 0); });
    h.fn("_NSStringFromProtocol", [](Cpu& c) { c.ret(str(c, c.mem.read_cstr(c.mem.read<uint64_t>(c.arg(0) + 8)))); });
    h.fn("_NSProtocolFromString", [](Cpu& c) { c.ret(objc(c).protocol_named(to_utf8(c, c.arg(0)))); });
}

}

std::string url_string_path(Cpu& c, Id url)
{
    return url_path(c, url);
}
Id new_file_url(Cpu& c, const std::string& path)
{
    return make_url(c, path, true);
}

void register_system(objc::ObjcRuntime& o)
{
    o.define("NSBundle", "NSObject");
    register_bundle(o);
    register_process_info(o);
    register_defaults(o);
    register_file_manager(o);
    register_url(o);
    register_path_functions(o.rt.hle);
}

}
