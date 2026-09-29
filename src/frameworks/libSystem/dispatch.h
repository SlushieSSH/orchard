#pragma once

#include <cstddef>

#include "core/memory.h"

namespace orchard
{
class Cpu;
class Hle;
namespace objc
{
class ObjcRuntime;
}

size_t drain_main_queue(Cpu& main);
bool main_queue_has_work();

GuestAddr main_queue_object(Cpu& c);
GuestAddr global_queue_object();
void dispatch_block_async(Cpu& c, GuestAddr queue, GuestAddr block);
void dispatch_function_async(GuestAddr queue, GuestAddr fn, GuestAddr ctx);
GuestAddr create_serial_queue(Cpu& c, const char* label);

void register_dispatch(Hle& h);
void register_dispatch_classes(objc::ObjcRuntime& o);

}
