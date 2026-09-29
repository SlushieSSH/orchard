#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/libSystem/blocks.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::Class;
using objc::objc;

namespace
{
constexpr uint64_t kNotFound = 0x7fffffffffffffffull;

bool class_is(Cpu& c, Id obj, const char* name)
{
    Class* k = objc(c).class_of(obj);
    return k && k->name == name;
}

Id retain(Cpu& c, Id o)
{
    return objc(c).retain_entry(c, o);
}
void release(Cpu& c, Id o)
{
    objc(c).release_entry(c, o);
}
Id autorelease(Cpu& c, Id o)
{
    return objc(c).autorelease(c, o);
}
Id copy_key(Cpu& c, Id k)
{
    return is_string(c, k) ? objc(c).send(c, k, "copy") : retain(c, k);
}

Id new_of(Cpu& c, const char* cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}

Id alloc_cluster(Cpu& c, const char* immutable_cls, const char* mutable_cls, const char* mutable_abstract)
{
    Class* k = objc(c).class_at(c.arg(0));
    if (!k || !k->host) return objc(c).alloc_instance(k);
    bool mut = objc(c).is_subclass(k, objc(c).class_named(mutable_abstract));
    return new_of(c, mut ? mutable_cls : immutable_cls);
}

std::vector<Id> variadic_objects(Cpu& c, Id first)
{
    std::vector<Id> out;
    if (!first) return out;
    out.push_back(first);
    for (GuestAddr p = c.sp();; p += 8)
    {
        Id o = c.mem.read<uint64_t>(p);
        if (!o) break;
        out.push_back(o);
    }
    return out;
}

std::vector<Id> read_ids(Cpu& c, GuestAddr p, uint64_t n)
{
    std::vector<Id> v(n);
    for (uint64_t i = 0; i < n; ++i)
        v[i] = c.mem.read<uint64_t>(p + i * 8);
    return v;
}

void fill_array(Cpu& c, Id self, const std::vector<Id>& items)
{
    auto& a = store().get<ArrayData>(self);
    for (Id o : a.items)
        release(c, o);
    a.items.clear();
    for (Id o : items)
        a.items.push_back(retain(c, o));
}

void dict_set(Cpu& c, Id self, Id key, Id value)
{
    auto& d = store().get<DictData>(self);
    for (auto& e : d.entries)
    {
        if (objects_equal(c, e.first, key))
        {
            Id old = e.second;
            e.second = retain(c, value);
            release(c, old);
            return;
        }
    }
    d.entries.emplace_back(copy_key(c, key), retain(c, value));
}

void dict_remove(Cpu& c, Id self, Id key)
{
    auto& d = store().get<DictData>(self);
    for (size_t i = 0; i < d.entries.size(); ++i)
    {
        if (objects_equal(c, d.entries[i].first, key))
        {
            release(c, d.entries[i].first);
            release(c, d.entries[i].second);
            d.entries.erase(d.entries.begin() + i);
            return;
        }
    }
}

void release_contents(Cpu& c, Id obj)
{
    auto data = store().take(obj);
    if (!data) return;
    if (auto* a = std::get_if<ArrayData>(data.get()))
        for (Id o : a->items)
            release(c, o);
    if (auto* s = std::get_if<SetData>(data.get()))
        for (Id o : s->items)
            release(c, o);
    if (auto* d = std::get_if<DictData>(data.get()))
        for (auto& [k, v] : d->entries)
        {
            release(c, k);
            release(c, v);
        }
    if (auto* d = std::get_if<DataData>(data.get()))
        if (d->guest) c.rt.heap.free(d->guest);
}

void ignored_nil(const char* what, Cpu& c)
{
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) std::fprintf(stderr, "[objc] %s from %s, ignored\n", what, c.rt.describe(c.lr()).c_str());
}

void fast_enumerate(Cpu& c, const std::vector<Id>& items)
{
    GuestAddr state = c.arg(2), buf = c.arg(3);
    uint64_t cap = c.arg(4), start = c.mem.read<uint64_t>(state);
    uint64_t n = start < items.size() ? std::min<uint64_t>(cap, items.size() - start) : 0;
    for (uint64_t i = 0; i < n; ++i)
        c.mem.write<uint64_t>(buf + i * 8, items[start + i]);
    c.mem.write<uint64_t>(state, start + n);
    c.mem.write<uint64_t>(state + 8, buf);
    c.mem.write<uint64_t>(state + 16, state + 24);
    c.ret(n);
}

GuestAddr data_guest_bytes(Cpu& c, Id self)
{
    auto& d = store().get<DataData>(self);
    if (d.guest_dirty || !d.guest)
    {
        if (d.guest) c.rt.heap.free(d.guest);
        d.guest = c.rt.heap.alloc(std::max<size_t>(d.bytes.size(), 1));
        if (!d.bytes.empty()) c.mem.write_bytes(d.guest, d.bytes.data(), d.bytes.size());
        d.guest_dirty = false;
    }
    return d.guest;
}

std::vector<uint8_t>& data_bytes(Cpu& c, Id self)
{
    auto& d = store().get<DataData>(self);
    if (d.guest && !d.guest_dirty && !d.bytes.empty()) c.mem.read_bytes(d.guest, d.bytes.data(), d.bytes.size());
    return d.bytes;
}

std::string number_description(const NumberData& n)
{
    if (n.type == 'c' || n.type == 'B') return std::to_string(n.i);
    if (n.is_float())
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", n.d);
        return buf;
    }
    return n.is_unsigned() ? std::to_string(n.u) : std::to_string(n.i);
}

