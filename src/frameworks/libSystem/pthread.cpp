#include <chrono>
#include <cstring>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "frameworks/libSystem/errno.h"
#include "hle/hle.h"
#include "host/profiler.h"

namespace orchard
{
namespace
{
using clock = std::chrono::steady_clock;

struct GMutex
{
    std::mutex inner;
    std::condition_variable_any cv;
    Cpu* owner = nullptr;
    unsigned depth = 0;
    bool recursive = false;
};

struct GCond
{
    std::mutex m;
    std::condition_variable_any cv;
    uint64_t seq = 0;
};

struct GRwLock
{
    std::shared_mutex m;
    Cpu* writer = nullptr;
};

struct GSem
{
    std::mutex m;
    std::condition_variable_any cv;
    int64_t count = 0;
};

struct GOnce
{
    std::mutex m;
    bool done = false;
};

template <typename T> class Table
{
public:
    T& get(GuestAddr addr)
    {
        std::lock_guard g(lock_);
        auto& p = items_[addr];
        if (!p) p = std::make_unique<T>();
        return *p;
    }
    bool has(GuestAddr addr)
    {
        std::lock_guard g(lock_);
        return items_.count(addr) != 0;
    }
    void erase(GuestAddr addr)
    {
        std::lock_guard g(lock_);
        items_.erase(addr);
    }

private:
    std::mutex lock_;
    std::unordered_map<GuestAddr, std::unique_ptr<T>> items_;
};

Table<GMutex> mutexes;
Table<GCond> conds;
Table<GRwLock> rwlocks;
Table<GSem> semaphores;
Table<GOnce> onces;

constexpr uint32_t kRecursiveMutexInitSig = 0x32aaaba2;
constexpr uint64_t PTHREAD_MUTEX_RECURSIVE = 2;

GMutex& mutex_for(Cpu& c, GuestAddr addr)
{
    bool fresh = !mutexes.has(addr);
    GMutex& m = mutexes.get(addr);
    if (fresh && c.mem.read<uint32_t>(addr) == kRecursiveMutexInitSig) m.recursive = true;
    return m;
}

bool lock(Cpu& c, GMutex& m, bool try_only = false, bool* busy = nullptr)
{
    std::unique_lock l(m.inner);
    if (m.owner == &c && m.recursive)
    {
        ++m.depth;
        return true;
    }
    if (try_only && m.owner)
    {
        if (busy) *busy = true;
        return true;
    }
    if (!c.rt.wait(m.cv, l, [&] { return m.owner == nullptr; })) return false;
    m.owner = &c;
    m.depth = 1;
    return true;
}

void unlock(Cpu& c, GMutex& m)
{
    std::lock_guard l(m.inner);
    if (m.owner != &c) return;
    if (--m.depth == 0)
    {
        m.owner = nullptr;
        m.cv.notify_one();
    }
}

int cond_wait(Cpu& c, GCond& cond, GMutex& m, clock::time_point deadline = clock::time_point::max())
{
    std::unique_lock l(m.inner);
    if (m.owner != &c) return EPERM_;
    unsigned depth = m.depth;
    m.owner = nullptr;
    m.depth = 0;
    m.cv.notify_one();
    bool signaled;
    {
        std::unique_lock cl(cond.m);
        l.unlock();
        uint64_t seen = cond.seq;
        signaled = c.rt.wait(cond.cv, cl, [&] { return cond.seq != seen; }, deadline);
    }
    if (c.rt.halted())
    {
        c.exit_thread();
        return 0;
    }
    l.lock();
    c.rt.wait(m.cv, l, [&] { return m.owner == nullptr; });
    m.owner = &c;
    m.depth = depth;
    return signaled ? 0 : ETIMEDOUT_;
}

clock::time_point deadline_from_timespec(Cpu& c, GuestAddr ts, bool relative)
{
    int64_t sec = c.mem.read<int64_t>(ts), nsec = c.mem.read<int64_t>(ts + 8);
    auto dur = std::chrono::seconds(sec) + std::chrono::nanoseconds(nsec);
    if (relative) return clock::now() + std::chrono::duration_cast<clock::duration>(dur);
    auto wall_now = std::chrono::system_clock::now().time_since_epoch();
    auto remaining = std::chrono::duration_cast<clock::duration>(dur - wall_now);
    return clock::now() + std::max(remaining, clock::duration::zero());
}

void sem_wait_on(Cpu& c, GSem& s, clock::time_point deadline, bool* timed_out)
{
    std::unique_lock l(s.m);
    bool ok = c.rt.wait(s.cv, l, [&] { return s.count > 0; }, deadline);
    if (ok) --s.count;
    if (timed_out) *timed_out = !ok;
    if (!ok && c.rt.halted()) c.exit_thread();
}

void sem_post_to(GSem& s)
{
    std::lock_guard l(s.m);
    ++s.count;
    s.cv.notify_one();
}

constexpr uint64_t ATTR_DETACH = 8, ATTR_STACKSIZE = 16, ATTR_STACKADDR = 24;
constexpr uint64_t kDefaultStack = 512 * 1024;

Cpu* thread_cpu(Cpu& c, GuestAddr pthread)
{
    if (pthread == c.pthread) return &c;
    if (c.rt.main_cpu && pthread == c.rt.main_cpu->pthread) return c.rt.main_cpu;
    return c.rt.threads->cpu_of(pthread);
}

void register_threads(Hle& h)
{
    h.fn("_pthread_attr_init", [](Cpu& c) {
        std::memset(c.mem.host(c.arg(0)), 0, 64);
        c.mem.write<uint64_t>(c.arg(0) + ATTR_STACKSIZE, kDefaultStack);
        c.ret(0);
    });
    h.fn("_pthread_attr_destroy", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_attr_setdetachstate", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(0) + ATTR_DETACH, c.arg(1));
        c.ret(0);
    });
    h.fn("_pthread_attr_setstacksize", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(0) + ATTR_STACKSIZE, c.arg(1));
        c.ret(0);
    });
    h.fn("_pthread_attr_setstack", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(0) + ATTR_STACKADDR, c.arg(1));
        c.mem.write<uint64_t>(c.arg(0) + ATTR_STACKSIZE, c.arg(2));
        c.ret(0);
    });
    h.fn("_pthread_attr_setschedparam", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_attr_setschedpolicy", [](Cpu& c) { c.ret(0); });

    h.fn("_pthread_create", [](Cpu& c) {
        GuestAddr attr = c.arg(1);
        uint64_t stack = attr ? c.mem.read<uint64_t>(attr + ATTR_STACKSIZE) : kDefaultStack;
        bool detached = attr && c.mem.read<uint64_t>(attr + ATTR_DETACH) == 2;
        GuestAddr t = c.rt.threads->create(c.arg(2), c.arg(3), stack, detached);
        c.mem.write<uint64_t>(c.arg(0), t);
        c.ret(0);
    });
    h.fn("_pthread_join", [](Cpu& c) {
        uint64_t value = 0;
        if (!c.rt.threads->join(c.arg(0), &value))
        {
            if (c.rt.halted()) return c.exit_thread();
            return c.ret(ESRCH_);
        }
        if (c.arg(1)) c.mem.write<uint64_t>(c.arg(1), value);
        c.ret(0);
    });
    h.fn("_pthread_detach", [](Cpu& c) {
        c.rt.threads->detach(c.arg(0));
        c.ret(0);
    });
    h.fn("_pthread_exit", [](Cpu& c) {
        c.exited = true;
        c.exit_value = c.arg(0);
        c.exit_thread();
    });
    h.fn("_pthread_self", [](Cpu& c) { c.ret(c.pthread); });
    h.fn("_pthread_equal", [](Cpu& c) { c.ret(c.arg(0) == c.arg(1)); });
    h.fn("_pthread_main_np", [](Cpu& c) { c.ret(c.is_main); });
    h.fn("_pthread_threadid_np", [](Cpu& c) {
        Cpu* t = c.arg(0) ? thread_cpu(c, c.arg(0)) : &c;
        if (!t) return c.ret(ESRCH_);
        c.mem.write<uint64_t>(c.arg(1), t->thread_id);
        c.ret(0);
    });
    h.fn("_pthread_mach_thread_np", [](Cpu& c) {
        Cpu* t = thread_cpu(c, c.arg(0));
        c.ret(t ? 0x1000 + t->thread_id : 0);
    });
    h.fn("_mach_thread_self", [](Cpu& c) { c.ret(0x1000 + c.thread_id); });
    h.fn("_pthread_from_mach_thread_np", [](Cpu& c) { c.ret(c.arg(0) == 0x1000 + c.thread_id ? c.pthread : 0); });
    h.fn("_pthread_setname_np", [](Cpu& c) {
        c.name = c.mem.read_cstr(c.arg(0));
        name_host_thread(c.name);
        c.ret(0);
    });
    h.fn("_pthread_getname_np", [](Cpu& c) {
        Cpu* t = thread_cpu(c, c.arg(0));
        std::string n = t ? t->name : "";
        uint64_t len = c.arg(2);
        if (len)
        {
            n.resize(std::min<size_t>(n.size(), len - 1));
            c.mem.write_bytes(c.arg(1), n.c_str(), n.size() + 1);
        }
        c.ret(0);
    });
    h.fn("_pthread_get_stackaddr_np", [](Cpu& c) {
        Cpu* t = thread_cpu(c, c.arg(0));
        c.ret(t ? t->stack_top : 0);
    });
    h.fn("_pthread_get_stacksize_np", [](Cpu& c) {
        Cpu* t = thread_cpu(c, c.arg(0));
        c.ret(t ? t->stack_size : 0);
    });
    h.fn("_pthread_getschedparam", [](Cpu& c) {
        if (c.arg(1)) c.mem.write<int32_t>(c.arg(1), 1);
        if (c.arg(2)) c.mem.write<int32_t>(c.arg(2), 31);
        c.ret(0);
    });
    h.fn("_pthread_setschedparam", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_kill", [](Cpu& c) { c.ret(c.arg(1) == 0 ? 0 : ENOTSUP_); });
    h.fn("_pthread_sigmask", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_atfork", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_setcancelstate", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_setcanceltype", [](Cpu& c) { c.ret(0); });
    h.fn("_sched_yield", [](Cpu& c) {
        std::this_thread::yield();
        c.ret(0);
    });
    h.fn("_sched_get_priority_max", [](Cpu& c) { c.ret(47); });
    h.fn("_sched_get_priority_min", [](Cpu& c) { c.ret(15); });

    h.fn("_pthread_key_create", [](Cpu& c) {
        uint64_t key;
        if (!c.rt.threads->create_key(c.arg(1), &key)) return c.ret(EAGAIN_);
        c.mem.write<uint64_t>(c.arg(0), key);
        c.ret(0);
    });
    h.fn("_pthread_key_init_np", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_key_delete", [](Cpu& c) {
        c.rt.threads->delete_key(c.arg(0));
        c.ret(0);
    });
    h.fn("_pthread_getspecific", [](Cpu& c) { c.ret(c.arg(0) < Threads::kTsdSlots ? c.mem.read<uint64_t>(c.tsd + c.arg(0) * 8) : 0); });
    h.fn("_pthread_setspecific", [](Cpu& c) {
        if (c.arg(0) >= Threads::kTsdSlots) return c.ret(EINVAL_);
        c.mem.write<uint64_t>(c.tsd + c.arg(0) * 8, c.arg(1));
        c.ret(0);
    });

    h.fn("_pthread_once", [](Cpu& c) {
        GOnce& o = onces.get(c.arg(0));
        std::lock_guard g(o.m);
        if (!o.done)
        {
            c.call(c.arg(1));
            o.done = true;
        }
        c.ret(0);
    });
}

void register_mutexes(Hle& h)
{
    h.fn("_pthread_mutexattr_init", [](Cpu& c) {
        std::memset(c.mem.host(c.arg(0)), 0, 16);
        c.ret(0);
    });
    h.fn("_pthread_mutexattr_destroy", [](Cpu& c) { c.ret(0); });
    h.fn("_pthread_mutexattr_settype", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(0) + 8, c.arg(1));
        c.ret(0);
    });
    h.fn("_pthread_mutexattr_setprotocol", [](Cpu& c) { c.ret(0); });

    h.fn("_pthread_mutex_init", [](Cpu& c) {
        mutexes.erase(c.arg(0));
        GMutex& m = mutexes.get(c.arg(0));
        m.recursive = c.arg(1) && c.mem.read<uint64_t>(c.arg(1) + 8) == PTHREAD_MUTEX_RECURSIVE;
        c.ret(0);
    });
    h.fn("_pthread_mutex_destroy", [](Cpu& c) {
        mutexes.erase(c.arg(0));
        c.ret(0);
    });
    h.fn("_pthread_mutex_lock", [](Cpu& c) {
        if (!lock(c, mutex_for(c, c.arg(0)))) return c.exit_thread();
        c.ret(0);
    });
    h.fn("_pthread_mutex_trylock", [](Cpu& c) {
        bool busy = false;
        lock(c, mutex_for(c, c.arg(0)), true, &busy);
        c.ret(busy ? EBUSY_ : 0);
    });
    h.fn("_pthread_mutex_unlock", [](Cpu& c) {
        unlock(c, mutex_for(c, c.arg(0)));
        c.ret(0);
    });

    h.fn("_pthread_cond_init", [](Cpu& c) {
        conds.erase(c.arg(0));
        c.ret(0);
    });
    h.fn("_pthread_cond_destroy", [](Cpu& c) {
        conds.erase(c.arg(0));
        c.ret(0);
    });
    h.fn("_pthread_cond_wait", [](Cpu& c) { c.ret(cond_wait(c, conds.get(c.arg(0)), mutex_for(c, c.arg(1)))); });
    h.fn("_pthread_cond_timedwait",
         [](Cpu& c) { c.ret(cond_wait(c, conds.get(c.arg(0)), mutex_for(c, c.arg(1)), deadline_from_timespec(c, c.arg(2), false))); });
    h.fn("_pthread_cond_timedwait_relative_np",
         [](Cpu& c) { c.ret(cond_wait(c, conds.get(c.arg(0)), mutex_for(c, c.arg(1)), deadline_from_timespec(c, c.arg(2), true))); });
    auto signal = [](Cpu& c) {
        GCond& cond = conds.get(c.arg(0));
        {
            std::lock_guard g(cond.m);
            ++cond.seq;
        }
        cond.cv.notify_all();
        c.ret(0);
    };
    h.fn("_pthread_cond_signal", signal);
    h.fn("_pthread_cond_broadcast", signal);

    h.fn("_pthread_rwlock_init", [](Cpu& c) {
        rwlocks.erase(c.arg(0));
        c.ret(0);
    });
    h.fn("_pthread_rwlock_destroy", [](Cpu& c) {
        rwlocks.erase(c.arg(0));
        c.ret(0);
    });
    h.fn("_pthread_rwlock_rdlock", [](Cpu& c) {
        rwlocks.get(c.arg(0)).m.lock_shared();
        c.ret(0);
    });
    h.fn("_pthread_rwlock_tryrdlock", [](Cpu& c) { c.ret(rwlocks.get(c.arg(0)).m.try_lock_shared() ? 0 : EBUSY_); });
    h.fn("_pthread_rwlock_wrlock", [](Cpu& c) {
        GRwLock& rw = rwlocks.get(c.arg(0));
        rw.m.lock();
        rw.writer = &c;
        c.ret(0);
    });
    h.fn("_pthread_rwlock_trywrlock", [](Cpu& c) {
        GRwLock& rw = rwlocks.get(c.arg(0));
        if (!rw.m.try_lock()) return c.ret(EBUSY_);
        rw.writer = &c;
        c.ret(0);
    });
    h.fn("_pthread_rwlock_unlock", [](Cpu& c) {
        GRwLock& rw = rwlocks.get(c.arg(0));
        if (rw.writer == &c)
        {
            rw.writer = nullptr;
            rw.m.unlock();
        }
        else
        {
            rw.m.unlock_shared();
        }
        c.ret(0);
    });

    auto unfair_lock = [](Cpu& c) {
        if (!lock(c, mutexes.get(c.arg(0)))) c.exit_thread();
    };
    auto unfair_unlock = [](Cpu& c) { unlock(c, mutexes.get(c.arg(0))); };
    h.fn("_os_unfair_lock_lock", unfair_lock);
    h.fn("_os_unfair_lock_unlock", unfair_unlock);
    h.fn("_os_unfair_lock_trylock", [](Cpu& c) {
        bool busy = false;
        lock(c, mutexes.get(c.arg(0)), true, &busy);
        c.ret(!busy);
    });
    h.fn("_OSSpinLockLock", unfair_lock);
    h.fn("_OSSpinLockUnlock", unfair_unlock);
}

