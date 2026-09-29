#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace orchard::audio
{
struct Pcm
{
    double rate = 44100;
    uint32_t channels = 2;
    std::vector<float> samples;
    size_t frames() const { return channels ? samples.size() / channels : 0; }
    double seconds() const { return rate > 0 ? frames() / rate : 0; }
};

bool decode_audio(const std::vector<uint8_t>& bytes, Pcm& out, std::string* error = nullptr);

struct LinearFormat
{
    double rate = 44100;
    uint32_t channels = 2;
    uint32_t bits = 16;
    bool is_float = false;
    bool big_endian = false;
    bool non_interleaved = false;
};

void append_linear(const uint8_t* data, size_t bytes, const LinearFormat& f, std::vector<float>& out);

}
