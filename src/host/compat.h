#pragma once

#include <filesystem>

namespace orchard
{
int run_compat(const std::filesystem::path& exe, const std::filesystem::path& dir, double seconds);
}