void register_arrays(objc::ObjcRuntime& o)
{
    o.define("__NSArrayI", "NSArray");
    o.define("__NSArrayM", "NSMutableArray");
    o.define("__NSDictionaryI", "NSDictionary");
    o.define("__NSDictionaryM", "NSMutableDictionary");
    o.define("__NSSetI", "NSSet");
    o.define("__NSSetM", "NSMutableSet");

    o.class_method("NSArray", "alloc", [](Cpu& c) { c.ret(alloc_cluster(c, "__NSArrayI", "__NSArrayM", "NSMutableArray")); });
    o.class_method("NSArray", "allocWithZone:", [](Cpu& c) { c.ret(alloc_cluster(c, "__NSArrayI", "__NSArrayM", "NSMutableArray")); });
    o.class_method("NSArray", "array", [](Cpu& c) {
        Id a = objc(c).send(c, objc(c).send(c, c.arg(0), "alloc"), "init");
        c.ret(autorelease(c, a));
    });
    o.class_method("NSArray", "arrayWithObject:", [](Cpu& c) {
        Id a = objc(c).send(c, c.arg(0), "alloc");
        fill_array(c, a, {c.arg(2)});
        c.ret(autorelease(c, a));
    });
    o.class_method("NSArray", "arrayWithObjects:", [](Cpu& c) {
        Id a = objc(c).send(c, c.arg(0), "alloc");
        fill_array(c, a, variadic_objects(c, c.arg(2)));
        c.ret(autorelease(c, a));
    });
    o.class_method("NSArray", "arrayWithObjects:count:", [](Cpu& c) {
        Id a = objc(c).send(c, c.arg(0), "alloc");
        fill_array(c, a, read_ids(c, c.arg(2), c.arg(3)));
        c.ret(autorelease(c, a));
    });
    o.class_method("NSArray", "arrayWithArray:", [](Cpu& c) {
        Id a = objc(c).send(c, c.arg(0), "alloc");
        fill_array(c, a, array_items(c, c.arg(2)));
        c.ret(autorelease(c, a));
    });
    o.class_method("NSMutableArray", "arrayWithCapacity:", [](Cpu& c) {
        Id a = objc(c).send(c, c.arg(0), "alloc");
        store().get<ArrayData>(a);
        c.ret(autorelease(c, a));
    });

    o.method("NSArray", "init", [](Cpu& c) { store().get<ArrayData>(c.arg(0)); });
    o.method("NSMutableArray", "initWithCapacity:", [](Cpu& c) { store().get<ArrayData>(c.arg(0)); });
    o.method("NSArray", "initWithArray:", [](Cpu& c) { fill_array(c, c.arg(0), array_items(c, c.arg(2))); });
    o.method("NSArray", "initWithObjects:count:", [](Cpu& c) { fill_array(c, c.arg(0), read_ids(c, c.arg(2), c.arg(3))); });
    o.method("NSArray", "initWithObjects:", [](Cpu& c) { fill_array(c, c.arg(0), variadic_objects(c, c.arg(2))); });
    o.method("NSArray", "dealloc", [](Cpu& c) {
        release_contents(c, c.arg(0));
        objc(c).dispose(c.arg(0));
    });

    o.method("NSArray", "count", [](Cpu& c) { c.ret(array_items(c, c.arg(0)).size()); });
    o.method("NSArray", "objectAtIndex:", [](Cpu& c) {
        auto items = array_items(c, c.arg(0));
        if (c.arg(2) >= items.size()) return c.stop("-[NSArray objectAtIndex:] index " + std::to_string(c.arg(2)) + " beyond bounds");
        c.ret(items[c.arg(2)]);
    });
    o.method("NSArray", "objectAtIndexedSubscript:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "objectAtIndex:", {c.arg(2)})); });
    o.method("NSArray", "firstObject", [](Cpu& c) {
        auto items = array_items(c, c.arg(0));
        c.ret(items.empty() ? 0 : items.front());
    });
    o.method("NSArray", "lastObject", [](Cpu& c) {
        auto items = array_items(c, c.arg(0));
        c.ret(items.empty() ? 0 : items.back());
    });
    o.method("NSArray", "containsObject:", [](Cpu& c) {
        for (Id i : array_items(c, c.arg(0)))
            if (objects_equal(c, i, c.arg(2))) return c.ret(1);
        c.ret(0);
    });
    o.method("NSArray", "indexOfObject:", [](Cpu& c) {
        auto items = array_items(c, c.arg(0));
        for (size_t i = 0; i < items.size(); ++i)
            if (objects_equal(c, items[i], c.arg(2))) return c.ret(i);
        c.ret(kNotFound);
    });
    o.method("NSArray", "countByEnumeratingWithState:objects:count:", [](Cpu& c) { fast_enumerate(c, array_items(c, c.arg(0))); });
    o.method("NSArray", "copyWithZone:", [](Cpu& c) {
        if (class_is(c, c.arg(0), "__NSArrayI")) return c.ret(retain(c, c.arg(0)));
        Id a = new_of(c, "__NSArrayI");
        fill_array(c, a, array_items(c, c.arg(0)));
        c.ret(a);
    });
    o.method("NSArray", "mutableCopyWithZone:", [](Cpu& c) {
        Id a = new_of(c, "__NSArrayM");
        fill_array(c, a, array_items(c, c.arg(0)));
        c.ret(a);
    });
    o.method("NSArray", "arrayByAddingObject:", [](Cpu& c) {
        auto items = array_items(c, c.arg(0));
        items.push_back(c.arg(2));
        c.ret(make_array(c, items));
    });
    o.method("NSArray", "arrayByAddingObjectsFromArray:", [](Cpu& c) {
        auto items = array_items(c, c.arg(0));
        auto more = array_items(c, c.arg(2));
        items.insert(items.end(), more.begin(), more.end());
        c.ret(make_array(c, items));
    });
    o.method("NSArray", "componentsJoinedByString:", [](Cpu& c) {
        std::u16string out, sep = to_utf16(c, c.arg(2));
        bool first = true;
        for (Id i : array_items(c, c.arg(0)))
        {
            if (!first) out += sep;
            first = false;
            out += utf8_to_16(describe(c, i));
        }
        c.ret(string16_autoreleased(c, out));
    });
    o.method("NSArray", "enumerateObjectsUsingBlock:", [](Cpu& c) {
        GuestAddr stop = c.mem.alloc_system(8, 8);
        c.mem.write<uint64_t>(stop, 0);
        auto items = array_items(c, c.arg(0));
        for (size_t i = 0; i < items.size() && !c.stopped(); ++i)
        {
            call_block(c, c.arg(2), {items[i], i, stop});
            if (c.mem.read<uint8_t>(stop)) break;
        }
    });
    o.method("NSArray", "isEqualToArray:", [](Cpu& c) {
        auto a = array_items(c, c.arg(0)), b = array_items(c, c.arg(2));
        if (a.size() != b.size()) return c.ret(0);
        for (size_t i = 0; i < a.size(); ++i)
            if (!objects_equal(c, a[i], b[i])) return c.ret(0);
        c.ret(1);
    });
    o.method("NSArray", "description", [](Cpu& c) {
        std::string s = "(\n";
        for (Id i : array_items(c, c.arg(0)))
            s += "    " + describe(c, i) + ",\n";
        c.ret(string_autoreleased(c, s + ")"));
    });

    o.method("NSMutableArray", "addObject:", [](Cpu& c) {
        if (!c.arg(2)) return ignored_nil("-[NSMutableArray addObject:] with nil", c);
        store().get<ArrayData>(c.arg(0)).items.push_back(retain(c, c.arg(2)));
    });
    o.method("NSMutableArray", "addObjectsFromArray:", [](Cpu& c) {
        for (Id i : array_items(c, c.arg(2)))
            store().get<ArrayData>(c.arg(0)).items.push_back(retain(c, i));
    });
    o.method("NSMutableArray", "insertObject:atIndex:", [](Cpu& c) {
        auto& a = store().get<ArrayData>(c.arg(0));
        a.items.insert(a.items.begin() + std::min<size_t>(c.arg(3), a.items.size()), retain(c, c.arg(2)));
    });
    o.method("NSMutableArray", "removeObjectAtIndex:", [](Cpu& c) {
        auto& a = store().get<ArrayData>(c.arg(0));
        if (c.arg(2) >= a.items.size()) return;
        Id old = a.items[c.arg(2)];
        a.items.erase(a.items.begin() + c.arg(2));
        release(c, old);
    });
    o.method("NSMutableArray", "removeObject:", [](Cpu& c) {
        auto& a = store().get<ArrayData>(c.arg(0));
        std::vector<Id> removed;
        for (size_t i = 0; i < a.items.size();)
        {
            if (objects_equal(c, a.items[i], c.arg(2)))
            {
                removed.push_back(a.items[i]);
                a.items.erase(a.items.begin() + i);
            }
            else
            {
                ++i;
            }
        }
        for (Id r : removed)
            release(c, r);
    });
    o.method("NSMutableArray", "removeLastObject", [](Cpu& c) {
        auto& a = store().get<ArrayData>(c.arg(0));
        if (a.items.empty()) return;
        Id old = a.items.back();
        a.items.pop_back();
        release(c, old);
    });
    o.method("NSMutableArray", "removeAllObjects", [](Cpu& c) { fill_array(c, c.arg(0), {}); });
    o.method("NSMutableArray", "replaceObjectAtIndex:withObject:", [](Cpu& c) {
        auto& a = store().get<ArrayData>(c.arg(0));
        if (c.arg(2) >= a.items.size()) return;
        Id old = a.items[c.arg(2)];
        a.items[c.arg(2)] = retain(c, c.arg(3));
        release(c, old);
    });
    o.method("NSMutableArray", "setObject:atIndexedSubscript:", [](Cpu& c) {
        auto& a = store().get<ArrayData>(c.arg(0));
        if (c.arg(3) == a.items.size()) return (void)a.items.push_back(retain(c, c.arg(2)));
        if (c.arg(3) > a.items.size()) return;
        Id old = a.items[c.arg(3)];
        a.items[c.arg(3)] = retain(c, c.arg(2));
        release(c, old);
    });
}

void register_dictionaries(objc::ObjcRuntime& o)
{
    o.class_method("NSDictionary", "alloc",
                   [](Cpu& c) { c.ret(alloc_cluster(c, "__NSDictionaryI", "__NSDictionaryM", "NSMutableDictionary")); });
    o.class_method("NSDictionary",
                   "allocWithZone:", [](Cpu& c) { c.ret(alloc_cluster(c, "__NSDictionaryI", "__NSDictionaryM", "NSMutableDictionary")); });
    o.class_method("NSDictionary", "dictionary", [](Cpu& c) {
        Id d = objc(c).send(c, c.arg(0), "alloc");
        store().get<DictData>(d);
        c.ret(autorelease(c, d));
    });
    o.class_method("NSMutableDictionary", "dictionaryWithCapacity:", [](Cpu& c) {
        Id d = objc(c).send(c, c.arg(0), "alloc");
        store().get<DictData>(d);
        c.ret(autorelease(c, d));
    });
    o.class_method("NSDictionary", "dictionaryWithDictionary:", [](Cpu& c) {
        Id d = objc(c).send(c, c.arg(0), "alloc");
        for (auto& [k, v] : dict_entries(c, c.arg(2)))
            dict_set(c, d, k, v);
        c.ret(autorelease(c, d));
    });
    o.class_method("NSDictionary", "dictionaryWithObject:forKey:", [](Cpu& c) {
        Id d = objc(c).send(c, c.arg(0), "alloc");
        dict_set(c, d, c.arg(3), c.arg(2));
        c.ret(autorelease(c, d));
    });
    o.class_method("NSDictionary", "dictionaryWithObjects:forKeys:count:", [](Cpu& c) {
        Id d = objc(c).send(c, c.arg(0), "alloc");
        auto vals = read_ids(c, c.arg(2), c.arg(4)), keys = read_ids(c, c.arg(3), c.arg(4));
        store().get<DictData>(d);
        for (size_t i = 0; i < vals.size(); ++i)
            dict_set(c, d, keys[i], vals[i]);
        c.ret(autorelease(c, d));
    });
    o.class_method("NSDictionary", "dictionaryWithObjectsAndKeys:", [](Cpu& c) {
        Id d = objc(c).send(c, c.arg(0), "alloc");
        auto list = variadic_objects(c, c.arg(2));
        store().get<DictData>(d);
        for (size_t i = 0; i + 1 < list.size(); i += 2)
            dict_set(c, d, list[i + 1], list[i]);
        c.ret(autorelease(c, d));
    });

    o.method("NSDictionary", "init", [](Cpu& c) { store().get<DictData>(c.arg(0)); });
    o.method("NSMutableDictionary", "initWithCapacity:", [](Cpu& c) { store().get<DictData>(c.arg(0)); });
    o.method("NSDictionary", "initWithDictionary:", [](Cpu& c) {
        store().get<DictData>(c.arg(0));
        for (auto& [k, v] : dict_entries(c, c.arg(2)))
            dict_set(c, c.arg(0), k, v);
    });
    o.method("NSDictionary", "initWithObjects:forKeys:count:", [](Cpu& c) {
        auto vals = read_ids(c, c.arg(2), c.arg(4)), keys = read_ids(c, c.arg(3), c.arg(4));
        store().get<DictData>(c.arg(0));
        for (size_t i = 0; i < vals.size(); ++i)
            dict_set(c, c.arg(0), keys[i], vals[i]);
    });
    o.method("NSDictionary", "dealloc", [](Cpu& c) {
        release_contents(c, c.arg(0));
        objc(c).dispose(c.arg(0));
    });

    o.method("NSDictionary", "count", [](Cpu& c) { c.ret(dict_entries(c, c.arg(0)).size()); });
    o.method("NSDictionary", "objectForKey:", [](Cpu& c) { c.ret(dict_lookup(c, c.arg(0), c.arg(2))); });
    o.method("NSDictionary", "objectForKeyedSubscript:", [](Cpu& c) { c.ret(dict_lookup(c, c.arg(0), c.arg(2))); });
    o.method("NSDictionary", "valueForKey:", [](Cpu& c) { c.ret(dict_lookup(c, c.arg(0), c.arg(2))); });
    o.method("NSDictionary", "allKeys", [](Cpu& c) {
        std::vector<Id> keys;
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            keys.push_back(k);
        c.ret(make_array(c, keys));
    });
    o.method("NSDictionary", "allValues", [](Cpu& c) {
        std::vector<Id> vals;
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            vals.push_back(v);
        c.ret(make_array(c, vals));
    });
    o.method("NSDictionary", "countByEnumeratingWithState:objects:count:", [](Cpu& c) {
        std::vector<Id> keys;
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            keys.push_back(k);
        fast_enumerate(c, keys);
    });
    o.method("NSDictionary", "keyEnumerator", [](Cpu& c) {
        std::vector<Id> keys;
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            keys.push_back(k);
        c.ret(objc(c).send(c, make_array(c, keys), "objectEnumerator"));
    });
    o.method("NSDictionary", "enumerateKeysAndObjectsUsingBlock:", [](Cpu& c) {
        GuestAddr stop = c.mem.alloc_system(8, 8);
        c.mem.write<uint64_t>(stop, 0);
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
        {
            call_block(c, c.arg(2), {k, v, stop});
            if (c.stopped() || c.mem.read<uint8_t>(stop)) break;
        }
    });
    o.method("NSDictionary", "copyWithZone:", [](Cpu& c) {
        if (class_is(c, c.arg(0), "__NSDictionaryI")) return c.ret(retain(c, c.arg(0)));
        Id d = new_of(c, "__NSDictionaryI");
        store().get<DictData>(d);
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            dict_set(c, d, k, v);
        c.ret(d);
    });
    o.method("NSDictionary", "mutableCopyWithZone:", [](Cpu& c) {
        Id d = new_of(c, "__NSDictionaryM");
        store().get<DictData>(d);
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            dict_set(c, d, k, v);
        c.ret(d);
    });
    o.method("NSDictionary", "description", [](Cpu& c) {
        std::string s = "{\n";
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            s += "    " + describe(c, k) + " = " + describe(c, v) + ";\n";
        c.ret(string_autoreleased(c, s + "}"));
    });

    o.method("NSMutableDictionary", "setObject:forKey:", [](Cpu& c) {
        if (!c.arg(2)) return ignored_nil("-[NSMutableDictionary setObject:forKey:] with nil object", c);
        dict_set(c, c.arg(0), c.arg(3), c.arg(2));
    });
    o.method("NSMutableDictionary", "setObject:forKeyedSubscript:", [](Cpu& c) {
        if (!c.arg(2)) return dict_remove(c, c.arg(0), c.arg(3));
        dict_set(c, c.arg(0), c.arg(3), c.arg(2));
    });
    o.method("NSMutableDictionary", "setValue:forKey:", [](Cpu& c) {
        if (!c.arg(2)) return dict_remove(c, c.arg(0), c.arg(3));
        dict_set(c, c.arg(0), c.arg(3), c.arg(2));
    });
    o.method("NSMutableDictionary", "removeObjectForKey:", [](Cpu& c) { dict_remove(c, c.arg(0), c.arg(2)); });
    o.method("NSMutableDictionary", "removeAllObjects", [](Cpu& c) {
        for (auto& [k, v] : dict_entries(c, c.arg(0)))
            dict_remove(c, c.arg(0), k);
    });
    o.method("NSMutableDictionary", "addEntriesFromDictionary:", [](Cpu& c) {
        for (auto& [k, v] : dict_entries(c, c.arg(2)))
            dict_set(c, c.arg(0), k, v);
    });
}

void register_sets(objc::ObjcRuntime& o)
{
    auto add = [](Cpu& c, Id self, Id obj) {
        auto& s = store().get<SetData>(self);
        for (Id i : s.items)
            if (objects_equal(c, i, obj)) return;
        s.items.push_back(retain(c, obj));
    };
    static decltype(add) s_add = add;
    o.class_method("NSSet", "alloc", [](Cpu& c) { c.ret(alloc_cluster(c, "__NSSetI", "__NSSetM", "NSMutableSet")); });
    o.class_method("NSSet", "set", [](Cpu& c) {
        Id s = objc(c).send(c, c.arg(0), "alloc");
        store().get<SetData>(s);
        c.ret(autorelease(c, s));
    });
    o.class_method("NSSet", "setWithArray:", [](Cpu& c) {
        Id s = objc(c).send(c, c.arg(0), "alloc");
        store().get<SetData>(s);
        for (Id i : array_items(c, c.arg(2)))
            s_add(c, s, i);
        c.ret(autorelease(c, s));
    });
    o.class_method("NSSet", "setWithObject:", [](Cpu& c) {
        Id s = objc(c).send(c, c.arg(0), "alloc");
        s_add(c, s, c.arg(2));
        c.ret(autorelease(c, s));
    });
    o.class_method("NSSet", "setWithObjects:", [](Cpu& c) {
        Id s = objc(c).send(c, c.arg(0), "alloc");
        store().get<SetData>(s);
        for (Id i : variadic_objects(c, c.arg(2)))
            s_add(c, s, i);
        c.ret(autorelease(c, s));
    });
    o.method("NSSet", "init", [](Cpu& c) { store().get<SetData>(c.arg(0)); });
    o.method("NSSet", "initWithObjects:", [](Cpu& c) {
        store().get<SetData>(c.arg(0));
        for (Id i : variadic_objects(c, c.arg(2)))
            s_add(c, c.arg(0), i);
        c.ret(c.arg(0));
    });
    o.method("NSSet", "initWithArray:", [](Cpu& c) {
        store().get<SetData>(c.arg(0));
        for (Id i : array_items(c, c.arg(2)))
            s_add(c, c.arg(0), i);
    });
    o.method("NSSet", "dealloc", [](Cpu& c) {
        release_contents(c, c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    o.method("NSSet", "count", [](Cpu& c) { c.ret(store().get<SetData>(c.arg(0)).items.size()); });
    o.method("NSSet", "containsObject:", [](Cpu& c) {
        for (Id i : store().get<SetData>(c.arg(0)).items)
            if (objects_equal(c, i, c.arg(2))) return c.ret(1);
        c.ret(0);
    });
    o.method("NSSet", "member:", [](Cpu& c) {
        for (Id i : store().get<SetData>(c.arg(0)).items)
            if (objects_equal(c, i, c.arg(2))) return c.ret(i);
        c.ret(0);
    });
    o.method("NSSet", "allObjects", [](Cpu& c) { c.ret(make_array(c, store().get<SetData>(c.arg(0)).items)); });
    o.method("NSSet", "anyObject", [](Cpu& c) {
        auto& s = store().get<SetData>(c.arg(0));
        c.ret(s.items.empty() ? 0 : s.items.front());
    });
    o.method("NSSet",
             "countByEnumeratingWithState:objects:count:", [](Cpu& c) { fast_enumerate(c, store().get<SetData>(c.arg(0)).items); });
    o.method("NSSet", "copyWithZone:", [](Cpu& c) {
        Id s = new_of(c, "__NSSetI");
        store().get<SetData>(s);
        for (Id i : store().get<SetData>(c.arg(0)).items)
            s_add(c, s, i);
        c.ret(s);
    });
    o.method("NSSet", "mutableCopyWithZone:", [](Cpu& c) {
        Id s = new_of(c, "__NSSetM");
        store().get<SetData>(s);
        for (Id i : store().get<SetData>(c.arg(0)).items)
            s_add(c, s, i);
        c.ret(s);
    });
    o.method("NSMutableSet", "addObject:", [](Cpu& c) { s_add(c, c.arg(0), c.arg(2)); });
    o.method("NSMutableSet", "removeObject:", [](Cpu& c) {
        auto& s = store().get<SetData>(c.arg(0));
        for (size_t i = 0; i < s.items.size(); ++i)
        {
            if (objects_equal(c, s.items[i], c.arg(2)))
            {
                Id old = s.items[i];
                s.items.erase(s.items.begin() + i);
                release(c, old);
                return;
            }
        }
    });
    o.method("NSMutableSet", "removeAllObjects", [](Cpu& c) {
        auto& s = store().get<SetData>(c.arg(0));
        auto items = std::move(s.items);
        s.items.clear();
        for (Id i : items)
            release(c, i);
    });
}

void register_numbers(objc::ObjcRuntime& o)
{
    o.define("__NSCFNumber", "NSNumber");
    o.define("__NSCFBoolean", "NSNumber");
    o.define("NSNull", "NSObject");

    auto make = [](Cpu& c, char type, int64_t i, uint64_t u, double d) {
        NumberData n;
        n.type = type;
        n.i = i;
        n.u = u;
        n.d = d;
        return make_number(c, n);
    };
    static decltype(make) s_make = make;
    o.class_method("NSNumber", "numberWithInt:", [](Cpu& c) { c.ret(s_make(c, 'i', int32_t(c.arg(2)), 0, 0)); });
    o.class_method("NSNumber", "numberWithInteger:", [](Cpu& c) { c.ret(s_make(c, 'q', int64_t(c.arg(2)), 0, 0)); });
    o.class_method("NSNumber", "numberWithLong:", [](Cpu& c) { c.ret(s_make(c, 'q', int64_t(c.arg(2)), 0, 0)); });
    o.class_method("NSNumber", "numberWithLongLong:", [](Cpu& c) { c.ret(s_make(c, 'q', int64_t(c.arg(2)), 0, 0)); });
    o.class_method("NSNumber", "numberWithShort:", [](Cpu& c) { c.ret(s_make(c, 's', int16_t(c.arg(2)), 0, 0)); });
    o.class_method("NSNumber", "numberWithChar:", [](Cpu& c) { c.ret(s_make(c, 'c', int8_t(c.arg(2)), 0, 0)); });
    o.class_method("NSNumber", "numberWithUnsignedInt:", [](Cpu& c) { c.ret(s_make(c, 'I', 0, uint32_t(c.arg(2)), 0)); });
    o.class_method("NSNumber", "numberWithUnsignedInteger:", [](Cpu& c) { c.ret(s_make(c, 'Q', 0, c.arg(2), 0)); });
    o.class_method("NSNumber", "numberWithUnsignedLong:", [](Cpu& c) { c.ret(s_make(c, 'Q', 0, c.arg(2), 0)); });
    o.class_method("NSNumber", "numberWithUnsignedLongLong:", [](Cpu& c) { c.ret(s_make(c, 'Q', 0, c.arg(2), 0)); });
    o.class_method("NSNumber", "numberWithUnsignedChar:", [](Cpu& c) { c.ret(s_make(c, 'C', 0, uint8_t(c.arg(2)), 0)); });
    o.class_method("NSNumber", "numberWithBool:", [](Cpu& c) { c.ret(s_make(c, 'B', c.arg(2) & 1, 0, 0)); });
    o.class_method("NSNumber", "numberWithFloat:", [](Cpu& c) { c.ret(s_make(c, 'f', 0, 0, c.s(0))); });
    o.class_method("NSNumber", "numberWithDouble:", [](Cpu& c) { c.ret(s_make(c, 'd', 0, 0, c.d(0))); });
    o.method("NSNumber", "initWithInt:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'i', int32_t(c.arg(2)), 0, 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithInteger:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'q', int64_t(c.arg(2)), 0, 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithLong:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'q', int64_t(c.arg(2)), 0, 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithLongLong:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'q', int64_t(c.arg(2)), 0, 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithShort:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 's', int16_t(c.arg(2)), 0, 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithChar:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'c', int8_t(c.arg(2)), 0, 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithUnsignedInt:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'I', 0, uint32_t(c.arg(2)), 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithUnsignedInteger:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'Q', 0, c.arg(2), 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithUnsignedLong:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'Q', 0, c.arg(2), 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithUnsignedLongLong:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'Q', 0, c.arg(2), 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithUnsignedChar:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'C', 0, uint8_t(c.arg(2)), 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithBool:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'B', c.arg(2) & 1, 0, 0));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithFloat:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'f', 0, 0, c.s(0)));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });
    o.method("NSNumber", "initWithDouble:", [](Cpu& c) {
        Id n = objc(c).retain(s_make(c, 'd', 0, 0, c.d(0)));
        objc(c).dispose(c.arg(0));
        c.ret(n);
    });

    auto value = [](Cpu& c) {
        NumberData n;
        number_of(c, c.arg(0), n);
        return n;
    };
    static decltype(value) s_value = value;
    for (const char* sel : {"intValue", "shortValue", "charValue"})
        o.method("NSNumber", sel, [](Cpu& c) { c.ret(uint64_t(int64_t(int32_t(s_value(c).as_int())))); });
    for (const char* sel : {"integerValue", "longValue", "longLongValue"})
        o.method("NSNumber", sel, [](Cpu& c) { c.ret(uint64_t(s_value(c).as_int())); });
    for (const char* sel : {"unsignedIntValue", "unsignedShortValue", "unsignedCharValue"})
        o.method("NSNumber", sel, [](Cpu& c) { c.ret(uint32_t(s_value(c).as_int())); });
    for (const char* sel : {"unsignedIntegerValue", "unsignedLongValue", "unsignedLongLongValue"})
        o.method("NSNumber", sel, [](Cpu& c) {
            NumberData n = s_value(c);
            c.ret(n.is_unsigned() ? n.u : uint64_t(n.as_int()));
        });
    o.method("NSNumber", "boolValue", [](Cpu& c) { c.ret(s_value(c).as_double() != 0); });
    o.method("NSNumber", "floatValue", [](Cpu& c) { c.set_s(0, float(s_value(c).as_double())); });
    o.method("NSNumber", "doubleValue", [](Cpu& c) { c.set_d(0, s_value(c).as_double()); });
    o.method("NSNumber", "objCType", [](Cpu& c) {
        static std::unordered_map<char, GuestAddr> types;
        char t = s_value(c).type;
        auto& g = types[t];
        if (!g) g = c.mem.alloc_cstr_region(std::string(1, t));
        c.ret(g);
    });
    o.method("NSNumber", "stringValue", [](Cpu& c) { c.ret(string_autoreleased(c, number_description(s_value(c)))); });
    o.method("NSNumber", "description", [](Cpu& c) { c.ret(string_autoreleased(c, number_description(s_value(c)))); });
    o.method("NSNumber", "isEqualToNumber:", [](Cpu& c) { c.ret(objects_equal(c, c.arg(0), c.arg(2))); });
    o.method("NSNumber", "isEqual:", [](Cpu& c) { c.ret(objects_equal(c, c.arg(0), c.arg(2))); });
    o.method("NSNumber", "hash", [](Cpu& c) { c.ret(object_hash(c, c.arg(0))); });
    o.method("NSNumber", "compare:", [](Cpu& c) {
        NumberData a, b;
        number_of(c, c.arg(0), a);
        number_of(c, c.arg(2), b);
        double x = a.as_double(), y = b.as_double();
        c.ret(uint64_t(x < y ? -1 : x > y ? 1 : 0));
    });
    o.method("NSNumber", "copyWithZone:", [](Cpu& c) { c.ret(retain(c, c.arg(0))); });

    o.class_method("NSNull", "null", [](Cpu& c) { c.ret(null_object(c)); });
    o.method("NSNull", "description", [](Cpu& c) { c.ret(string_autoreleased(c, "<null>")); });
}

void register_data(objc::ObjcRuntime& o)
{
    o.define("__NSCFData", "NSMutableData");
    auto alloc = [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        c.ret(k && k->host ? new_of(c, "__NSCFData") : objc(c).alloc_instance(k));
    };
    o.class_method("NSData", "alloc", alloc);
    o.class_method("NSData", "allocWithZone:", alloc);
    o.class_method("NSData", "data", [](Cpu& c) { c.ret(make_data(c, {})); });
    o.class_method("NSMutableData", "data", [](Cpu& c) { c.ret(make_data(c, {}, true)); });
    o.class_method("NSMutableData", "dataWithCapacity:", [](Cpu& c) { c.ret(make_data(c, {}, true)); });
    o.class_method("NSMutableData", "dataWithLength:", [](Cpu& c) { c.ret(make_data(c, std::vector<uint8_t>(c.arg(2)), true)); });
    o.class_method("NSData", "dataWithBytes:length:", [](Cpu& c) {
        std::vector<uint8_t> b(c.arg(3));
        if (!b.empty()) c.mem.read_bytes(c.arg(2), b.data(), b.size());
        c.ret(make_data(c, std::move(b)));
    });
    o.class_method("NSData", "dataWithData:", [](Cpu& c) {
        auto* d = store().find<DataData>(c.arg(2));
        c.ret(make_data(c, d ? data_bytes(c, c.arg(2)) : std::vector<uint8_t>{}));
    });
    o.method("NSData", "init", [](Cpu& c) { store().get<DataData>(c.arg(0)); });
    o.method("NSData", "initWithBytes:length:", [](Cpu& c) {
        auto& d = store().get<DataData>(c.arg(0));
        d.bytes.resize(c.arg(3));
        if (c.arg(3)) c.mem.read_bytes(c.arg(2), d.bytes.data(), c.arg(3));
        d.guest_dirty = true;
    });
    o.method("NSData", "initWithBytesNoCopy:length:freeWhenDone:", [](Cpu& c) {
        auto& d = store().get<DataData>(c.arg(0));
        d.bytes.resize(c.arg(3));
        if (c.arg(3)) c.mem.read_bytes(c.arg(2), d.bytes.data(), c.arg(3));
        d.guest_dirty = true;
        if (c.arg(4) & 1) c.rt.heap.free(c.arg(2));
    });
    o.method("NSMutableData", "initWithLength:", [](Cpu& c) {
        auto& d = store().get<DataData>(c.arg(0));
        d.bytes.assign(c.arg(2), 0);
        d.guest_dirty = true;
    });
    o.method("NSMutableData", "initWithCapacity:", [](Cpu& c) { store().get<DataData>(c.arg(0)); });
    o.method("NSData", "dealloc", [](Cpu& c) {
        release_contents(c, c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    o.method("NSData", "length", [](Cpu& c) { c.ret(data_bytes(c, c.arg(0)).size()); });
    o.method("NSData", "bytes", [](Cpu& c) {
        data_bytes(c, c.arg(0));
        c.ret(data_guest_bytes(c, c.arg(0)));
    });
    o.method("NSMutableData", "mutableBytes", [](Cpu& c) {
        data_bytes(c, c.arg(0));
        c.ret(data_guest_bytes(c, c.arg(0)));
    });
    o.method("NSData", "getBytes:length:", [](Cpu& c) {
        auto& b = data_bytes(c, c.arg(0));
        uint64_t n = std::min<uint64_t>(b.size(), c.arg(3));
        if (n) c.mem.write_bytes(c.arg(2), b.data(), n);
    });
    o.method("NSData", "enumerateByteRangesUsingBlock:", [](Cpu& c) {
        static GuestAddr stop = c.rt.mem.alloc_system(8, 8);
        uint64_t n = data_bytes(c, c.arg(0)).size();
        if (!n) return;
        c.mem.write<uint8_t>(stop, 0);
        call_block(c, c.arg(2), {data_guest_bytes(c, c.arg(0)), 0, n, stop});
    });
    o.method("NSData", "copyWithZone:", [](Cpu& c) { c.ret(retain(c, make_data(c, data_bytes(c, c.arg(0))))); });
    o.method("NSData", "mutableCopyWithZone:", [](Cpu& c) { c.ret(retain(c, make_data(c, data_bytes(c, c.arg(0)), true))); });
    o.method("NSData", "isEqualToData:", [](Cpu& c) {
        c.ret(store().find<DataData>(c.arg(2)) && data_bytes(c, c.arg(0)) == data_bytes(c, c.arg(2)));
    });
    o.method("NSMutableData", "appendBytes:length:", [](Cpu& c) {
        auto& b = data_bytes(c, c.arg(0));
        size_t at = b.size();
        b.resize(at + c.arg(3));
        if (c.arg(3)) c.mem.read_bytes(c.arg(2), b.data() + at, c.arg(3));
        store().get<DataData>(c.arg(0)).guest_dirty = true;
    });
    o.method("NSMutableData", "appendData:", [](Cpu& c) {
        auto more = data_bytes(c, c.arg(2));
        auto& b = data_bytes(c, c.arg(0));
        b.insert(b.end(), more.begin(), more.end());
        store().get<DataData>(c.arg(0)).guest_dirty = true;
    });
    o.method("NSMutableData", "setLength:", [](Cpu& c) {
        data_bytes(c, c.arg(0)).resize(c.arg(2));
        store().get<DataData>(c.arg(0)).guest_dirty = true;
    });
}

}

ObjectStore& store()
{
    static ObjectStore s;
    return s;
}

bool number_of(Cpu& c, Id obj, NumberData& out)
{
    if (auto* n = store().find<NumberData>(obj))
    {
        out = *n;
        return true;
    }
    Class* k = objc(c).class_of(obj);
    if (!k) return false;
    if (k->name == "NSConstantIntegerNumber")
    {
        std::string enc = c.mem.read_cstr(c.mem.read<uint64_t>(obj + 8));
        out.type = enc.empty() ? 'q' : enc[0];
        out.i = c.mem.read<int64_t>(obj + 16);
        out.u = uint64_t(out.i);
        return true;
    }
    if (k->name == "NSConstantDoubleNumber")
    {
        out.type = 'd';
        out.d = c.mem.read<double>(obj + 8);
        return true;
    }
    if (k->name == "NSConstantFloatNumber")
    {
        out.type = 'f';
        out.d = c.mem.read<float>(obj + 8);
        return true;
    }
    return false;
}

bool objects_equal(Cpu& c, Id a, Id b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    bool sa = is_string(c, a), sb = is_string(c, b);
    if (sa || sb) return sa && sb && to_utf16(c, a) == to_utf16(c, b);
    NumberData na, nb;
    if (number_of(c, a, na))
    {
        if (!number_of(c, b, nb)) return false;
        if (na.is_float() || nb.is_float()) return na.as_double() == nb.as_double();
        return na.as_int() == nb.as_int();
    }
    return objc(c).send(c, a, "isEqual:", {b}) & 1;
}

uint64_t object_hash(Cpu& c, Id obj)
{
    if (is_string(c, obj))
    {
        uint64_t h = 1469598103934665603ull;
        for (char16_t ch : to_utf16(c, obj))
            h = (h ^ ch) * 1099511628211ull;
        return h;
    }
    NumberData n;
    if (number_of(c, obj, n)) return uint64_t(n.is_float() && n.d != std::floor(n.d) ? std::hash<double>{}(n.d) : n.as_int());
    return objc(c).send(c, obj, "hash");
}

std::vector<Id> array_items(Cpu& c, Id arr)
{
    if (!arr) return {};
    if (auto* a = store().find<ArrayData>(arr)) return a->items;
    if (class_is(c, arr, "NSConstantArray")) return read_ids(c, c.mem.read<uint64_t>(arr + 16), c.mem.read<uint64_t>(arr + 8));
    Class* k = objc(c).class_of(arr);
    if (k && !k->host)
    {
        uint64_t n = objc(c).send(c, arr, "count");
        std::vector<Id> out;
        for (uint64_t i = 0; i < n && !c.stopped(); ++i)
            out.push_back(objc(c).send(c, arr, "objectAtIndex:", {i}));
        return out;
    }
    return {};
}

std::vector<std::pair<Id, Id>> dict_entries(Cpu& c, Id dict)
{
    if (!dict) return {};
    if (auto* d = store().find<DictData>(dict)) return d->entries;
    if (class_is(c, dict, "NSConstantDictionary"))
    {
        uint64_t n = c.mem.read<uint64_t>(dict + 16);
        auto keys = read_ids(c, c.mem.read<uint64_t>(dict + 24), n), vals = read_ids(c, c.mem.read<uint64_t>(dict + 32), n);
        std::vector<std::pair<Id, Id>> out;
        for (uint64_t i = 0; i < n; ++i)
            out.emplace_back(keys[i], vals[i]);
        return out;
    }
    return {};
}

Id dict_lookup(Cpu& c, Id dict, Id key)
{
    if (!key) return 0;
    for (auto& [k, v] : dict_entries(c, dict))
        if (objects_equal(c, k, key)) return v;
    return 0;
}

Id make_array(Cpu& c, const std::vector<Id>& items, bool mutable_)
{
    Id a = new_of(c, mutable_ ? "__NSArrayM" : "__NSArrayI");
    fill_array(c, a, items);
    return autorelease(c, a);
}

Id make_dict(Cpu& c, const std::vector<std::pair<Id, Id>>& entries, bool mutable_)
{
    Id d = new_of(c, mutable_ ? "__NSDictionaryM" : "__NSDictionaryI");
    store().get<DictData>(d);
    for (auto& [k, v] : entries)
        dict_set(c, d, k, v);
    return autorelease(c, d);
}

Id make_number(Cpu& c, NumberData n)
{
    Id obj = new_of(c, n.type == 'B' ? "__NSCFBoolean" : "__NSCFNumber");
    store().get<NumberData>(obj) = n;
    return autorelease(c, obj);
}

Id make_data(Cpu& c, std::vector<uint8_t> bytes, bool)
{
    Id obj = new_of(c, "__NSCFData");
    auto& d = store().get<DataData>(obj);
    d.bytes = std::move(bytes);
    d.guest_dirty = true;
    return autorelease(c, obj);
}

Id null_object(Cpu& c)
{
    static Id null = [&] {
        Id n = c.rt.mem.alloc_system(16, 16);
        c.mem.write<uint64_t>(n, objc(c).host_class("NSNull")->addr);
        return n;
    }();
    return null;
}

void register_collections(objc::ObjcRuntime& o)
{
    register_arrays(o);
    register_dictionaries(o);
    register_sets(o);
    register_numbers(o);
    register_data(o);
}

}
