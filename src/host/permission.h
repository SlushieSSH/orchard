#pragma once

#include <filesystem>
#include <string>

namespace orchard
{
enum class NetworkPolicy
{
    Ask,
    Allow,
    Deny
};

void init_network_permission(const std::filesystem::path& store, const std::string& bundle_id, const std::string& app_name,
                             NetworkPolicy override_policy);
bool network_allowed();

}
