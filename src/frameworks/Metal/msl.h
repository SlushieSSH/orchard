#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orchard::metal::msl
{
constexpr int kBufferRegisterBase = 64;
constexpr int kConstSamplerTop = 15;

struct Field
{
    std::string type;
    std::string name;
    int array = 0;
    std::string semantic;
    std::string qualifier;
};

struct ConstSampler
{
    int slot = 0;
    bool linear_min = false, linear_mag = false, linear_mip = false, has_mip = false;
    int address = 0;
    int compare = 0;
};

struct Result
{
    bool ok = false;
    std::string error;
    std::string hlsl;
    bool vertex = true;
    std::map<int, uint32_t> buffers;
    std::vector<int> textures;
    std::vector<int> samplers;
    std::vector<int> compare_samplers;
    std::vector<ConstSampler> const_samplers;
    std::vector<Field> outputs;
    std::vector<int> attributes;
};

struct Options
{
    std::map<int, uint64_t> constants;
    std::map<std::string, uint64_t> named_constants;
    const std::vector<Field>* vertex_outputs = nullptr;
};

Result translate(const std::string& source, const std::string& entry, const Options& options);

}
