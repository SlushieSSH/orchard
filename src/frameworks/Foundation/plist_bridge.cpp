#include "frameworks/Foundation/plist_bridge.h"

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
Id plist_to_object(Cpu& c, const Plist& p, bool mutable_containers)
{
    switch (p.kind)
    {
    case Plist::Kind::Bool: {
        NumberData n;
        n.type = 'B';
        n.i = p.b;
        return make_number(c, n);
    }
    case Plist::Kind::Int: {
        NumberData n;
        n.type = 'q';
        n.i = p.i;
        return make_number(c, n);
    }
    case Plist::Kind::Real: {
        NumberData n;
        n.type = 'd';
        n.d = p.r;
        return make_number(c, n);
    }
    case Plist::Kind::String: return string_autoreleased(c, p.s);
    case Plist::Kind::Data: return make_data(c, p.data, mutable_containers);
    case Plist::Kind::Date: return date_with_reference_seconds(c, p.r);
    case Plist::Kind::Array: {
        std::vector<Id> items;
        for (auto& v : p.array)
            items.push_back(plist_to_object(c, v, mutable_containers));
        return make_array(c, items, mutable_containers);
    }
    case Plist::Kind::Dict: {
        std::vector<std::pair<Id, Id>> entries;
        for (auto& [k, v] : p.dict)
            entries.emplace_back(string_autoreleased(c, k), plist_to_object(c, v, mutable_containers));
        return make_dict(c, entries, mutable_containers);
    }
    default: return 0;
    }
}

std::optional<Plist> object_to_plist(Cpu& c, Id obj)
{
    Plist p;
    if (!obj) return std::nullopt;
    if (is_string(c, obj))
    {
        p.kind = Plist::Kind::String;
        p.s = to_utf8(c, obj);
        return p;
    }
    NumberData n;
    if (number_of(c, obj, n))
    {
        if (n.type == 'B')
        {
            p.kind = Plist::Kind::Bool;
            p.b = n.i != 0;
        }
        else if (n.is_float())
        {
            p.kind = Plist::Kind::Real;
            p.r = n.d;
        }
        else
        {
            p.kind = Plist::Kind::Int;
            p.i = n.as_int();
        }
        return p;
    }
    if (auto* d = store().find<DataData>(obj))
    {
        p.kind = Plist::Kind::Data;
        p.data = d->bytes;
        if (d->guest && !d->guest_dirty && !p.data.empty()) c.mem.read_bytes(d->guest, p.data.data(), p.data.size());
        return p;
    }
    double secs;
    if (date_seconds(c, obj, secs))
    {
        p.kind = Plist::Kind::Date;
        p.r = secs;
        return p;
    }
    objc::Class* k = objc::objc(c).class_of(obj);
    if (objc::objc(c).is_subclass(k, objc::objc(c).class_named("NSDictionary")))
    {
        p.kind = Plist::Kind::Dict;
        for (auto& [key, v] : dict_entries(c, obj))
            if (auto pv = object_to_plist(c, v)) p.dict.emplace_back(to_utf8(c, key), std::move(*pv));
        return p;
    }
    if (objc::objc(c).is_subclass(k, objc::objc(c).class_named("NSArray")))
    {
        p.kind = Plist::Kind::Array;
        for (Id v : array_items(c, obj))
            if (auto pv = object_to_plist(c, v)) p.array.push_back(std::move(*pv));
        return p;
    }
    return std::nullopt;
}

}
