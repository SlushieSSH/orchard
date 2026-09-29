#pragma once

#include <cstdint>
#include <string>

namespace orchard
{
void start_profiler(double from_seconds, double to_seconds);
void name_host_thread(const std::string& name);
uint64_t process_private_bytes();
void start_hitch_monitor(double threshold_seconds);
void note_frame_presented();
}
