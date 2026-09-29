#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/memory.h"
#include "hle/hle.h"

namespace orchard
{
struct Runtime;
struct LoadedImage;
class Cpu;
}

namespace orchard::objc
{
using SEL = GuestAddr;
using Id = GuestAddr;

constexpr uint64_t kIsaMask = 0x0000000ffffffff8ull;

struct Method
{
    SEL sel = 0;
    GuestAddr imp = 0;
    GuestAddr types = 0;
    GuestAddr guest = 0;
};

struct Ivar
{
    std::string name;
    std::string type;
    GuestAddr offset_var = 0;
    GuestAddr guest = 0;
};

struct Property
{
    std::string name;
    std::string attributes;
    GuestAddr guest = 0;
};

struct Class
{
    GuestAddr addr = 0;
    std::string name;
    Class* super = nullptr;
    Class* meta = nullptr;
    Class* instance = nullptr;
    bool is_meta = false;
    bool host = false;
    bool swift = false;
    uint32_t instance_size = 8;
    std::unordered_map<SEL, Method> methods;
    std::vector<Ivar> ivars;
    std::vector<Property> properties;
    std::vector<GuestAddr> protocols;
    const LoadedImage* image = nullptr;
    bool initialized = false;
    int8_t custom_rr = -1;
    bool inert = false;
    std::unordered_map<SEL, GuestAddr> cache;
};

using HostMethod = HleFn;

class ObjcRuntime
{
public:
    explicit ObjcRuntime(orchard::Runtime& rt);

    SEL sel(std::string_view name);
    std::string sel_name(SEL s) const;

    Class* class_at(GuestAddr addr) const;
    Class* class_named(std::string_view name) const;
    Class* class_of(Id obj) const;

    void define(const std::string& name, const std::string& super);
    Class* host_class(const std::string& name);
    void method(const std::string& cls, const std::string& sel, HostMethod fn);
    void class_method(const std::string& cls, const std::string& sel, HostMethod fn);

    void register_image(LoadedImage& img);
    void call_loads(Cpu& cpu, LoadedImage& img);

    GuestAddr lookup(Class* cls, SEL sel);
    GuestAddr resolve_send(Cpu& cpu, Id receiver, SEL sel, Class* start = nullptr);
    Id send(Cpu& cpu, Id receiver, SEL sel, std::initializer_list<uint64_t> args = {});
    Id send(Cpu& cpu, Id receiver, std::string_view sel, std::initializer_list<uint64_t> args = {})
    {
        return send(cpu, receiver, this->sel(sel), args);
    }
    bool responds(Class* cls, SEL sel) { return lookup(cls, sel) != 0; }
    bool is_kind_of(Id obj, Class* cls) const;
    bool is_subclass(Class* cls, Class* of) const;

    Id alloc_instance(Class* cls, uint64_t extra = 0);
    Id retain_entry(Cpu& cpu, Id obj);
    void release_entry(Cpu& cpu, Id obj);
    Id retain(Id obj);
    void release(Cpu& cpu, Id obj);
    Id autorelease(Cpu& cpu, Id obj);
    uint64_t retain_count(Id obj) const;
    GuestAddr autorelease_push(Cpu& cpu);
    void autorelease_pop(Cpu& cpu, GuestAddr token);
    void dispose(Id obj);

    Id weak_load(GuestAddr location);
    void weak_store(GuestAddr location, Id obj);
    void weak_unregister(GuestAddr location);
    void set_associated(Cpu& cpu, Id obj, GuestAddr key, Id value, uint64_t policy);
    Id get_associated(Id obj, GuestAddr key) const;

    GuestAddr method_handle(Class* cls, Method& m);
    GuestAddr ivar_handle(Class* cls, Ivar& iv);

    GuestAddr protocol_named(std::string_view name) const;
    std::pair<Class*, SEL> method_from_handle(GuestAddr handle) const;
    void set_method_imp(Class* cls, SEL sel, GuestAddr imp);

    Class* realize(GuestAddr cls) { return realize_guest_class(cls, nullptr); }

    void add_fallback(std::function<GuestAddr(Cpu&, Class*, SEL)> f) { fallbacks_.push_back(std::move(f)); }

    void alias(GuestAddr addr, Class* cls);

    Class* adopt(GuestAddr cache_class);
    void replace_guest_class(const std::string& name) { replaced_guest_classes_.insert(name); }

    Class* allocate_class_pair(Class* super, const std::string& name, uint64_t extra);
    void register_class_pair(Class* cls);

    orchard::Runtime& rt;

private:
    std::set<std::string> replaced_guest_classes_;
    Class* realize_guest_class(GuestAddr addr, const LoadedImage* img);
    void read_method_list(Class* cls, GuestAddr list);
    void read_ivar_list(Class* cls, GuestAddr list);
    void read_property_list(Class* cls, GuestAddr list);
    void attach_category(GuestAddr cat, const LoadedImage& img);
    void make_guest_class(Class* cls, Class* meta);
    void flush_caches();
    void send_initialize(Cpu& cpu, Class* cls);

    std::unordered_map<std::string, SEL> sels_;
    std::unordered_map<SEL, std::string> sel_names_;
    std::unordered_map<GuestAddr, Class*> by_addr_;
    std::unordered_map<std::string, Class*> by_name_;
    std::unordered_map<std::string, std::string> host_supers_;
    std::deque<Class> classes_;
    std::unordered_map<std::string, GuestAddr> protocols_;
    std::unordered_map<GuestAddr, GuestAddr> protocol_canonical_;
    std::unordered_map<GuestAddr, std::pair<Class*, SEL>> method_handles_;

    std::unordered_map<Id, uint64_t> extra_retains_;
    std::unordered_map<Id, std::unordered_set<GuestAddr>> weak_refs_;
    std::unordered_map<GuestAddr, Id> weak_locations_;
    std::unordered_map<Id, std::map<GuestAddr, Id>> associated_;
    std::map<const LoadedImage*, std::vector<std::pair<Class*, GuestAddr>>> pending_loads_;
    mutable std::recursive_mutex lock_;
    std::vector<std::function<GuestAddr(Cpu&, Class*, SEL)>> fallbacks_;
};

ObjcRuntime& objc(Cpu& cpu);

}
