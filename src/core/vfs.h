#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orchard
{
class Vfs
{
public:
    void mount(std::string guest_prefix, std::filesystem::path host_dir);

    std::optional<std::filesystem::path> to_host(const std::string& guest_path) const;
    std::optional<std::string> to_guest(const std::filesystem::path& host_path) const;

    static std::string normalize(const std::string& path, const std::string& cwd = "/");

    std::string cwd = "/";

    std::string bundle_path;
    std::string home;
    std::string bundle_id;

private:
    struct Mount
    {
        std::string guest;
        std::filesystem::path host;
    };
    std::vector<Mount> mounts_;
    mutable std::mutex lock_;
};

}
