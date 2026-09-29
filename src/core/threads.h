#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/memory.h"

namespace orchard
{
struct Runtime;
class Cpu;

class Threads
{
public:
    static constexpr uint64_t kTsdOffset = 0x200;
    static constexpr uint64_t kErrnoOffset = 0x100;
    static constexpr uint64_t kTsdSlots = 512;

    explicit Threads(Runtime& rt) : rt_(rt) {}
    ~Threads();

    void attach(Cpu& cpu, uint64_t stack_size, std::string name);

    GuestAddr create(GuestAddr start, GuestAddr arg, uint64_t stack_size, bool detached);

    void spawn_host(std::string name, std::function<void(Cpu&)> body);

    bool join(GuestAddr pthread, uint64_t* retval);
    void detach(GuestAddr pthread);

    Cpu* cpu_of(GuestAddr pthread);
    size_t live_threads() const;

    static constexpr uint64_t kFirstKey = 256;
    bool create_key(GuestAddr destructor, uint64_t* key);
    void delete_key(uint64_t key);

private:
    struct Record
    {
        GuestAddr pthread = 0;
        std::thread host;
        std::unique_ptr<Cpu> cpu;
        std::atomic<bool> done = false;
        uint64_t retval = 0;
        bool detached = false;
    };

    void run_tsd_destructors(Cpu& cpu);
    std::unique_ptr<Cpu> take_cpu(uint64_t id);
    void recycle_cpu(std::unique_ptr<Cpu> cpu);

    Runtime& rt_;
    mutable std::mutex lock_;
    std::condition_variable_any done_cv_;
    std::unordered_map<GuestAddr, std::shared_ptr<Record>> records_;
    std::vector<std::unique_ptr<Cpu>> idle_cpus_;
    std::atomic<uint64_t> next_id_ = 1;
    std::unordered_map<uint64_t, GuestAddr> key_destructors_;
    uint64_t next_key_ = kFirstKey;
};

}
