#pragma once

#include <filesystem>
#include <string>

namespace orchard
{
std::filesystem::path prepare_app(const std::filesystem::path& input);
bool register_ipa_association(bool remove);
void show_error(const std::string& message);
void set_dialogs_enabled(bool enabled);
std::filesystem::path local_cache_dir(const std::string& name);
}
