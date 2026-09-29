#pragma once

#include <cstdint>

namespace orchard::metal::astc
{
void decode(const uint8_t* src, uint64_t bpr, int width, int height, int block_w, int block_h, uint8_t* rgba);

}
