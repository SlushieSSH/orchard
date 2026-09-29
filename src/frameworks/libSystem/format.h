#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/memory.h"

namespace orchard
{
class Cpu;

struct VarArgs
{
    Memory& mem;
    GuestAddr next;

    uint64_t u64()
    {
        uint64_t v = mem.read<uint64_t>(next);
        next += 8;
        return v;
    }
    double f64()
    {
        double v = mem.read<double>(next);
        next += 8;
        return v;
    }
};

std::string guest_format(Cpu& cpu, const std::string& fmt, VarArgs args, const std::function<std::string(uint64_t)>& object = nullptr);

}
