#include "host/memstats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/runtime.h"
#include "host/profiler.h"

namespace orchard
{
void start_memstats(Runtime& rt)
{
    std::thread([&rt] {
        name_host_thread("memstats");
        for (int tick = 1;; ++tick)
        {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            auto guest = rt.mem.committed_by_name();
            auto hs = rt.heap.stats();
            uint64_t guest_total = 0;
            for (auto& [n, b] : guest)
                guest_total += b;
            uint64_t priv = process_private_bytes();
            std::printf("[mem] t=%ds private=%lluMB guest=%lluMB host-side=%lluMB | heap live small=%lluMB large=%lluMB (%llu allocs), "
                        "free small=%lluMB large=%lluMB\n",
                        tick * 10, (unsigned long long)(priv >> 20), (unsigned long long)(guest_total >> 20),
                        (unsigned long long)((priv - std::min(priv, guest_total)) >> 20), (unsigned long long)(hs.live_small >> 20),
                        (unsigned long long)(hs.live_large >> 20), (unsigned long long)hs.allocations,
                        (unsigned long long)(hs.free_small >> 20), (unsigned long long)(hs.free_large >> 20));
            for (auto& [n, b] : guest)
                if (b >= (16ull << 20)) std::printf("[mem]   %-26s %6lluMB\n", n.c_str(), (unsigned long long)(b >> 20));
            std::fflush(stdout);
        }
    }).detach();
}

}
