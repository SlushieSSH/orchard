#include "core/vfs.h"

#include <algorithm>

namespace orchard
{
void Vfs::mount(std::string guest_prefix, std::filesystem::path host_dir)
{
    std::lock_guard g(lock_);
    guest_prefix = normalize(guest_prefix);
    mounts_.push_back({guest_prefix, std::move(host_dir)});
    std::sort(mounts_.begin(), mounts_.end(), [](auto& a, auto& b) { return a.guest.size() > b.guest.size(); });
}

std::string Vfs::normalize(const std::string& path, const std::string& base)
{
    std::string full = path.empty() || path[0] != '/' ? base + "/" + path : path;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= full.size())
    {
        size_t j = full.find('/', i);
        if (j == std::string::npos) j = full.size();
        std::string part = full.substr(i, j - i);
        if (part == "..")
        {
            if (!parts.empty()) parts.pop_back();
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out;
    for (auto& p : parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

std::optional<std::filesystem::path> Vfs::to_host(const std::string& guest_path) const
{
    std::string p = normalize(guest_path, cwd);
    if (p.starts_with("/var/")) p = "/private" + p;
    std::lock_guard g(lock_);
    for (auto& m : mounts_)
    {
        if (p == m.guest) return m.host;
        if (p.starts_with(m.guest + "/"))
        {
            std::filesystem::path host = m.host;
            std::string rest = p.substr(m.guest.size() + 1);
            host /= std::filesystem::path(std::u8string(rest.begin(), rest.end()));
            return host;
        }
    }
    return std::nullopt;
}

std::optional<std::string> Vfs::to_guest(const std::filesystem::path& host_path) const
{
    std::lock_guard g(lock_);
    auto hp = std::filesystem::weakly_canonical(host_path);
    for (auto& m : mounts_)
    {
        auto base = std::filesystem::weakly_canonical(m.host);
        auto rel = hp.lexically_relative(base);
        if (!rel.empty() && rel.native()[0] != L'.')
        {
            auto r = rel.generic_u8string();
            return m.guest + "/" + std::string(r.begin(), r.end());
        }
        if (hp == base) return m.guest;
    }
    return std::nullopt;
}

}
