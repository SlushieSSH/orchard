#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <fstream>

#include "core/plist.h"
#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "frameworks/UIKit/uikit.h"
#include "host/compat.h"
#include "host/launcher.h"
#include "host/memstats.h"
#include "host/permission.h"
#include "host/profiler.h"
#include "loader/cache_images.h"
#include "loader/dyld_cache.h"
#include "loader/linker.h"

namespace fs = std::filesystem;
using namespace orchard;

namespace
{
constexpr const char* kBundleRoot = "/private/var/containers/Bundle/Application/5E1C0A3A-0C4D-4F7B-9A57-0A0C0DE0A11E";
constexpr const char* kDataRoot = "/private/var/mobile/Containers/Data/Application/7D3F6B21-4A1E-4C55-8E0B-1A2B3C4D5E6F";

struct ProcessArgs
{
    GuestAddr argc = 1;
    GuestAddr argv = 0;
    GuestAddr envp = 0;
    GuestAddr apple = 0;
};

GuestAddr string_array(Runtime& rt, const std::vector<std::string>& strings)
{
    GuestAddr arr = rt.mem.alloc_system((strings.size() + 1) * 8, 8);
    for (size_t i = 0; i < strings.size(); ++i)
        rt.mem.write<uint64_t>(arr + i * 8, rt.mem.alloc_cstr_region(strings[i]));
    rt.mem.write<uint64_t>(arr + strings.size() * 8, 0);
    return arr;
}

void setup_filesystem(Runtime& rt, const fs::path& app, const fs::path& userdata_root, NetworkPolicy network)
{
    std::ifstream f(app / "Info.plist", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    auto info = parse_plist(bytes);
    rt.vfs.bundle_id = info ? info->string_or("CFBundleIdentifier", app.stem().string()) : app.stem().string();
    rt.vfs.bundle_path = std::string(kBundleRoot) + "/" + app.filename().string();
    std::string name = app.stem().string();
    if (info) name = info->string_or("CFBundleDisplayName", info->string_or("CFBundleName", name));
    fs::create_directories(userdata_root);
    init_network_permission(userdata_root / "network.txt", rt.vfs.bundle_id, name, network);
    rt.vfs.home = kDataRoot;
    rt.vfs.mount(kBundleRoot, app.parent_path());

    fs::path data = userdata_root / rt.vfs.bundle_id;
    for (const char* sub : {"Documents", "Library/Caches", "Library/Preferences", "Library/Application Support", "tmp"})
        fs::create_directories(data / sub);
    rt.vfs.mount(kDataRoot, data);
    rt.vfs.cwd = rt.vfs.bundle_path;
}

ProcessArgs setup_process(Runtime& rt, const fs::path& app)
{
    std::string exe = std::string(kBundleRoot) + "/" + app.filename().string() + "/" + app.stem().string();
    std::string home = kDataRoot;
    ProcessArgs a;
    a.argv = string_array(rt, {exe});
    std::vector<std::string> env = {"HOME=" + home,
                                    "CFFIXED_USER_HOME=" + home,
                                    "TMPDIR=" + home + "/tmp/",
                                    "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
                                    "SHELL=/bin/sh",
                                    "USER=mobile",
                                    "LOGNAME=mobile"};
    for (auto& kv : env)
        rt.env[kv.substr(0, kv.find('='))] = kv.substr(kv.find('=') + 1);
    a.envp = string_array(rt, env);
    a.apple = string_array(rt, {"executable_path=" + exe});
    return a;
}

void report_failure(Runtime& rt)
{
    const auto& f = rt.failure();
    std::printf("\nSTOPPED on %s: %s\n", f.thread.c_str(), f.reason.c_str());
    for (size_t i = 0; i < f.backtrace.size(); ++i)
        std::printf("  %s %s\n", i == 0 ? "pc" : i == 1 ? "lr" : "  ", rt.describe(f.backtrace[i]).c_str());
    if (!f.hle_stack.empty())
    {
        std::printf("  inside HLE calls:");
        for (auto& n : f.hle_stack)
            std::printf(" %s", n.c_str());
        std::printf("\n");
    }
    show_error("The game stopped because Orchard hit something it does not support yet.\n\n" + f.reason);
}

}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: orchard <game.ipa | Name.app> [options]\n"
                             "       orchard --register    add Orchard to the Open with list for .ipa files\n"
                             "       orchard --unregister  remove it again\n");
        return 2;
    }
    std::string first = argv[1];
    if (first == "--register" || first == "--unregister")
    {
        bool ok = register_ipa_association(first == "--unregister");
        std::printf("%s\n", ok ? (first == "--register" ? "Orchard is now in the Open with list for .ipa files."
                                                        : "Orchard was removed from the Open with list.")
                               : "could not update the file association");
        return ok ? 0 : 1;
    }
    fs::path exe_dir = fs::absolute(argv[0]).parent_path();
    if (first == "--compat" && argc >= 3)
    {
        double seconds = argc >= 4 ? std::atof(argv[3]) : 60;
        return run_compat(fs::absolute(argv[0]), fs::absolute(argv[2]), seconds);
    }
    auto cache_in = [](const fs::path& dir) {
        for (fs::path candidate : {dir / "cache" / "dyld_shared_cache_arm64", dir / "external/ios-16.7.16/cache/dyld_shared_cache_arm64"})
            if (fs::exists(candidate)) return candidate;
        return fs::path();
    };
    fs::path root = cache_in(exe_dir).empty() && !cache_in(exe_dir.parent_path()).empty() ? exe_dir.parent_path() : exe_dir;
    fs::path app;
    try
    {
        app = prepare_app(fs::absolute(argv[1]));
    }
    catch (const std::exception& e)
    {
        show_error(e.what());
        return 1;
    }
    Runtime rt;
    fs::path cache_file = cache_in(root);
    NetworkPolicy network = NetworkPolicy::Ask;
    bool memstats = false;
    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--lenient")
            rt.lenient = true;
        else if (a == "--run-all-frameworks")
            rt.run_all_frameworks = true;
        else if (a == "--network" && i + 1 < argc)
        {
            std::string v = argv[++i];
            network = v == "allow" ? NetworkPolicy::Allow : v == "deny" ? NetworkPolicy::Deny : NetworkPolicy::Ask;
        }
        else if (a == "--profile" && i + 1 < argc)
        {
            double from = 0, to = 0;
            if (std::sscanf(argv[++i], "%lf:%lf", &from, &to) == 2) start_profiler(from, to);
        }
        else if (a == "--hitches")
            start_hitch_monitor(0.3);
        else if (a == "--memstats")
            memstats = true;
        else if (a == "--no-cache")
            cache_file.clear();
        else if (a == "--cache" && i + 1 < argc)
            cache_file = argv[++i];
        else if (a == "--quit-after" && i + 1 < argc)
            uikit::app().quit_after = std::stod(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc)
            uikit::app().screenshot = argv[++i];
        else if ((a == "--tap" || a == "--swipe") && i + 1 < argc)
        {
            float t = 0, v[4] = {};
            int n = std::sscanf(argv[++i], "%f:%f,%f,%f,%f", &t, &v[0], &v[1], &v[2], &v[3]);
            if (n == 3 || n == 5) uikit::app().script.push_back({t, std::vector<float>(v, v + n - 1)});
        }
    }
    std::stable_sort(uikit::app().script.begin(), uikit::app().script.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    if (uikit::app().quit_after > 0) set_dialogs_enabled(false);
    if (cache_file.empty())
    {
        show_error("Orchard needs the iOS system cache (dyld_shared_cache_arm64) and could not find it.\n\n"
                   "Put the decompressed file here:\n" +
                   (root / "cache" / "dyld_shared_cache_arm64").string());
        return 1;
    }
    else
    {
        try
        {
            rt.cache = std::make_unique<DyldCache>(rt.mem, cache_file);
            rt.cache_images = std::make_unique<CacheImages>(rt, *rt.cache);
            std::printf("dyld_shared_cache: %zu images\n", rt.cache->images().size());
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "no dyld_shared_cache (%s); Swift and C++ runtimes unavailable\n", e.what());
        }
    }

    setup_filesystem(rt, fs::absolute(app), root / "userdata", network);

    if (memstats) start_memstats(rt);
    Linker linker(rt);
    auto t0 = std::chrono::steady_clock::now();
    try
    {
        linker.load_bundle(app);
    }
    catch (const std::exception& e)
    {
        show_error(std::string("Could not load ") + app.filename().string() + ": " + e.what());
        return 1;
    }
    double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    uint64_t mapped = 0;
    size_t inits = 0;
    for (auto& img : linker.images())
    {
        mapped += img->end - img->base;
        inits += img->initializers.size();
    }
    const auto& ls = linker.stats();
    std::printf("loaded %zu images (%.1f MiB) in %.0f ms: %zu rebases, %zu binds (%zu to Apple libraries), %zu unresolved\n",
                linker.images().size(), mapped / 1048576.0, load_ms, ls.rebases, ls.binds, ls.system_binds, ls.unresolved);
    for (auto& img : linker.images())
        std::printf("  0x%09llx  %-30s %3zu initializers%s\n", (unsigned long long)img->base, img->name.c_str(), img->initializers.size(),
                    img->inert ? "  (inert)" : "");

    name_host_thread("main");
    Cpu cpu(rt, 0, 256);
    rt.threads->attach(cpu, 8ull << 20, "main");
    cpu.is_main = true;
    rt.main_cpu = &cpu;
    ProcessArgs pa = setup_process(rt, app);
    if (rt.cache_images)
    {
        auto set = [&](const char* name, uint64_t value, size_t size) {
            if (auto f = rt.cache_images->lookup("/usr/lib/libSystem.B.dylib", name)) rt.mem.write_bytes(f->addr, &value, size);
        };
        set("_environ", pa.envp, 8);
        set("_NXArgv", pa.argv, 8);
        set("_NXArgc", pa.argc, 4);
        set("___progname", rt.mem.alloc_cstr_region(app.stem().string()), 8);
    }

    std::printf("\nrunning %zu static initializers...\n", inits);
    t0 = std::chrono::steady_clock::now();
    linker.run_initializers(cpu, pa.argc, pa.argv, pa.envp, pa.apple);
    if (!cpu.stopped())
    {
        std::printf("initializers done; calling main at %s\n", rt.describe(linker.main_entry()).c_str());
        cpu.call(linker.main_entry(), {pa.argc, pa.argv, pa.envp, pa.apple});
        if (!cpu.stopped()) std::printf("main returned %llu\n", (unsigned long long)cpu.x(0));
    }
    double run_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if (rt.halted()) report_failure(rt);
    auto hs = rt.hle.stats();
    std::printf("\n%.0f ms of guest execution, %llu calls into HLE; %zu imported functions stubbed, %zu implemented\n", run_ms,
                (unsigned long long)cpu.hle_calls(), hs.stubs, hs.implemented);
    if (!hs.unimplemented_called.empty())
    {
        std::printf("unimplemented functions the app called (%zu):\n", hs.unimplemented_called.size());
        for (auto& n : hs.unimplemented_called)
            std::printf("  %s\n", n.c_str());
    }
    return 0;
}
