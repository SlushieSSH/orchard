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
Id retain(Cpu& c, Id o)
{
    return objc(c).retain_entry(c, o);
}
void release(Cpu& c, Id o)
{
    objc(c).release_entry(c, o);
}

Id make(Cpu& c, const char* cls)
{
    Id t = objc(c).alloc_instance(objc(c).host_class(cls));
    if (std::string_view(cls) == "NSMapTable")
        store().get<DictData>(t);
    else
        store().get<SetData>(t);
    return objc(c).autorelease(c, t);
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

std::vector<Id> keys_of(Id table)
{
    std::vector<Id> keys;
    for (auto& [k, v] : store().get<DictData>(table).entries)
        keys.push_back(k);
    return keys;
}

void register_map_table(objc::ObjcRuntime& o)
{
    o.define("NSMapTable", "NSObject");
    for (const char* sel : {"mapTableWithKeyOptions:valueOptions:", "strongToStrongObjectsMapTable", "weakToStrongObjectsMapTable",
                            "strongToWeakObjectsMapTable", "weakToWeakObjectsMapTable", "mapTableWithStrongToStrongObjects",
                            "mapTableWithWeakToStrongObjects", "mapTableWithStrongToWeakObjects"})
        o.class_method("NSMapTable", sel, [](Cpu& c) { c.ret(make(c, "NSMapTable")); });
    o.method("NSMapTable", "initWithKeyOptions:valueOptions:capacity:", [](Cpu& c) { store().get<DictData>(c.arg(0)); });
    o.method("NSMapTable", "objectForKey:", [](Cpu& c) {
        for (auto& [k, v] : store().get<DictData>(c.arg(0)).entries)
            if (objects_equal(c, k, c.arg(2))) return c.ret(v);
        c.ret(0);
    });
    o.method("NSMapTable", "setObject:forKey:", [](Cpu& c) {
        auto& d = store().get<DictData>(c.arg(0));
        for (auto& e : d.entries)
        {
            if (objects_equal(c, e.first, c.arg(3)))
            {
                Id old = e.second;
                e.second = retain(c, c.arg(2));
                release(c, old);
                return;
            }
        }
        d.entries.emplace_back(retain(c, c.arg(3)), retain(c, c.arg(2)));
    });
    o.method("NSMapTable", "removeObjectForKey:", [](Cpu& c) {
        auto& d = store().get<DictData>(c.arg(0));
        for (size_t i = 0; i < d.entries.size(); ++i)
        {
            if (objects_equal(c, d.entries[i].first, c.arg(2)))
            {
                auto [k, v] = d.entries[i];
                d.entries.erase(d.entries.begin() + i);
                release(c, k);
                release(c, v);
                return;
            }
        }
    });
    o.method("NSMapTable", "removeAllObjects", [](Cpu& c) {
        auto entries = std::move(store().get<DictData>(c.arg(0)).entries);
        store().get<DictData>(c.arg(0)).entries.clear();
        for (auto& [k, v] : entries)
        {
            release(c, k);
            release(c, v);
        }
    });
    o.method("NSMapTable", "count", [](Cpu& c) { c.ret(store().get<DictData>(c.arg(0)).entries.size()); });
    o.method("NSMapTable", "keyEnumerator", [](Cpu& c) { c.ret(objc(c).send(c, make_array(c, keys_of(c.arg(0))), "objectEnumerator")); });
    o.method("NSMapTable", "objectEnumerator", [](Cpu& c) {
        std::vector<Id> vals;
        for (auto& [k, v] : store().get<DictData>(c.arg(0)).entries)
            vals.push_back(v);
        c.ret(objc(c).send(c, make_array(c, vals), "objectEnumerator"));
    });
    o.method("NSMapTable", "dictionaryRepresentation", [](Cpu& c) { c.ret(make_dict(c, store().get<DictData>(c.arg(0)).entries)); });
    o.method("NSMapTable", "countByEnumeratingWithState:objects:count:", [](Cpu& c) { fast_enumerate(c, keys_of(c.arg(0))); });
}

void register_hash_table(objc::ObjcRuntime& o)
{
    o.define("NSHashTable", "NSObject");
    for (const char* sel : {"hashTableWithOptions:", "weakObjectsHashTable", "hashTableWithWeakObjects"})
        o.class_method("NSHashTable", sel, [](Cpu& c) { c.ret(make(c, "NSHashTable")); });
    o.method("NSHashTable", "initWithOptions:capacity:", [](Cpu& c) { store().get<SetData>(c.arg(0)); });
    o.method("NSHashTable", "addObject:", [](Cpu& c) {
        auto& s = store().get<SetData>(c.arg(0));
        for (Id i : s.items)
            if (objects_equal(c, i, c.arg(2))) return;
        s.items.push_back(retain(c, c.arg(2)));
    });
    o.method("NSHashTable", "removeObject:", [](Cpu& c) {
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
    o.method("NSHashTable", "removeAllObjects", [](Cpu& c) {
        auto items = std::move(store().get<SetData>(c.arg(0)).items);
        store().get<SetData>(c.arg(0)).items.clear();
        for (Id i : items)
            release(c, i);
    });
    o.method("NSHashTable", "containsObject:", [](Cpu& c) {
        for (Id i : store().get<SetData>(c.arg(0)).items)
            if (objects_equal(c, i, c.arg(2))) return c.ret(1);
        c.ret(0);
    });
    o.method("NSHashTable", "count", [](Cpu& c) { c.ret(store().get<SetData>(c.arg(0)).items.size()); });
    o.method("NSHashTable", "allObjects", [](Cpu& c) { c.ret(make_array(c, store().get<SetData>(c.arg(0)).items)); });
    o.method("NSHashTable", "anyObject", [](Cpu& c) {
        auto& s = store().get<SetData>(c.arg(0));
        c.ret(s.items.empty() ? 0 : s.items.front());
    });
    o.method("NSHashTable",
             "countByEnumeratingWithState:objects:count:", [](Cpu& c) { fast_enumerate(c, store().get<SetData>(c.arg(0)).items); });
}

}

void register_map_tables(objc::ObjcRuntime& o)
{
    register_map_table(o);
    register_hash_table(o);
}

}
