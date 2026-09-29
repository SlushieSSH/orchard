#include "host/compat.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace orchard
{
namespace
{
struct Run
{
    std::string stage = "did not start";
    std::string stop;
    std::map<std::string, int> missing;
    bool timed_out = false;
};

bool run_child(const fs::path& exe, const fs::path& app, double seconds, bool lenient, const fs::path& log)
{
    std::wstring cmd = L"\"" + exe.wstring() + L"\" \"" + app.wstring() + L"\" --network deny --quit-after " +
                       std::to_wstring(int(seconds)) + (lenient ? L" --lenient" : L"");
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    bool started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(out);
    if (!started) return false;
    bool finished = WaitForSingleObject(pi.hProcess, DWORD((seconds + 90) * 1000)) == WAIT_OBJECT_0;
    if (!finished) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return finished;
}

Run parse(const fs::path& log)
{
    Run r;
    std::ifstream in(log);
    int frames = 0;
    for (std::string line; std::getline(in, line);)
    {
        auto has = [&](const char* s) { return line.find(s) != std::string::npos; };
        if (has("unpacking ") && r.stage == "did not start") r.stage = "unpacked";
        if (has("running ") && has("static initializers")) r.stage = "initializers";
        if (has("calling main at")) r.stage = "main";
        if (has("[UIKit] UIApplicationMain")) r.stage = "UIApplicationMain";
        if (has("[UIKit] launch finished")) r.stage = "run loop";
        if (has("[Metal] presented frame "))
        {
            frames = std::max(frames, std::atoi(line.c_str() + line.find("frame ") + 6));
            r.stage = frames >= 600 ? "running" : "first frame";
        }
        if (line.rfind("STOPPED on ", 0) == 0 && r.stop.empty()) r.stop = line.substr(11);
        if (line.rfind("load failed", 0) == 0 || line.rfind("Could not load", 0) == 0) r.stop = line;
        for (const char* tag : {"[hle] unimplemented ", "[objc] unimplemented "})
        {
            size_t u = line.find(tag);
            if (u == std::string::npos) continue;
            std::string name = line.substr(u + std::strlen(tag));
            name = name.substr(0, name.find(" called from"));
            ++r.missing[name];
        }
    }
    return r;
}

std::string cell(std::string s)
{
    for (char& ch : s)
        if (ch == '|') ch = '/';
    if (s.size() > 140) s = s.substr(0, 140) + "...";
    return s;
}
}

int run_compat(const fs::path& exe, const fs::path& dir, double seconds)
{
    std::vector<fs::path> apps;
    for (auto& e : fs::directory_iterator(dir))
    {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (ext == ".ipa" || (e.is_directory() && ext == ".app")) apps.push_back(e.path());
    }
    std::sort(apps.begin(), apps.end());
    if (apps.empty())
    {
        std::printf("no .ipa files in %s\n", dir.string().c_str());
        return 1;
    }
    fs::path logs = dir / "compat-logs";
    fs::create_directories(logs);
    std::ofstream md(dir / "compat.md");
    md << "| Game | Reached | Stopped on | Lenient run reached | Lenient run stopped on | Missing, most called first |\n";
    md << "|---|---|---|---|---|---|\n";
    for (auto& app : apps)
    {
        std::string name = app.stem().string();
        std::printf("[compat] %s: strict run...\n", name.c_str());
        fs::path strict_log = logs / (name + ".log");
        bool finished = run_child(exe, app, seconds, false, strict_log);
        Run strict = parse(strict_log);
        strict.timed_out = !finished;
        Run lenient;
        bool ran_lenient = !strict.stop.empty();
        if (ran_lenient)
        {
            std::printf("[compat] %s: stopped (%s), lenient run...\n", name.c_str(), strict.stop.c_str());
            fs::path lenient_log = logs / (name + ".lenient.log");
            run_child(exe, app, seconds, true, lenient_log);
            lenient = parse(lenient_log);
        }
        std::vector<std::pair<int, std::string>> missing;
        for (auto& [fn, n] : lenient.missing)
            missing.push_back({n, fn});
        std::sort(missing.rbegin(), missing.rend());
        std::string top;
        for (size_t i = 0; i < missing.size() && i < 6; ++i)
            top += (i ? ", " : "") + missing[i].second;
        std::string stop = strict.timed_out ? "hung (killed)" : strict.stop.empty() ? "-" : strict.stop;
        md << "| " << cell(name) << " | " << strict.stage << " | " << cell(stop) << " | " << (ran_lenient ? lenient.stage : "-") << " | "
           << cell(!ran_lenient || lenient.stop.empty() ? "-" : lenient.stop) << " | " << cell(top.empty() ? "-" : top) << " |\n";
        md.flush();
        std::printf("[compat] %s: %s\n", name.c_str(), strict.stage.c_str());
    }
    std::printf("[compat] wrote %s\n", (dir / "compat.md").string().c_str());
    return 0;
}

}
