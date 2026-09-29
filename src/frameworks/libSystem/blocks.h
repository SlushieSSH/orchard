#pragma once

#include <initializer_list>

#include "core/memory.h"

namespace orchard
{
class Cpu;
class Hle;

GuestAddr block_copy(Cpu& cpu, GuestAddr block);
void block_release(Cpu& cpu, GuestAddr block);
uint64_t call_block(Cpu& cpu, GuestAddr block, std::initializer_list<uint64_t> args = {});

void register_blocks(Hle& h);

}
