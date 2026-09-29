#include "host/profiler.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace orchard
{
namespace
{
struct Sample
{
    uint32_t group;
    uint64_t rip;
    uint64_t caller;
};

HMODULE module_of(uint64_t addr)
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &m);
    return m;
}

std::string symbol(HANDLE process, uint64_t addr)
{
    HMODULE m = module_of(addr);
    if (!m) return "[jit code]";
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring w(path);
    std::string mod(w.begin() + w.find_last_of(L'\\') + 1, w.end());
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(process, addr, &disp, sym)) return mod;
    std::string name = sym->Name;
    if (name.size() > 110) name = name.substr(0, 110) + "...";
    return mod == "orchard.exe" ? name : mod + "!" + name;
}

uint64_t first_orchard_frame(HANDLE process, HANDLE thread, CONTEXT ctx, HMODULE self)
{
    STACKFRAME64 frame = {};
    frame.AddrPC = {ctx.Rip, 0, AddrModeFlat};
    frame.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
    frame.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
    for (int i = 0; i < 16; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr))
            break;
        if (i > 0 && module_of(frame.AddrPC.Offset) == self) return frame.AddrPC.Offset;
    }
    return 0;
}

std::vector<uint64_t> orchard_frames(HANDLE process, HANDLE thread, CONTEXT ctx, HMODULE self, size_t want)
{
    std::vector<uint64_t> out;
    STACKFRAME64 frame = {};
    frame.AddrPC = {ctx.Rip, 0, AddrModeFlat};
    frame.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
    frame.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
    for (int i = 0; i < 24 && out.size() < want; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr))
            break;
        if (i > 0 && module_of(frame.AddrPC.Offset) == self) out.push_back(frame.AddrPC.Offset);
    }
    return out;
}

std::string thread_name(HANDLE h, DWORD id)
{
    PWSTR desc = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc)
    {
        std::wstring w(desc);
        name.assign(w.begin(), w.end());
        LocalFree(desc);
    }
    return name.empty() ? "unnamed" : name;
}
}

