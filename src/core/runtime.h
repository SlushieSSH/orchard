#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "core/heap.h"
#include "core/memory.h"
#include "core/vfs.h"
#include "hle/hle.h"

namespace orchard
{
class Linker;
class Cpu;
class Threads;
class DyldCache;
class CacheImages;
namespace objc
{
class ObjcRuntime;
}

struct Runtime
{
    Memory mem;
    Heap heap{mem};
    Hle hle{*this};
    static constexpr size_t kMaxProcessors = 64;
    Dynarmic::ExclusiveMonitor monitor{kMaxProcessors};
    Linker* linker = nullptr;
    std::unique_ptr<DyldCache> cache;
    std::unique_ptr<CacheImages> cache_images;
    std::unique_ptr<objc::ObjcRuntime> objc;
    bool lenient = false;
    bool run_all_frameworks = false;

    std::unique_ptr<Threads> threads;
    Vfs vfs;

    std::mutex env_lock;
    std::map<std::string, std::string> env;
    std::map<std::string, GuestAddr> env_strings;
    Cpu* main_cpu = nullptr;

    Runtime();
    ~Runtime();

    void fail(Cpu& cpu);
    bool halted() const { return halted_; }
    struct Failure
    {
        std::string reason;
        std::string thread;
        std::vector<GuestAddr> backtrace;
        std::vector<std::string> hle_stack;
    };
    const Failure& failure() const { return failure_; }

    void register_cpu(Cpu* cpu);
    Cpu* find_cpu(uint64_t thread_id);
    std::vector<Cpu*> all_cpus();
    void unregister_cpu(Cpu* cpu);

    template <typename Lock, typename Pred>
    bool wait(std::condition_variable_any& cv, Lock& lock, Pred pred,
              std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max())
    {
        while (!pred())
        {
            if (halted_) return false;
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            auto slice = std::min(deadline, now + std::chrono::milliseconds(50));
            cv.wait_until(lock, slice);
        }
        return true;
    }

    std::string describe(GuestAddr addr) const;

private:
    std::atomic<bool> halted_ = false;
    Failure failure_;
    std::mutex cpus_lock_;
    std::vector<Cpu*> cpus_;
};

}
