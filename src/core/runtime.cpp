#include "core/runtime.h"

#include "loader/cache_images.h"
#include "loader/dyld_cache.h"
#include "loader/linker.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "objc/runtime.h"

#include <algorithm>

namespace orchard
{
void map_commpage(Runtime& rt);

Runtime::Runtime()
{
    map_commpage(*this);
    objc = std::make_unique<objc::ObjcRuntime>(*this);
    threads = std::make_unique<Threads>(*this);
}

void Runtime::fail(Cpu& cpu)
{
    std::lock_guard g(cpus_lock_);
    if (halted_.exchange(true)) return;
    failure_ = {cpu.stop_reason(), cpu.name.empty() ? "thread " + std::to_string(cpu.thread_id) : cpu.name, cpu.stop_backtrace(),
                cpu.stop_hle_stack()};
    for (Cpu* other : cpus_)
        if (other != &cpu) other->exit_thread();
}

void Runtime::register_cpu(Cpu* cpu)
{
    std::lock_guard g(cpus_lock_);
    cpus_.push_back(cpu);
    if (halted_) cpu->exit_thread();
}

Cpu* Runtime::find_cpu(uint64_t thread_id)
{
    std::lock_guard g(cpus_lock_);
    for (Cpu* c : cpus_)
        if (c->thread_id == thread_id) return c;
    return nullptr;
}

std::vector<Cpu*> Runtime::all_cpus()
{
    std::lock_guard g(cpus_lock_);
    return cpus_;
}

void Runtime::unregister_cpu(Cpu* cpu)
{
    std::lock_guard g(cpus_lock_);
    cpus_.erase(std::remove(cpus_.begin(), cpus_.end(), cpu), cpus_.end());
}
Runtime::~Runtime() = default;

std::string Runtime::describe(GuestAddr addr) const
{
    if (linker) return linker->symbolicate(addr);
    return mem.describe(addr);
}

}
