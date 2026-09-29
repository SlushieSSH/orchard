#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "core/memory.h"

namespace orchard
{
class Cpu;
}

namespace orchard::foundation
{
using Id = GuestAddr;

struct ArrayData
{
    std::vector<Id> items;
};

struct DictData
{
    std::vector<std::pair<Id, Id>> entries;
};

struct SetData
{
    std::vector<Id> items;
};

struct NumberData
{
    char type = 'q';
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;
    bool is_float() const { return type == 'f' || type == 'd'; }
    bool is_unsigned() const { return type == 'Q' || type == 'L' || type == 'I' || type == 'S' || type == 'C'; }
    double as_double() const { return is_float() ? d : is_unsigned() ? double(u) : double(i); }
    int64_t as_int() const { return is_float() ? int64_t(d) : is_unsigned() ? int64_t(u) : i; }
};

struct DataData
{
    std::vector<uint8_t> bytes;
    GuestAddr guest = 0;
    bool guest_dirty = true;
};

struct ValueData
{
    std::string type;
    std::vector<uint8_t> bytes;
};

using ObjectData = std::variant<ArrayData, DictData, SetData, NumberData, DataData, ValueData>;

class ObjectStore
{
public:
    template <typename T> T* find(Id obj)
    {
        std::lock_guard g(lock_);
        auto it = items_.find(obj);
        return it == items_.end() ? nullptr : std::get_if<T>(it->second.get());
    }
    template <typename T> T& get(Id obj)
    {
        std::lock_guard g(lock_);
        auto& p = items_[obj];
        if (!p || !std::holds_alternative<T>(*p)) p = std::make_shared<ObjectData>(T{});
        return std::get<T>(*p);
    }
    std::shared_ptr<ObjectData> take(Id obj)
    {
        std::lock_guard g(lock_);
        auto it = items_.find(obj);
        if (it == items_.end()) return nullptr;
        auto p = std::move(it->second);
        items_.erase(it);
        return p;
    }
    std::recursive_mutex& lock() { return lock_; }

private:
    std::recursive_mutex lock_;
    std::unordered_map<Id, std::shared_ptr<ObjectData>> items_;
};

ObjectStore& store();

bool objects_equal(Cpu& c, Id a, Id b);
uint64_t object_hash(Cpu& c, Id obj);

std::vector<Id> array_items(Cpu& c, Id arr);
std::vector<std::pair<Id, Id>> dict_entries(Cpu& c, Id dict);
Id dict_lookup(Cpu& c, Id dict, Id key);

Id make_array(Cpu& c, const std::vector<Id>& items, bool mutable_ = false);
Id make_dict(Cpu& c, const std::vector<std::pair<Id, Id>>& entries, bool mutable_ = false);
Id make_number(Cpu& c, NumberData n);
Id make_data(Cpu& c, std::vector<uint8_t> bytes, bool mutable_ = false);
Id null_object(Cpu& c);

bool number_of(Cpu& c, Id obj, NumberData& out);

}
