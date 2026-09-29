#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "core/memory.h"

namespace orchard
{
class Cpu;
namespace objc
{
class ObjcRuntime;
}
}

namespace orchard::metal
{
using Id = GuestAddr;

struct Value
{
    uint64_t x = 0;
    double d[4] = {0, 0, 0, 0};
    bool set = false;
};

Value prop(Cpu& c, Id bag, const std::string& name);
uint64_t prop_x(Cpu& c, Id bag, const std::string& name);
double prop_d(Cpu& c, Id bag, const std::string& name);
Id prop_obj(Cpu& c, Id bag, const std::string& name);
Id indexed(Cpu& c, Id array, uint64_t index);
bool is_descriptor(Cpu& c, Id obj);
void set_prop(Id bag, const std::string& name, uint64_t x);
std::map<std::string, Value> props(Id bag);

void register_descriptors(objc::ObjcRuntime& o);
GuestAddr descriptor_accessor_stub(Cpu& c);

}