void register_semaphores(Hle& h)
{
    static std::atomic<uint64_t> next_port{0x5e000};
    h.fn("_semaphore_create", [](Cpu& c) {
        GuestAddr port = next_port.fetch_add(4);
        semaphores.get(port).count = int64_t(int32_t(c.arg(3)));
        c.mem.write<uint32_t>(c.arg(1), uint32_t(port));
        c.ret(0);
    });
    h.fn("_semaphore_destroy", [](Cpu& c) {
        semaphores.erase(c.arg(1));
        c.ret(0);
    });
    h.fn("_semaphore_signal", [](Cpu& c) {
        sem_post_to(semaphores.get(c.arg(0)));
        c.ret(0);
    });
    h.fn("_semaphore_wait", [](Cpu& c) {
        sem_wait_on(c, semaphores.get(c.arg(0)), clock::time_point::max(), nullptr);
        c.ret(0);
    });

    static std::mutex names_lock;
    static std::unordered_map<std::string, GuestAddr> names;
    h.fn("_sem_open", [](Cpu& c) {
        std::string name = c.mem.read_cstr(c.arg(0));
        std::lock_guard g(names_lock);
        auto it = names.find(name);
        if (it == names.end())
        {
            GuestAddr handle = c.mem.alloc_system(8, 8);
            semaphores.get(handle).count = int64_t(uint32_t(c.mem.read<uint32_t>(c.sp() + 8)));
            it = names.emplace(name, handle).first;
        }
        c.ret(it->second);
    });
    h.fn("_sem_close", [](Cpu& c) { c.ret(0); });
    h.fn("_sem_unlink", [](Cpu& c) { c.ret(0); });
    h.fn("_sem_post", [](Cpu& c) {
        sem_post_to(semaphores.get(c.arg(0)));
        c.ret(0);
    });
    h.fn("_sem_wait", [](Cpu& c) {
        sem_wait_on(c, semaphores.get(c.arg(0)), clock::time_point::max(), nullptr);
        c.ret(0);
    });
    h.fn("_sem_trywait", [](Cpu& c) {
        GSem& s = semaphores.get(c.arg(0));
        std::lock_guard l(s.m);
        if (s.count <= 0) return c.ret(uint64_t(-1));
        --s.count;
        c.ret(0);
    });
}

void register_sleep(Hle& h)
{
    h.fn("_usleep", [](Cpu& c) {
        std::this_thread::sleep_for(std::chrono::microseconds(c.arg(0)));
        c.ret(0);
    });
    h.fn("_sleep", [](Cpu& c) {
        std::this_thread::sleep_for(std::chrono::seconds(c.arg(0)));
        c.ret(0);
    });
    h.fn("_nanosleep", [](Cpu& c) {
        GuestAddr ts = c.arg(0);
        std::this_thread::sleep_for(std::chrono::seconds(c.mem.read<int64_t>(ts)) + std::chrono::nanoseconds(c.mem.read<int64_t>(ts + 8)));
        c.ret(0);
    });
}

}

void register_pthread(Hle& h)
{
    register_threads(h);
    register_mutexes(h);
    register_semaphores(h);
    register_sleep(h);
}

}
