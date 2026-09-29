#include "objc/runtime.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "loader/cache_images.h"
#include "loader/dyld_cache.h"
#include "loader/linker.h"

namespace orchard::objc
{
namespace
{
constexpr uint32_t RO_META = 1 << 0;
constexpr uint32_t RO_ROOT = 1 << 1;
constexpr uint64_t FAST_DATA_MASK = 0x00007ffffffffff8ull;
constexpr uint32_t kRelativeMethodList = 0x80000000;

constexpr uint64_t kClassSize = 40;
constexpr uint64_t RO_FLAGS = 0, RO_INSTANCE_START = 4, RO_INSTANCE_SIZE = 8, RO_NAME = 24, RO_METHODS = 32, RO_PROTOCOLS = 40,
                   RO_IVARS = 48, RO_PROPERTIES = 64, kRoSize = 72;

const macho::Section* find_section(const LoadedImage& img, std::string_view name)
{
    for (auto& seg : img.macho.segments)
        for (auto& sec : seg.sections)
            if (sec.sectname == name) return &sec;
    return nullptr;
}

template <typename F> void each_pointer(const LoadedImage& img, std::string_view section, Memory& mem, F&& f)
{
    const macho::Section* sec = find_section(img, section);
    if (!sec) return;
    GuestAddr at = sec->addr + img.slide;
    for (uint64_t o = 0; o + 8 <= sec->size; o += 8)
        f(at + o, mem.read<uint64_t>(at + o));
}

}

ObjcRuntime& objc(Cpu& cpu)
{
    return *cpu.rt.objc;
}

void register_nsobject(ObjcRuntime& rt);
void register_foundation(ObjcRuntime& rt);
}
namespace orchard::uikit
{
void register_uikit(objc::ObjcRuntime& o);
}
namespace orchard::objc
{
ObjcRuntime::ObjcRuntime(orchard::Runtime& runtime) : rt(runtime)
{
    host_class("NSObject");
    define("NSProxy", "");
    define("Protocol", "NSObject");

    static constexpr std::string_view kCls = "_OBJC_CLASS_$_", kMeta = "_OBJC_METACLASS_$_";
    rt.hle.data_resolver([](const std::string& name) { return name.starts_with(kCls) || name.starts_with(kMeta); },
                         [this](const std::string& name) -> GuestAddr {
                             if (name.starts_with(kCls)) return host_class(name.substr(kCls.size()))->addr;
                             return host_class(name.substr(kMeta.size()))->meta->addr;
                         });

    register_nsobject(*this);
    register_foundation(*this);
    uikit::register_uikit(*this);
}

SEL ObjcRuntime::sel(std::string_view name)
{
    std::lock_guard g(lock_);
    auto it = sels_.find(std::string(name));
    if (it != sels_.end()) return it->second;
    SEL s = rt.mem.alloc_cstr_region(name);
    sels_.emplace(std::string(name), s);
    sel_names_.emplace(s, std::string(name));
    return s;
}

std::string ObjcRuntime::sel_name(SEL s) const
{
    std::lock_guard g(lock_);
    auto it = sel_names_.find(s);
    if (it != sel_names_.end()) return it->second;
    return s ? rt.mem.read_cstr(s) : "<null selector>";
}

Class* ObjcRuntime::class_at(GuestAddr addr) const
{
    std::lock_guard g(lock_);
    auto it = by_addr_.find(addr & kIsaMask);
    return it == by_addr_.end() ? nullptr : it->second;
}

Class* ObjcRuntime::class_named(std::string_view name) const
{
    std::lock_guard g(lock_);
    auto it = by_name_.find(std::string(name));
    return it == by_name_.end() ? nullptr : it->second;
}

Class* ObjcRuntime::class_of(Id obj) const
{
    if (!obj || (obj >> 63)) return nullptr;
    if (!rt.mem.is_mapped(obj, 8)) return nullptr;
    return class_at(rt.mem.read<uint64_t>(obj));
}

void ObjcRuntime::define(const std::string& name, const std::string& super)
{
    host_supers_[name] = super;
    host_class(name);
}

void ObjcRuntime::make_guest_class(Class* cls, Class* meta)
{
    Class* root_meta = meta;
    for (Class* c = meta; c; c = c->super)
        if (c->is_meta) root_meta = c;

    auto build = [&](Class* c, GuestAddr isa, GuestAddr super, uint32_t flags) {
        GuestAddr ro = rt.mem.alloc_system(kRoSize, 8);
        rt.mem.write<uint32_t>(ro + RO_FLAGS, flags);
        rt.mem.write<uint32_t>(ro + RO_INSTANCE_START, c->instance_size);
        rt.mem.write<uint32_t>(ro + RO_INSTANCE_SIZE, c->instance_size);
        rt.mem.write<uint64_t>(ro + RO_NAME, rt.mem.alloc_cstr_region(c->name));
        rt.mem.write<uint64_t>(c->addr, isa);
        rt.mem.write<uint64_t>(c->addr + 8, super);
        rt.mem.write<uint64_t>(c->addr + 32, ro);
    };
    bool root = !cls->super;
    build(cls, meta->addr, root ? 0 : cls->super->addr, root ? RO_ROOT : 0);
    build(meta, root ? meta->addr : root_meta->addr, root ? cls->addr : meta->super->addr, RO_META | (root ? RO_ROOT : 0));
}

Class* ObjcRuntime::host_class(const std::string& name)
{
    std::lock_guard g(lock_);
    if (auto* c = class_named(name)) return c;

    std::string super_name = "NSObject";
    if (auto it = host_supers_.find(name); it != host_supers_.end()) super_name = it->second;
    if (name == "NSObject") super_name.clear();
    Class* super = super_name.empty() ? nullptr : host_class(super_name);

    Class& cls = classes_.emplace_back();
    Class& meta = classes_.emplace_back();
    cls.name = meta.name = name;
    cls.host = meta.host = true;
    cls.super = super;
    cls.meta = &meta;
    meta.is_meta = true;
    meta.instance = &cls;
    meta.super = super ? super->meta : &cls;
    cls.instance_size = super ? super->instance_size : 8;
    cls.initialized = true;
    cls.addr = rt.mem.alloc_system(kClassSize, 16);
    meta.addr = rt.mem.alloc_system(kClassSize, 16);
    make_guest_class(&cls, &meta);

    by_addr_[cls.addr] = &cls;
    by_addr_[meta.addr] = &meta;
    by_name_[name] = &cls;
    return &cls;
}

void ObjcRuntime::method(const std::string& cls, const std::string& s, HostMethod fn)
{
    Class* c = host_class(cls);
    SEL se = sel(s);
    c->methods[se] = {se, rt.hle.make_stub("-[" + cls + " " + s + "]", fn), 0, 0};
}

void ObjcRuntime::class_method(const std::string& cls, const std::string& s, HostMethod fn)
{
    Class* c = host_class(cls)->meta;
    SEL se = sel(s);
    c->methods[se] = {se, rt.hle.make_stub("+[" + cls + " " + s + "]", fn), 0, 0};
}

bool ObjcRuntime::is_subclass(Class* cls, Class* of) const
{
    for (Class* c = cls; c; c = c->super)
        if (c == of) return true;
    return false;
}

bool ObjcRuntime::is_kind_of(Id obj, Class* cls) const
{
    return is_subclass(class_of(obj), cls);
}

void ObjcRuntime::read_method_list(Class* cls, GuestAddr list)
{
    if (!list) return;
    uint32_t entsize_flags = rt.mem.read<uint32_t>(list);
    uint32_t count = rt.mem.read<uint32_t>(list + 4);
    bool relative = entsize_flags & kRelativeMethodList;
    uint32_t entsize = entsize_flags & 0xfffc;
    GuestAddr selector_base = 0;
    if (relative && rt.cache_images && rt.cache_images->cache().contains(list)) selector_base = rt.cache_images->relative_selector_base();
    for (uint32_t i = 0; i < count; ++i)
    {
        GuestAddr e = list + 8 + uint64_t(i) * entsize;
        Method m;
        if (relative)
        {
            int32_t name_off = rt.mem.read<int32_t>(e);
            GuestAddr name = selector_base ? selector_base + int64_t(name_off) : rt.mem.read<uint64_t>(e + int64_t(name_off));
            m.sel = sel(rt.mem.read_cstr(name));
            m.types = e + 4 + int64_t(rt.mem.read<int32_t>(e + 4));
            int32_t imp_off = rt.mem.read<int32_t>(e + 8);
            m.imp = imp_off ? e + 8 + int64_t(imp_off) : 0;
        }
        else
        {
            m.sel = sel(rt.mem.read_cstr(rt.mem.read<uint64_t>(e)));
            m.types = rt.mem.read<uint64_t>(e + 8);
            m.imp = rt.mem.read<uint64_t>(e + 16);
        }
        cls->methods[m.sel] = m;
    }
}

void ObjcRuntime::read_ivar_list(Class* cls, GuestAddr list)
{
    if (!list) return;
    uint32_t entsize = rt.mem.read<uint32_t>(list);
    uint32_t count = rt.mem.read<uint32_t>(list + 4);
    for (uint32_t i = 0; i < count; ++i)
    {
        GuestAddr e = list + 8 + uint64_t(i) * entsize;
        Ivar iv;
        iv.offset_var = rt.mem.read<uint64_t>(e);
        GuestAddr name = rt.mem.read<uint64_t>(e + 8), type = rt.mem.read<uint64_t>(e + 16);
        if (name) iv.name = rt.mem.read_cstr(name);
        if (type) iv.type = rt.mem.read_cstr(type);
        cls->ivars.push_back(std::move(iv));
    }
}

void ObjcRuntime::read_property_list(Class* cls, GuestAddr list)
{
    if (!list) return;
    uint32_t entsize = rt.mem.read<uint32_t>(list);
    uint32_t count = rt.mem.read<uint32_t>(list + 4);
    for (uint32_t i = 0; i < count; ++i)
    {
        GuestAddr e = list + 8 + uint64_t(i) * entsize;
        Property p;
        GuestAddr name = rt.mem.read<uint64_t>(e), attrs = rt.mem.read<uint64_t>(e + 8);
        if (name) p.name = rt.mem.read_cstr(name);
        if (attrs) p.attributes = rt.mem.read_cstr(attrs);
        cls->properties.push_back(std::move(p));
    }
}

namespace
{
bool in_replaced_cache_dylib(orchard::Runtime& rt, GuestAddr addr)
{
    if (!rt.cache_images || !rt.cache_images->cache().contains(addr)) return false;
    auto* d = rt.cache_images->dylib_containing(addr);
    return !d || !d->real;
}
}

void ObjcRuntime::alias(GuestAddr addr, Class* cls)
{
    std::lock_guard g(lock_);
    by_addr_[addr & kIsaMask] = cls;
    GuestAddr meta = rt.mem.read<uint64_t>(addr) & kIsaMask;
    if (meta && !by_addr_.count(meta)) by_addr_[meta] = cls->meta;
}

Class* ObjcRuntime::adopt(GuestAddr cache_class)
{
    std::lock_guard g(lock_);
    if (Class* c = class_at(cache_class)) return c;
    GuestAddr meta = rt.mem.read<uint64_t>(cache_class) & kIsaMask;
    GuestAddr ro = rt.mem.read<uint64_t>(cache_class + 32) & FAST_DATA_MASK;
    std::string name = rt.mem.read_cstr(rt.mem.read<uint64_t>(ro + RO_NAME));
    Class* host = host_class(name);
    by_addr_[cache_class & kIsaMask] = host;
    by_addr_[meta] = host->meta;
    return host;
}

namespace
{
bool inert_sdk_class(const std::string& name)
{
    static const char* kPrefixes[] = {"GAD", "IS",  "OMID", "LPM", "SML", "IA",  "AL",  "UADS", "FIR", "GUL", "APM", "GDT",
                                      "IM",  "VNG", "MA",   "TJ",  "FB",  "SDL", "PAG", "BU",   "MTG", "APD", "CHB", "HBS"};
    for (const char* p : kPrefixes)
    {
        size_t n = std::strlen(p);
        if (name.size() > n + 1 && name.starts_with(p) && std::isupper(uint8_t(name[n])) && std::islower(uint8_t(name[n + 1]))) return true;
        if (name.size() > n + 1 && name.starts_with(p) && std::isupper(uint8_t(name[n])) && std::isupper(uint8_t(name[n + 1])) &&
            std::string_view(p).size() >= 3)
            return true;
    }
    return name.starts_with("Tenjin") || name.starts_with("Vungle") || name.starts_with("InMobi") || name.starts_with("AppLovin") ||
           name.starts_with("UnityAds") || name.starts_with("Fyber") || name.starts_with("IronSource");
}
}

Class* ObjcRuntime::realize_guest_class(GuestAddr addr, const LoadedImage* img)
{
    if (!addr) return nullptr;
    if (Class* c = class_at(addr)) return c;
    if (in_replaced_cache_dylib(rt, addr)) return adopt(addr);
    if (!replaced_guest_classes_.empty())
    {
        GuestAddr ro = rt.mem.read<uint64_t>(addr + 32) & FAST_DATA_MASK;
        std::string name = rt.mem.read_cstr(rt.mem.read<uint64_t>(ro + RO_NAME));
        if (replaced_guest_classes_.count(name)) return adopt(addr);
    }
    std::lock_guard g(lock_);

    GuestAddr meta_addr = rt.mem.read<uint64_t>(addr);
    GuestAddr super_addr = rt.mem.read<uint64_t>(addr + 8);
    Class* super = realize_guest_class(super_addr, nullptr);
    if (Class* c = class_at(addr)) return c;

    Class& cls = classes_.emplace_back();
    Class& meta = classes_.emplace_back();
    cls.addr = addr;
    meta.addr = meta_addr;
    cls.meta = &meta;
    meta.instance = &cls;
    meta.is_meta = true;
    cls.super = super;
    by_addr_[addr] = &cls;
    by_addr_[meta_addr] = &meta;
    if (!img && rt.linker) img = rt.linker->image_containing(addr);
    cls.image = meta.image = img;

    uint64_t bits = rt.mem.read<uint64_t>(addr + 32);
    cls.swift = (bits & 3) != 0;
    GuestAddr ro = bits & FAST_DATA_MASK;
    cls.instance_size = rt.mem.read<uint32_t>(ro + RO_INSTANCE_SIZE);
    cls.name = meta.name = rt.mem.read_cstr(rt.mem.read<uint64_t>(ro + RO_NAME));
    read_method_list(&cls, rt.mem.read<uint64_t>(ro + RO_METHODS));
    read_ivar_list(&cls, rt.mem.read<uint64_t>(ro + RO_IVARS));
    read_property_list(&cls, rt.mem.read<uint64_t>(ro + RO_PROPERTIES));
    if (GuestAddr protos = rt.mem.read<uint64_t>(ro + RO_PROTOCOLS))
    {
        uint64_t n = rt.mem.read<uint64_t>(protos);
        for (uint64_t i = 0; i < n; ++i)
            cls.protocols.push_back(rt.mem.read<uint64_t>(protos + 8 + i * 8));
    }

    GuestAddr meta_ro = rt.mem.read<uint64_t>(meta_addr + 32) & FAST_DATA_MASK;
    read_method_list(&meta, rt.mem.read<uint64_t>(meta_ro + RO_METHODS));
    GuestAddr meta_super = rt.mem.read<uint64_t>(meta_addr + 8);
    meta.super = class_at(meta_super);
    if (!meta.super) meta.super = super ? super->meta : &cls;

    cls.inert = meta.inert = !rt.run_all_frameworks && inert_sdk_class(cls.name);
    by_name_.emplace(cls.name, &cls);
    return &cls;
}

void ObjcRuntime::attach_category(GuestAddr cat, const LoadedImage& img)
{
    GuestAddr cls_addr = rt.mem.read<uint64_t>(cat + 8);
    Class* cls = class_at(cls_addr);
    if (!cls) cls = realize_guest_class(cls_addr, &img);
    if (!cls) return;
    read_method_list(cls, rt.mem.read<uint64_t>(cat + 16));
    read_method_list(cls->meta, rt.mem.read<uint64_t>(cat + 24));
    read_property_list(cls, rt.mem.read<uint64_t>(cat + 40));
    if (GuestAddr protos = rt.mem.read<uint64_t>(cat + 32))
    {
        uint64_t n = rt.mem.read<uint64_t>(protos);
        for (uint64_t i = 0; i < n; ++i)
            cls->protocols.push_back(rt.mem.read<uint64_t>(protos + 8 + i * 8));
    }
}

void ObjcRuntime::register_image(LoadedImage& img)
{
    std::lock_guard g(lock_);
    Memory& mem = rt.mem;

    each_pointer(img, "__objc_selrefs", mem, [&](GuestAddr at, uint64_t str) { mem.write<uint64_t>(at, sel(mem.read_cstr(str))); });
    if (img.from_cache)
    {
        for (const char* sec : {"__objc_classrefs", "__objc_superrefs"})
            each_pointer(img, sec, mem, [&](GuestAddr, uint64_t c) {
                if (in_replaced_cache_dylib(rt, c)) adopt(c);
            });
    }

    GuestAddr protocol_isa = host_class("Protocol")->addr;
    each_pointer(img, "__objc_protolist", mem, [&](GuestAddr, uint64_t p) {
        std::string name = mem.read_cstr(mem.read<uint64_t>(p + 8));
        auto [it, fresh] = protocols_.emplace(name, p);
        protocol_canonical_[p] = it->second;
        if (fresh) mem.write<uint64_t>(p, protocol_isa);
    });
    each_pointer(img, "__objc_protorefs", mem, [&](GuestAddr at, uint64_t p) {
        if (auto it = protocol_canonical_.find(p); it != protocol_canonical_.end()) mem.write<uint64_t>(at, it->second);
    });

    each_pointer(img, "__objc_classlist", mem, [&](GuestAddr, uint64_t c) { realize_guest_class(c, &img); });
    each_pointer(img, "__objc_catlist", mem, [&](GuestAddr, uint64_t cat) { attach_category(cat, img); });
    each_pointer(img, "__objc_catlist2", mem, [&](GuestAddr, uint64_t cat) { attach_category(cat, img); });

    SEL load = sel("load");
    auto& loads = pending_loads_[&img];
    each_pointer(img, "__objc_nlclslist", mem, [&](GuestAddr, uint64_t c) {
        Class* cls = realize_guest_class(c, &img);
        if (cls->inert) return;
        if (auto it = cls->meta->methods.find(load); it != cls->meta->methods.end()) loads.emplace_back(cls, it->second.imp);
    });
    each_pointer(img, "__objc_nlcatlist", mem, [&](GuestAddr, uint64_t cat) {
        Class* cls = class_at(mem.read<uint64_t>(cat + 8));
        GuestAddr list = mem.read<uint64_t>(cat + 24);
        if (!cls || !list || cls->inert) return;
        if (inert_sdk_class(mem.read_cstr(mem.read<uint64_t>(cat))) && !rt.run_all_frameworks) return;
        Class scratch;
        read_method_list(&scratch, list);
        if (auto it = scratch.methods.find(load); it != scratch.methods.end()) loads.emplace_back(cls, it->second.imp);
    });
    flush_caches();
}

void ObjcRuntime::call_loads(Cpu& cpu, LoadedImage& img)
{
    std::vector<std::pair<Class*, GuestAddr>> loads;
    {
        std::lock_guard g(lock_);
        auto it = pending_loads_.find(&img);
        if (it == pending_loads_.end()) return;
        loads = std::move(it->second);
        pending_loads_.erase(it);
    }
    SEL load = sel("load");
    for (auto& [cls, imp] : loads)
    {
        if (cpu.stopped()) return;
        cpu.call(imp, {cls->addr, load});
    }
}

void ObjcRuntime::flush_caches()
{
    for (auto& c : classes_)
    {
        c.cache.clear();
        c.custom_rr = -1;
    }
}

GuestAddr ObjcRuntime::lookup(Class* cls, SEL s)
{
    std::lock_guard g(lock_);
    if (!cls) return 0;
    if (auto it = cls->cache.find(s); it != cls->cache.end()) return it->second;
    GuestAddr imp = 0;
    for (Class* c = cls; c; c = c->super)
    {
        auto it = c->methods.find(s);
        if (it != c->methods.end())
        {
            imp = it->second.imp;
            break;
        }
    }
    if (imp) cls->cache[s] = imp;
    return imp;
}

void ObjcRuntime::send_initialize(Cpu& cpu, Class* cls)
{
    if (!cls || cls->initialized) return;
    cls->initialized = true;
    send_initialize(cpu, cls->super);
    SEL init = sel("initialize");
    GuestAddr imp = lookup(cls->meta, init);
    Class* root = class_named("NSObject");
    if (imp && !(root && root->meta->methods.count(init) && root->meta->methods[init].imp == imp)) cpu.call(imp, {cls->addr, init});
}

namespace
{
bool permissive_class(const std::string& name)
{
    static const char* kPrefixes[] = {"UN",
                                      "SK",
                                      "GK",
                                      "ASIdentifier",
                                      "ATTracking",
                                      "CT",
                                      "AA",
                                      "CH",
                                      "MX",
                                      "CL",
                                      "CM",
                                      "GC",
                                      "WK",
                                      "SF",
                                      "MF",
                                      "AD",
                                      "NSUbiquitous",
                                      "UIPasteboard",
                                      "UIImpactFeedback",
                                      "UINotificationFeedback",
                                      "UISelectionFeedback",
                                      "NSUserActivity",
                                      "UIAccessibility",
                                      "SLCompose",
                                      "MPVolume",
                                      "MPRemote",
                                      "MPNowPlaying",
                                      "JSContext",
                                      "JSVirtualMachine",
                                      "NSURLSession",
                                      "NSURLCache",
                                      "NSHTTPCookie",
                                      "LA",
                                      "CK",
                                      "PH",
                                      "UIActivityViewController",
                                      "UITextView",
                                      "UITextField",
                                      "UIToolbar",
                                      "UIBarButtonItem",
                                      "UIActivityIndicatorView",
                                      "UILabel",
                                      "UIButton",
                                      "UIAlert",
                                      "UINavigation",
                                      "UIKeyCommand",
                                      "UIFont",
                                      "UIImage",
                                      "UIGestureRecognizer",
                                      "UITapGestureRecognizer",
                                      "UIPanGestureRecognizer",
                                      "UIPinchGestureRecognizer",
                                      "UISwipeGestureRecognizer",
                                      "UILongPressGestureRecognizer",
                                      "UIRotationGestureRecognizer",
                                      "UIStackView",
                                      "UIScrollView",
                                      "UIProgressView",
                                      "UIVisualEffectView",
                                      "UIBlurEffect",
                                      "UIVibrancyEffect",
                                      "UIStoryboard",
                                      "UITextInputMode",
                                      "UIPointer",
                                      "UIViewPropertyAnimator",
                                      "UIEventAttribution",
                                      "UIControl",
                                      "UISlider",
                                      "UITableView",
                                      "UICollectionView",
                                      "UIPageViewController",
                                      "UISplitViewController",
                                      "UITabBarController",
                                      "UIGraphicsImageRenderer",
                                      "UIUserNotificationSettings",
                                      "NSLayout",
                                      "UILayoutGuide",
                                      "AVPlayer",
                                      "AVAsset",
                                      "AVURLAsset",
                                      "AVMutableAudioMix",
                                      "AVAudioPlayer",
                                      "AVAudioEngine"};
    for (const char* p : kPrefixes)
        if (name == p || (name.starts_with(p) && name.size() > std::strlen(p) && std::isupper(uint8_t(name[std::strlen(p)])))) return true;
    return false;
}
}

GuestAddr ObjcRuntime::resolve_send(Cpu& cpu, Id receiver, SEL s, Class* start)
{
    if (!receiver) return rt.hle.ret_instruction();
    Class* cls = start ? start : class_of(receiver);
    if (!cls)
    {
        char buf[200];
        std::snprintf(buf, sizeof buf, "message -%s sent to 0x%llx whose isa 0x%llx is not a known class", sel_name(s).c_str(),
                      (unsigned long long)receiver,
                      (unsigned long long)(rt.mem.is_mapped(receiver, 8) ? rt.mem.read<uint64_t>(receiver) : 0));
        cpu.stop(buf);
        return 0;
    }
    if (cls->inert || (cls->image && cls->image->inert))
    {
        static std::unordered_set<std::string> seen;
        if (seen.insert(cls->name).second) std::fprintf(stderr, "[objc] %s is inert; messages return nil\n", cls->name.c_str());
        return rt.hle.ret_instruction();
    }
    send_initialize(cpu, cls->is_meta ? cls->instance : cls);
    if (cpu.stopped()) return 0;
    if (GuestAddr imp = lookup(cls, s)) return imp;

    Class* meta = cls->is_meta ? cls : cls->meta;
    Id class_obj = cls->is_meta ? receiver : (cls->addr);
    SEL resolver = sel(cls->is_meta ? "resolveClassMethod:" : "resolveInstanceMethod:");
    if (GuestAddr r = lookup(meta, resolver))
    {
        if (cpu.call(r, {class_obj, resolver, s}) & 1)
        {
            if (GuestAddr imp = lookup(cls, s)) return imp;
        }
    }

    for (auto& f : fallbacks_)
        if (GuestAddr imp = f(cpu, cls, s)) return imp;

    std::string kind = cls->is_meta ? "+" : "-";
    std::string what = kind + "[" + cls->name + " " + sel_name(s) + "]";
    bool host = false;
    std::string host_name;
    for (Class* c = cls; c; c = c->super)
    {
        host |= c->host && c->name != "NSObject";
        if (c->host && host_name.empty() && c->name != "NSObject") host_name = c->name;
    }

    if (permissive_class(host_name))
    {
        static std::mutex seen_lock;
        static std::unordered_set<std::string> seen;
        {
            std::lock_guard g(seen_lock);
            if (seen.insert(what).second) std::fprintf(stderr, "[objc] %s stubbed (permissive %s)\n", what.c_str(), host_name.c_str());
        }
        if (cls->is_meta && sel_name(s).find(':') == std::string::npos)
        {
            static GuestAddr shared = rt.hle.make_stub("permissive shared instance", [](Cpu& k) {
                static std::mutex m;
                static std::unordered_map<GuestAddr, GuestAddr> instances;
                std::lock_guard g(m);
                GuestAddr& inst = instances[k.arg(0)];
                if (!inst) inst = objc(k).alloc_instance(objc(k).class_at(k.arg(0)));
                k.ret(inst);
            });
            return shared;
        }
        return rt.hle.ret_instruction();
    }
    std::string msg = (host ? "unimplemented " : "unrecognized selector ") + what + " called from " + rt.describe(cpu.lr());
    if (!rt.lenient)
    {
        cpu.stop(msg);
        return 0;
    }
    static std::unordered_set<std::string> seen;
    if (seen.insert(what).second) std::fprintf(stderr, "[objc] %s, returning nil\n", msg.c_str());
    return rt.hle.ret_instruction();
}

Id ObjcRuntime::send(Cpu& cpu, Id receiver, SEL s, std::initializer_list<uint64_t> args)
{
    GuestAddr imp = resolve_send(cpu, receiver, s);
    if (!imp || imp == rt.hle.ret_instruction()) return 0;
    std::vector<uint64_t> all{receiver, s};
    all.insert(all.end(), args.begin(), args.end());
    switch (all.size())
    {
    case 2: return cpu.call(imp, {all[0], all[1]});
    case 3: return cpu.call(imp, {all[0], all[1], all[2]});
    case 4: return cpu.call(imp, {all[0], all[1], all[2], all[3]});
    case 5: return cpu.call(imp, {all[0], all[1], all[2], all[3], all[4]});
    case 6: return cpu.call(imp, {all[0], all[1], all[2], all[3], all[4], all[5]});
    case 7: return cpu.call(imp, {all[0], all[1], all[2], all[3], all[4], all[5], all[6]});
    default: return cpu.call(imp, {all[0], all[1], all[2], all[3], all[4], all[5], all[6], all[7]});
    }
}

Id ObjcRuntime::alloc_instance(Class* cls, uint64_t extra)
{
    uint64_t size = std::max<uint64_t>(cls->instance_size, 8) + extra;
    Id obj = rt.heap.calloc(size);
    rt.mem.write<uint64_t>(obj, cls->addr);
    return obj;
}

namespace
{
bool has_custom_rr(ObjcRuntime& o, Class* cls)
{
    if (cls->custom_rr < 0)
    {
        Class* root = o.class_named("NSObject");
        SEL r = o.sel("retain"), rel = o.sel("release");
        cls->custom_rr = o.lookup(cls, r) != o.lookup(root, r) || o.lookup(cls, rel) != o.lookup(root, rel);
    }
    return cls->custom_rr > 0;
}
}

Id ObjcRuntime::retain_entry(Cpu& cpu, Id obj)
{
    Class* cls = class_of(obj);
    if (cls && has_custom_rr(*this, cls)) return send(cpu, obj, "retain");
    return retain(obj);
}

void ObjcRuntime::release_entry(Cpu& cpu, Id obj)
{
    Class* cls = class_of(obj);
    if (cls && has_custom_rr(*this, cls))
    {
        send(cpu, obj, "release");
        return;
    }
    release(cpu, obj);
}

Id ObjcRuntime::retain(Id obj)
{
    if (!obj || (obj >> 63)) return obj;
    std::lock_guard g(lock_);
    ++extra_retains_[obj];
    return obj;
}

uint64_t ObjcRuntime::retain_count(Id obj) const
{
    std::lock_guard g(lock_);
    auto it = extra_retains_.find(obj);
    return 1 + (it == extra_retains_.end() ? 0 : it->second);
}

void ObjcRuntime::release(Cpu& cpu, Id obj)
{
    if (!obj || (obj >> 63)) return;
    {
        std::lock_guard g(lock_);
        auto it = extra_retains_.find(obj);
        if (it != extra_retains_.end())
        {
            if (--it->second == 0) extra_retains_.erase(it);
            return;
        }
    }
    if (!rt.heap.size_of(obj)) return;
    send(cpu, obj, "dealloc");
}

Id ObjcRuntime::autorelease(Cpu& cpu, Id obj)
{
    if (obj) cpu.autorelease_pool.push_back(obj);
    return obj;
}

GuestAddr ObjcRuntime::autorelease_push(Cpu& cpu)
{
    return (cpu.autorelease_pool.size() + 1) | (uint64_t(0xa7) << 40);
}

void ObjcRuntime::autorelease_pop(Cpu& cpu, GuestAddr token)
{
    size_t keep = size_t((token & 0xffffffffffull) - 1);
    while (cpu.autorelease_pool.size() > keep && !cpu.stopped())
    {
        Id obj = cpu.autorelease_pool.back();
        cpu.autorelease_pool.pop_back();
        release_entry(cpu, obj);
    }
}

void ObjcRuntime::dispose(Id obj)
{
    {
        std::lock_guard g(lock_);
        if (auto it = weak_refs_.find(obj); it != weak_refs_.end())
        {
            for (GuestAddr loc : it->second)
            {
                rt.mem.write<uint64_t>(loc, 0);
                weak_locations_.erase(loc);
            }
            weak_refs_.erase(it);
        }
        associated_.erase(obj);
        extra_retains_.erase(obj);
    }
    rt.heap.free(obj);
}

Id ObjcRuntime::weak_load(GuestAddr location)
{
    std::lock_guard g(lock_);
    return rt.mem.read<uint64_t>(location);
}

void ObjcRuntime::weak_unregister(GuestAddr location)
{
    std::lock_guard g(lock_);
    auto it = weak_locations_.find(location);
    if (it == weak_locations_.end()) return;
    if (auto w = weak_refs_.find(it->second); w != weak_refs_.end()) w->second.erase(location);
    weak_locations_.erase(it);
}

void ObjcRuntime::weak_store(GuestAddr location, Id obj)
{
    std::lock_guard g(lock_);
    weak_unregister(location);
    rt.mem.write<uint64_t>(location, obj);
    if (obj && !(obj >> 63))
    {
        weak_refs_[obj].insert(location);
        weak_locations_[location] = obj;
    }
}

void ObjcRuntime::set_associated(Cpu& cpu, Id obj, GuestAddr key, Id value, uint64_t policy)
{
    Id old = 0;
    {
        std::lock_guard g(lock_);
        auto& m = associated_[obj];
        if (auto it = m.find(key); it != m.end()) old = it->second;
        if (value)
            m[key] = value;
        else
            m.erase(key);
    }
    if (policy != 0 && value)
    {
        if (policy == 3 || policy == 0x303)
            value = send(cpu, value, "copy");
        else
            retain(value);
        std::lock_guard g(lock_);
        associated_[obj][key] = value;
    }
    if (old && policy != 0) release(cpu, old);
}

Id ObjcRuntime::get_associated(Id obj, GuestAddr key) const
{
    std::lock_guard g(lock_);
    auto it = associated_.find(obj);
    if (it == associated_.end()) return 0;
    auto k = it->second.find(key);
    return k == it->second.end() ? 0 : k->second;
}

GuestAddr ObjcRuntime::method_handle(Class* cls, Method& m)
{
    std::lock_guard g(lock_);
    if (!m.guest)
    {
        m.guest = rt.mem.alloc_system(24, 8);
        method_handles_[m.guest] = {cls, m.sel};
    }
    rt.mem.write<uint64_t>(m.guest, m.sel);
    rt.mem.write<uint64_t>(m.guest + 8, m.types);
    rt.mem.write<uint64_t>(m.guest + 16, m.imp);
    return m.guest;
}

std::pair<Class*, SEL> ObjcRuntime::method_from_handle(GuestAddr handle) const
{
    std::lock_guard g(lock_);
    auto it = method_handles_.find(handle);
    return it == method_handles_.end() ? std::pair<Class*, SEL>{nullptr, 0} : it->second;
}

void ObjcRuntime::set_method_imp(Class* cls, SEL s, GuestAddr imp)
{
    std::lock_guard g(lock_);
    auto& m = cls->methods[s];
    m.sel = s;
    m.imp = imp;
    if (m.guest) rt.mem.write<uint64_t>(m.guest + 16, imp);
    flush_caches();
}

GuestAddr ObjcRuntime::ivar_handle(Class* cls, Ivar& iv)
{
    std::lock_guard g(lock_);
    if (!iv.guest)
    {
        iv.guest = rt.mem.alloc_system(32, 8);
        rt.mem.write<uint64_t>(iv.guest, iv.offset_var);
        rt.mem.write<uint64_t>(iv.guest + 8, rt.mem.alloc_cstr_region(iv.name));
        rt.mem.write<uint64_t>(iv.guest + 16, rt.mem.alloc_cstr_region(iv.type));
    }
    (void)cls;
    return iv.guest;
}

GuestAddr ObjcRuntime::protocol_named(std::string_view name) const
{
    std::lock_guard g(lock_);
    auto it = protocols_.find(std::string(name));
    return it == protocols_.end() ? 0 : it->second;
}

Class* ObjcRuntime::allocate_class_pair(Class* super, const std::string& name, uint64_t extra)
{
    std::lock_guard g(lock_);
    if (class_named(name)) return nullptr;
    Class& cls = classes_.emplace_back();
    Class& meta = classes_.emplace_back();
    cls.name = meta.name = name;
    cls.super = super;
    cls.meta = &meta;
    meta.is_meta = true;
    meta.instance = &cls;
    meta.super = super ? super->meta : &cls;
    cls.instance_size = super ? super->instance_size : 8;
    cls.addr = rt.mem.alloc_system(kClassSize + extra, 16);
    meta.addr = rt.mem.alloc_system(kClassSize + extra, 16);
    make_guest_class(&cls, &meta);
    by_addr_[cls.addr] = &cls;
    by_addr_[meta.addr] = &meta;
    return &cls;
}

void ObjcRuntime::register_class_pair(Class* cls)
{
    std::lock_guard g(lock_);
    by_name_.emplace(cls->name, cls);
}

}
