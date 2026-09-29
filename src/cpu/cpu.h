#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/memory.h"

namespace Dynarmic::A64
{
class Jit;
}

namespace orchard
{
struct Runtime;
class JitCallbacks;

class Cpu
{
public:
    Cpu(Runtime& rt, size_t processor_id, size_t cache_mb = 128);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    Runtime& rt;
    Memory& mem;

    uint64_t x(int i) const;
    void set_x(int i, uint64_t v);
    double d(int i) const;
    void set_d(int i, double v);
    float s(int i) const;
    void set_s(int i, float v);
    GuestAddr sp() const;
    void set_sp(GuestAddr v);
    GuestAddr pc() const;
    GuestAddr lr() const { return x(30); }

    uint64_t arg(int i) const { return x(i); }
    void ret(uint64_t v) { set_x(0, v); }

    void set_initial_stack(GuestAddr top) { initial_sp_ = top; }
    uint64_t* tpidrro_el0() { return &tpidrro_; }

    uint64_t call(GuestAddr fn, std::initializer_list<uint64_t> args = {});

    void stop(std::string reason);
    void exit_thread();
    bool stopped() const { return stopped_; }
    const std::string& stop_reason() const { return stop_reason_; }
    const std::vector<GuestAddr>& stop_backtrace() const { return stop_backtrace_; }
    const std::vector<std::string>& stop_hle_stack() const { return stop_hle_stack_; }

    uint64_t hle_calls() const { return svc_count_; }

    void request_suspend();
    void resume();
    bool wait_until_parked();
    void read_thread_state(uint64_t out[34]) const;

    void park_for_reuse();
    void unpark_for_reuse();

    GuestAddr pthread = 0;
    GuestAddr tsd = 0;
    uint64_t thread_id = 0;
    bool is_main = false;
    std::string name;
    GuestAddr stack_top = 0;
    uint64_t stack_size = 0;
    bool exited = false;
    uint64_t exit_value = 0;

    std::vector<GuestAddr> autorelease_pool;
    std::vector<const std::string*> hle_stack;

    std::unordered_map<uint64_t, GuestAddr> tlv_blocks;

private:
    friend class JitCallbacks;

    struct Level;
    Level& current() const;
    void on_svc(Level& level);
    void in_hle_dispatch(Level& level, GuestAddr stub);
    void halt_levels();

    std::vector<std::unique_ptr<Level>> levels_;
    std::mutex levels_lock_;
    int depth_ = -1;
    size_t processor_id_;
    size_t cache_mb_;
    GuestAddr initial_sp_ = 0;
    uint64_t tpidrro_ = 0;
    uint64_t tpidr_ = 0;
    std::atomic<bool> stopped_ = false;
    void park_if_suspended();
    std::atomic<int> suspend_count_ = 0;
    std::atomic<bool> parked_ = true;
    std::mutex suspend_lock_;
    std::condition_variable_any suspend_cv_;
    std::string stop_reason_;
    std::vector<GuestAddr> stop_backtrace_;
    std::vector<std::string> stop_hle_stack_;
    uint64_t svc_count_ = 0;
};

}
