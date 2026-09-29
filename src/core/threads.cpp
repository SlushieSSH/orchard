#include "core/threads.h"

#include <vector>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "host/profiler.h"

namespace orchard
{
Threads::~Threads()
{
    std::lock_guard g(lock_);
    for (auto& [_, r] : records_)
        if (r->host.joinable()) r->host.detach();
}

void Threads::attach(Cpu& cpu, uint64_t stack_size, std::string name)
{
    stack_size = page_align_up(std::max<uint64_t>(stack_size, 64 * 1024));
    GuestAddr stack;
    if (cpu.stack_top && cpu.stack_size >= stack_size)
    {
        stack_size = cpu.stack_size;
        stack = cpu.stack_top - stack_size;
    }
    else
    {
        if (cpu.stack_top) rt_.mem.unmap(cpu.stack_top - cpu.stack_size, cpu.stack_size);
        stack = rt_.mem.map_anywhere(stack_size, "stack:" + name);
    }
    GuestAddr block = rt_.mem.map_anywhere(kTsdOffset + kTsdSlots * 8, "pthread:" + name);
    cpu.set_initial_stack(stack + stack_size);
    cpu.stack_top = stack + stack_size;
    cpu.stack_size = stack_size;
    cpu.pthread = block;
    cpu.tsd = block + kTsdOffset;
    rt_.mem.write<uint64_t>(cpu.tsd, block);
    *cpu.tpidrro_el0() = cpu.tsd;
    cpu.thread_id = next_id_++;
    cpu.name = std::move(name);
}

GuestAddr Threads::create(GuestAddr start, GuestAddr arg, uint64_t stack_size, bool detached)
{
    auto rec = std::make_shared<Record>();
    uint64_t id = next_id_.load();
    rec->cpu = take_cpu(id);
    attach(*rec->cpu, stack_size ? stack_size : 512 * 1024, "thread-" + std::to_string(id));
    rec->pthread = rec->cpu->pthread;
    rec->detached = detached;
    {
        std::lock_guard g(lock_);
        records_[rec->pthread] = rec;
    }
    rec->host = std::thread([this, rec, start, arg] {
        Cpu& cpu = *rec->cpu;
        name_host_thread(cpu.name);
        rec->retval = cpu.call(start, {arg});
        if (cpu.exited) rec->retval = cpu.exit_value;
        if (!rt_.halted()) run_tsd_destructors(cpu);
        std::unique_ptr<Cpu> finished;
        {
            std::lock_guard g(lock_);
            rec->done = true;
            finished = std::move(rec->cpu);
            if (rec->detached) records_.erase(rec->pthread);
            done_cv_.notify_all();
        }
        if (!rt_.halted()) recycle_cpu(std::move(finished));
    });
    if (detached) rec->host.detach();
    return rec->pthread;
}

void Threads::spawn_host(std::string name, std::function<void(Cpu&)> body)
{
    uint64_t id = next_id_.load();
    std::thread([this, name = std::move(name), body = std::move(body), id]() mutable {
        Cpu cpu(rt_, size_t(id), 64);
        attach(cpu, 512 * 1024, std::move(name));
        name_host_thread(cpu.name);
        body(cpu);
    }).detach();
}

std::unique_ptr<Cpu> Threads::take_cpu(uint64_t id)
{
    {
        std::lock_guard g(lock_);
        if (!idle_cpus_.empty())
        {
            auto cpu = std::move(idle_cpus_.back());
            idle_cpus_.pop_back();
            cpu->unpark_for_reuse();
            return cpu;
        }
    }
    return std::make_unique<Cpu>(rt_, size_t(id), 64);
}

void Threads::recycle_cpu(std::unique_ptr<Cpu> cpu)
{
    if (!cpu) return;
    for (auto& [key, block] : cpu->tlv_blocks)
        rt_.heap.free(block);
    cpu->park_for_reuse();
    std::lock_guard g(lock_);
    idle_cpus_.push_back(std::move(cpu));
}

bool Threads::join(GuestAddr pthread, uint64_t* retval)
{
    std::shared_ptr<Record> rec;
    {
        std::lock_guard g(lock_);
        auto it = records_.find(pthread);
        if (it == records_.end()) return false;
        rec = it->second;
    }
    std::unique_lock l(lock_);
    if (!rt_.wait(done_cv_, l, [&] { return rec->done.load(); })) return false;
    records_.erase(pthread);
    l.unlock();
    if (rec->host.joinable()) rec->host.join();
    rt_.mem.unmap(pthread, kTsdOffset + kTsdSlots * 8);
    if (retval) *retval = rec->retval;
    return true;
}

void Threads::detach(GuestAddr pthread)
{
    std::lock_guard g(lock_);
    auto it = records_.find(pthread);
    if (it == records_.end()) return;
    it->second->detached = true;
    if (it->second->host.joinable()) it->second->host.detach();
    if (it->second->done)
    {
        rt_.mem.unmap(pthread, kTsdOffset + kTsdSlots * 8);
        records_.erase(it);
    }
}

Cpu* Threads::cpu_of(GuestAddr pthread)
{
    std::lock_guard g(lock_);
    auto it = records_.find(pthread);
    return it == records_.end() ? nullptr : it->second->cpu.get();
}

size_t Threads::live_threads() const
{
    std::lock_guard g(lock_);
    size_t n = 0;
    for (auto& [_, r] : records_)
        n += !r->done;
    return n;
}

bool Threads::create_key(GuestAddr destructor, uint64_t* key)
{
    std::lock_guard g(lock_);
    if (next_key_ >= kTsdSlots) return false;
    *key = next_key_++;
    key_destructors_[*key] = destructor;
    return true;
}

void Threads::delete_key(uint64_t key)
{
    std::lock_guard g(lock_);
    key_destructors_.erase(key);
}

void Threads::run_tsd_destructors(Cpu& cpu)
{
    std::vector<std::pair<uint64_t, GuestAddr>> keys;
    {
        std::lock_guard g(lock_);
        keys.assign(key_destructors_.begin(), key_destructors_.end());
    }
    for (int pass = 0; pass < 4; ++pass)
    {
        bool any = false;
        for (auto [key, dtor] : keys)
        {
            GuestAddr slot = cpu.tsd + key * 8;
            uint64_t value = rt_.mem.read<uint64_t>(slot);
            if (!value || !dtor) continue;
            rt_.mem.write<uint64_t>(slot, 0);
            cpu.call(dtor, {value});
            any = true;
            if (cpu.stopped()) return;
        }
        if (!any) break;
    }
}

}