namespace
{
LONG WINAPI report_crash(EXCEPTION_POINTERS* info)
{
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    auto* rec = info->ExceptionRecord;
    std::fflush(stdout);
    std::fprintf(stderr, "\n[crash] Orchard crashed: exception 0x%08lx at %s\n", rec->ExceptionCode,
                 symbol(process, uint64_t(rec->ExceptionAddress)).c_str());
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        std::fprintf(stderr, "[crash] %s address 0x%llx\n", rec->ExceptionInformation[0] ? "writing" : "reading",
                     (unsigned long long)rec->ExceptionInformation[1]);
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC = {ctx.Rip, 0, AddrModeFlat};
    frame.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
    frame.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
    for (int i = 0; i < 20; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        std::fprintf(stderr, "[crash]   %s\n", symbol(process, frame.AddrPC.Offset).c_str());
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
}

void install_crash_reporter()
{
    SetUnhandledExceptionFilter(report_crash);
}

void name_host_thread(const std::string& name)
{
    std::wstring w(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), w.c_str());
}

uint64_t process_private_bytes()
{
    PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

std::atomic<int64_t> g_last_present_ns{0};

void note_frame_presented()
{
    g_last_present_ns = std::chrono::steady_clock::now().time_since_epoch().count();
}

void start_hitch_monitor(double threshold)
{
    DWORD main_id = GetCurrentThreadId();
    std::thread([threshold, main_id] {
        SetThreadDescription(GetCurrentThread(), L"hitch-monitor");
        HANDLE process = GetCurrentProcess();
        HMODULE self = GetModuleHandleW(nullptr);
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        DWORD self_id = GetCurrentThreadId();
        std::unordered_map<uint64_t, std::string> names;
        auto name_of = [&](uint64_t a) -> const std::string& {
            auto it = names.find(a);
            if (it != names.end()) return it->second;
            return names[a] = symbol(process, a);
        };
        auto now_ns = [] { return std::chrono::steady_clock::now().time_since_epoch().count(); };
        int hitch_no = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            int64_t last = g_last_present_ns;
            if (!last || (now_ns() - last) < int64_t(threshold * 1e9)) continue;

            struct Thread
            {
                HANDLE h;
                std::string name;
                bool main;
            };
            std::vector<Thread> threads;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self_id) continue;
                HANDLE h =
                    OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (h) threads.push_back({h, thread_name(h, te.th32ThreadID), te.th32ThreadID == main_id});
            }
            CloseHandle(snap);

            std::map<std::string, uint64_t> main_counts, busy_threads;
            std::map<std::string, std::map<std::string, uint64_t>> busy_syms;
            uint64_t main_total = 0;
            while (g_last_present_ns == last && now_ns() - last < int64_t(6e9))
            {
                for (auto& t : threads)
                {
                    if (SuspendThread(t.h) == DWORD(-1)) continue;
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_FULL;
                    bool got = GetThreadContext(t.h, &ctx);
                    std::vector<uint64_t> frames;
                    HMODULE m = got ? module_of(ctx.Rip) : nullptr;
                    if (got && t.main && m && m != self) frames = orchard_frames(process, t.h, ctx, self, 3);
                    ResumeThread(t.h);
                    if (!got) continue;
                    std::string key = name_of(ctx.Rip);
                    bool waiting = key.find("Wait") != std::string::npos || key.find("Delay") != std::string::npos ||
                                   key.find("RemoveIoCompletion") != std::string::npos || key.find("AlertThread") != std::string::npos;
                    if (t.main)
                    {
                        for (uint64_t f : frames)
                            key += "  <-  " + name_of(f);
                        ++main_counts[key];
                        ++main_total;
                    }
                    else if (!waiting)
                    {
                        ++busy_threads[t.name];
                        ++busy_syms[t.name][key];
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            double seconds = double(now_ns() - last) / 1e9;
            for (auto& t : threads)
                CloseHandle(t.h);

            std::vector<std::pair<std::string, uint64_t>> v(main_counts.begin(), main_counts.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::printf("\n[hitch] #%d: no frame for %.2fs; main thread (%llu samples):\n", ++hitch_no, seconds,
                        (unsigned long long)main_total);
            for (size_t i = 0; i < v.size() && i < 12; ++i)
                std::printf("[hitch]   %5.1f%%  %s\n", 100.0 * v[i].second / std::max<uint64_t>(main_total, 1), v[i].first.c_str());
            std::vector<std::pair<std::string, uint64_t>> b(busy_threads.begin(), busy_threads.end());
            std::sort(b.begin(), b.end(), [](auto& x, auto& y) { return x.second > y.second; });
            std::printf("[hitch]   busy other threads:");
            for (size_t i = 0; i < b.size() && i < 8; ++i)
                std::printf(" %s(%llu)", b[i].first.c_str(), (unsigned long long)b[i].second);
            std::printf("\n");
            for (size_t i = 0; i < b.size() && i < 2; ++i)
            {
                std::vector<std::pair<std::string, uint64_t>> syms(busy_syms[b[i].first].begin(), busy_syms[b[i].first].end());
                std::sort(syms.begin(), syms.end(), [](auto& x, auto& y) { return x.second > y.second; });
                for (size_t j = 0; j < syms.size() && j < 5; ++j)
                    std::printf("[hitch]     %s: %5.1f%%  %s\n", b[i].first.c_str(), 100.0 * syms[j].second / b[i].second,
                                syms[j].first.c_str());
            }
            if (g_last_present_ns == last) std::printf("[hitch]   still frozen when this report was taken\n");
            std::fflush(stdout);
            while (g_last_present_ns == last)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

void start_profiler(double from, double to)
{
    std::thread([from, to] {
        SetThreadDescription(GetCurrentThread(), L"profiler");
        HANDLE process = GetCurrentProcess();
        HMODULE self = GetModuleHandleW(nullptr);
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        DWORD self_id = GetCurrentThreadId();
        auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        while (elapsed() < from)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

        std::vector<Sample> samples;
        std::vector<std::string> groups;
        std::map<std::string, uint32_t> group_ids;
        struct Thread
        {
            HANDLE h;
            uint32_t group;
        };
        std::unordered_map<DWORD, Thread> threads;
        double next_refresh = 0;
        std::printf("[profile] sampling from %.0fs to %.0fs\n", from, to);
        while (elapsed() < to)
        {
            if (elapsed() >= next_refresh)
            {
                next_refresh = elapsed() + 0.2;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te = {sizeof(te)};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self_id) continue;
                    auto& t = threads[te.th32ThreadID];
                    if (!t.h)
                        t.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                         te.th32ThreadID);
                    if (!t.h) continue;
                    std::string name = thread_name(t.h, te.th32ThreadID);
                    auto [it, fresh] = group_ids.try_emplace(name, uint32_t(groups.size()));
                    if (fresh) groups.push_back(name);
                    t.group = it->second;
                }
                CloseHandle(snap);
            }
            for (auto& [id, t] : threads)
            {
                if (!t.h || SuspendThread(t.h) == DWORD(-1)) continue;
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(t.h, &ctx))
                {
                    HMODULE m = module_of(ctx.Rip);
                    uint64_t caller = (m && m != self) ? first_orchard_frame(process, t.h, ctx, self) : 0;
                    samples.push_back({t.group, ctx.Rip, caller});
                }
                ResumeThread(t.h);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::unordered_map<uint64_t, std::string> names;
        auto name_of = [&](uint64_t a) -> const std::string& {
            auto it = names.find(a);
            if (it != names.end()) return it->second;
            return names[a] = symbol(process, a);
        };
        std::vector<std::map<std::string, uint64_t>> per_group(groups.size());
        std::vector<uint64_t> totals(groups.size());
        for (auto& s : samples)
        {
            std::string key = name_of(s.rip);
            if (s.caller) key += "  <-  " + name_of(s.caller);
            ++per_group[s.group][key];
            ++totals[s.group];
        }
        std::vector<uint32_t> order(groups.size());
        for (uint32_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return totals[a] > totals[b]; });
        for (uint32_t g : order)
        {
            if (!totals[g]) continue;
            std::vector<std::pair<std::string, uint64_t>> v(per_group[g].begin(), per_group[g].end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::printf("\n[profile] thread group '%s': %llu samples\n", groups[g].c_str(), (unsigned long long)totals[g]);
            for (size_t i = 0; i < v.size() && i < (groups[g] == "main" ? 45 : 12); ++i)
                std::printf("[profile] %6.2f%%  %s\n", 100.0 * v[i].second / totals[g], v[i].first.c_str());
        }
        std::fflush(stdout);
    }).detach();
}

}
