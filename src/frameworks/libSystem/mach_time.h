#pragma once

#include <chrono>
#include <cstdint>

namespace orchard
{
constexpr uint64_t kMachTicksPerSecond = 24'000'000;

inline uint64_t mach_now()
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return uint64_t(ns) / 125 * 3;
}

inline std::chrono::steady_clock::time_point mach_to_steady(uint64_t ticks)
{
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ticks / 3 * 125));
}

}
