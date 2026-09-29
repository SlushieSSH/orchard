#include "frameworks/libSystem/dispatch.h"

#include <chrono>
#include <deque>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/mach_time.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard
{
namespace
{
using clock = std::chrono::steady_clock;
constexpr uint64_t DISPATCH_TIME_FOREVER = ~uint64_t(0);
constexpr uint64_t KERN_OPERATION_TIMED_OUT = 49;
constexpr int kMaxWorkers = 48;

struct Work
{
    GuestAddr fn = 0, ctx = 0;
    GuestAddr release_block = 0;
    GuestAddr group = 0;
    std::shared_ptr<std::atomic<bool>> done;
};

struct Queue
{
    GuestAddr obj = 0;
    std::string label;
    GuestAddr label_str = 0;
    bool serial = true;
    bool main = false;
    std::deque<Work> pending;
    bool busy = false;
    int suspended = 0;
    std::unordered_map<GuestAddr, GuestAddr> specific;
};

struct Group
{
    int count = 0;
    std::vector<std::pair<Queue*, Work>> notify;
};

struct Sema
{
    std::mutex m;
    std::condition_variable_any cv;
    int64_t count = 0;
};

struct Source
{
    Queue* queue = nullptr;
    bool timer = false;
    GuestAddr handler = 0, cancel_handler = 0;
    uint64_t start = 0, interval_ns = 0;
    int suspended = 1;
    bool cancelled = false;
    uint64_t generation = 0;
};

struct Timer
{
    Queue* queue;
    Work work;
    GuestAddr source = 0;
    uint64_t generation = 0;
};

struct State
{
    std::recursive_mutex m;
    std::condition_variable_any work_cv, done_cv, timer_cv;
    std::deque<std::pair<Queue*, Work>> ready;
    std::unordered_map<GuestAddr, std::unique_ptr<Queue>> queues;
    std::unordered_map<GuestAddr, Group> groups;
    std::unordered_map<GuestAddr, std::shared_ptr<Sema>> semas;
    std::unordered_map<GuestAddr, Source> sources;
    std::multimap<clock::time_point, Timer> timers;
    std::unordered_set<GuestAddr> cancelled_blocks;
    Queue* main = nullptr;
    Queue* global = nullptr;
    int workers = 0, idle = 0;
    bool timer_thread = false;
    Runtime* rt = nullptr;
};

State& st()
{
    static State s;
    return s;
}

thread_local Queue* current_queue = nullptr;

Queue* make_queue(Runtime& rt, GuestAddr obj, std::string label, bool serial)
{
    auto q = std::make_unique<Queue>();
    q->obj = obj;
    q->label = std::move(label);
    q->label_str = rt.mem.alloc_cstr_region(q->label);
    q->serial = serial;
    Queue* raw = q.get();
    std::lock_guard g(st().m);
    st().queues[obj] = std::move(q);
    return raw;
}

Queue* queue_of(GuestAddr obj)
{
    std::lock_guard g(st().m);
    auto it = st().queues.find(obj);
    return it == st().queues.end() ? nullptr : it->second.get();
}

GuestAddr new_object(Cpu& c, const char* cls)
{
    return c.rt.objc->alloc_instance(c.rt.objc->host_class(cls));
}

void enqueue(Queue* q, Work w);

void leave_group(GuestAddr group)
{
    std::vector<std::pair<Queue*, Work>> notify;
    {
        std::lock_guard g(st().m);
        Group& grp = st().groups[group];
        if (--grp.count == 0)
        {
            notify.swap(grp.notify);
            st().done_cv.notify_all();
        }
    }
    for (auto& [q, w] : notify)
        enqueue(q, w);
}

void run_work(Cpu& c, Queue* q, Work& w)
{
    Queue* prev = current_queue;
    current_queue = q;
    auto pool = c.rt.objc->autorelease_push(c);
    c.call(w.fn, {w.ctx});
    c.rt.objc->autorelease_pop(c, pool);
    current_queue = prev;
    if (w.release_block) block_release(c, w.release_block);
    if (w.group) leave_group(w.group);
    if (w.done)
    {
        std::lock_guard g(st().m);
        *w.done = true;
        st().done_cv.notify_all();
    }
}

void schedule_next(Queue* q);

void worker_loop(Cpu& c)
{
    State& s = st();
    for (;;)
    {
        std::pair<Queue*, Work> task;
        {
            std::unique_lock l(s.m);
            ++s.idle;
            bool got = c.rt.wait(s.work_cv, l, [&] { return !s.ready.empty(); });
            --s.idle;
            if (!got) return;
            task = std::move(s.ready.front());
            s.ready.pop_front();
        }
        run_work(c, task.first, task.second);
        if (c.stopped()) return;
        if (task.first && task.first->serial)
        {
            std::lock_guard g(s.m);
            schedule_next(task.first);
        }
    }
}

void ensure_worker(Runtime& rt)
{
    State& s = st();
    if (s.idle > 0 || s.workers >= kMaxWorkers) return;
    ++s.workers;
    rt.threads->spawn_host("dispatch-worker-" + std::to_string(s.workers), worker_loop);
}

void push_ready(Queue* q, Work w)
{
    st().ready.emplace_back(q, std::move(w));
    st().work_cv.notify_one();
    ensure_worker(*st().rt);
}

void schedule_next(Queue* q)
{
    if (q->pending.empty() || q->suspended)
    {
        q->busy = false;
        return;
    }
    q->busy = true;
    Work w = std::move(q->pending.front());
    q->pending.pop_front();
    push_ready(q, std::move(w));
}

void enqueue(Queue* q, Work w)
{
    State& s = st();
    std::lock_guard g(s.m);
    if (!q) q = s.global;
    if (q->main)
    {
        q->pending.push_back(std::move(w));
        return;
    }
    if (q->serial)
    {
        q->pending.push_back(std::move(w));
        if (!q->busy) schedule_next(q);
        return;
    }
    push_ready(q, std::move(w));
}

Work block_work(Cpu& c, GuestAddr block, GuestAddr group = 0)
{
    GuestAddr copy = block_copy(c, block);
    return {c.mem.read<uint64_t>(copy + 16), copy, copy, group, nullptr};
}

clock::time_point deadline_of(uint64_t t)
{
    if (t == DISPATCH_TIME_FOREVER) return clock::time_point::max();
    if (t == 0) return clock::now();
    if (int64_t(t) < 0)
    {
        auto wall = std::chrono::nanoseconds(-int64_t(t));
        auto now_wall = std::chrono::system_clock::now().time_since_epoch();
        return clock::now() + std::chrono::duration_cast<clock::duration>(wall - now_wall);
    }
    return mach_to_steady(t);
}

void timer_loop()
{
    State& s = st();
    std::unique_lock l(s.m);
    while (!s.rt->halted())
    {
        if (s.timers.empty())
        {
            s.timer_cv.wait_for(l, std::chrono::milliseconds(50));
            continue;
        }
        auto first = s.timers.begin();
        if (first->first > clock::now())
        {
            s.timer_cv.wait_until(l, std::min(first->first, clock::now() + std::chrono::milliseconds(50)));
            continue;
        }
        Timer t = first->second;
        s.timers.erase(first);
        if (t.source)
        {
            auto it = s.sources.find(t.source);
            if (it == s.sources.end()) continue;
            Source& src = it->second;
            if (src.cancelled || src.suspended || t.generation != src.generation || !src.handler) continue;
            Work w{s.rt->mem.read<uint64_t>(src.handler + 16), src.handler};
            enqueue(src.queue, w);
            if (src.interval_ns && src.interval_ns != DISPATCH_TIME_FOREVER)
                s.timers.emplace(clock::now() + std::chrono::nanoseconds(src.interval_ns), t);
        }
        else
        {
            enqueue(t.queue, t.work);
        }
    }
}

void add_timer(clock::time_point when, Timer t)
{
    State& s = st();
    std::lock_guard g(s.m);
    if (!s.timer_thread)
    {
        s.timer_thread = true;
        std::thread(timer_loop).detach();
    }
    s.timers.emplace(when, std::move(t));
    s.timer_cv.notify_all();
}

void arm_source(GuestAddr obj)
{
    State& s = st();
    std::lock_guard g(s.m);
    Source& src = s.sources[obj];
    if (!src.timer || src.suspended || src.cancelled || !src.handler) return;
    ++src.generation;
    add_timer(deadline_of(src.start), {src.queue, {}, obj, src.generation});
}

bool wait_done(Cpu& c, const std::shared_ptr<std::atomic<bool>>& done)
{
    std::unique_lock l(st().m);
    return c.rt.wait(st().done_cv, l, [&] { return done->load(); });
}

void sync_on(Cpu& c, Queue* q, Work w)
{
    State& s = st();
    if (!q) q = s.global;
    if (q->main && !c.is_main)
    {
        w.done = std::make_shared<std::atomic<bool>>(false);
        auto done = w.done;
        enqueue(q, std::move(w));
        if (!wait_done(c, done)) c.exit_thread();
        return;
    }
    if (q->serial && !q->main)
    {
        std::unique_lock l(s.m);
        if (!c.rt.wait(s.done_cv, l, [&] { return !q->busy && q->pending.empty(); })) return c.exit_thread();
        q->busy = true;
        l.unlock();
        run_work(c, q, w);
        l.lock();
        q->busy = false;
        schedule_next(q);
        s.done_cv.notify_all();
        return;
    }
    run_work(c, q, w);
}

void register_queues(Hle& h)
{
    h.data("__dispatch_main_q", [](Runtime& rt) {
        GuestAddr obj = rt.mem.alloc_system(128, 16);
        rt.mem.write<uint64_t>(obj, rt.objc->host_class("OS_dispatch_queue_main")->addr);
        st().rt = &rt;
        st().main = make_queue(rt, obj, "com.apple.main-thread", true);
        st().main->main = true;
        return obj;
    });
    h.data("__dispatch_queue_attr_concurrent", [](Runtime& rt) { return rt.mem.alloc_system(16, 16); });
    h.data("__dispatch_source_type_timer", [](Runtime& rt) { return rt.mem.alloc_system(16, 16); });

    h.fn("_dispatch_get_global_queue", [](Cpu& c) { c.ret(st().global->obj); });
    auto queue_create = [](Cpu& c) {
        static GuestAddr concurrent = c.rt.hle.resolve("__dispatch_queue_attr_concurrent", "libdispatch");
        bool serial = c.arg(1) != concurrent;
        GuestAddr obj = new_object(c, serial ? "OS_dispatch_queue_serial" : "OS_dispatch_queue_concurrent");
        make_queue(c.rt, obj, c.arg(0) ? c.mem.read_cstr(c.arg(0)) : "", serial);
        c.ret(obj);
    };
    h.fn("_dispatch_queue_create", queue_create);
    h.fn("_dispatch_queue_create_with_target$V2", queue_create);
    h.fn("_dispatch_queue_create_with_target", queue_create);
    h.fn("_dispatch_queue_attr_make_with_qos_class", [](Cpu& c) {
        static GuestAddr concurrent = c.rt.hle.resolve("__dispatch_queue_attr_concurrent", "libdispatch");
        c.ret(c.arg(0) == concurrent ? concurrent : 0);
    });
    h.fn("_dispatch_queue_get_label", [](Cpu& c) {
        Queue* q = c.arg(0) ? queue_of(c.arg(0)) : current_queue;
        if (!q && c.is_main) q = st().main;
        c.ret(q ? q->label_str : c.mem.alloc_cstr_region(""));
    });
    h.fn("_dispatch_queue_set_specific", [](Cpu& c) {
        if (Queue* q = queue_of(c.arg(0)))
        {
            std::lock_guard g(st().m);
            q->specific[c.arg(1)] = c.arg(2);
        }
    });
    h.fn("_dispatch_get_specific", [](Cpu& c) {
        Queue* q = current_queue;
        if (!q && c.is_main) q = st().main;
        std::lock_guard g(st().m);
        if (q)
            if (auto it = q->specific.find(c.arg(0)); it != q->specific.end()) return c.ret(it->second);
        c.ret(0);
    });
    h.fn("_dispatch_set_target_queue", [](Cpu& c) {});
    h.fn("_dispatch_assert_queue$V2", [](Cpu& c) {});

    h.fn("_dispatch_async", [](Cpu& c) { enqueue(queue_of(c.arg(0)), block_work(c, c.arg(1))); });
    h.fn("_dispatch_barrier_async", [](Cpu& c) { enqueue(queue_of(c.arg(0)), block_work(c, c.arg(1))); });
    h.fn("_dispatch_async_f", [](Cpu& c) { enqueue(queue_of(c.arg(0)), {c.arg(2), c.arg(1)}); });
    h.fn("_dispatch_sync", [](Cpu& c) {
        GuestAddr blk = c.arg(1);
        sync_on(c, queue_of(c.arg(0)), {c.mem.read<uint64_t>(blk + 16), blk});
    });
    h.fn("_dispatch_barrier_sync", [](Cpu& c) {
        GuestAddr blk = c.arg(1);
        sync_on(c, queue_of(c.arg(0)), {c.mem.read<uint64_t>(blk + 16), blk});
    });
    h.fn("_dispatch_after", [](Cpu& c) { add_timer(deadline_of(c.arg(0)), {queue_of(c.arg(1)), block_work(c, c.arg(2))}); });

    h.fn("_dispatch_once", [](Cpu& c) {
        GuestAddr pred = c.arg(0);
        if (c.mem.read<int64_t>(pred) == -1) return;
        static std::recursive_mutex once_lock;
        std::lock_guard g(once_lock);
        if (c.mem.read<int64_t>(pred) == -1) return;
        call_block(c, c.arg(1));
        c.mem.write<int64_t>(pred, -1);
    });
    h.fn("_dispatch_once_f", [](Cpu& c) {
        GuestAddr pred = c.arg(0);
        if (c.mem.read<int64_t>(pred) == -1) return;
        static std::recursive_mutex once_lock;
        std::lock_guard g(once_lock);
        if (c.mem.read<int64_t>(pred) == -1) return;
        c.call(c.arg(2), {c.arg(1)});
        c.mem.write<int64_t>(pred, -1);
    });

    h.fn("_dispatch_time", [](Cpu& c) {
        uint64_t when = c.arg(0);
        int64_t delta = int64_t(c.arg(1));
        if (when == DISPATCH_TIME_FOREVER) return c.ret(when);
        if (int64_t(when) < 0) return c.ret(uint64_t(int64_t(when) - delta));
        uint64_t base = when ? when : mach_now();
        c.ret(base + uint64_t(delta / 125 * 3));
    });
    h.fn("_dispatch_walltime", [](Cpu& c) {
        int64_t ns;
        if (c.arg(0))
            ns = c.mem.read<int64_t>(c.arg(0)) * 1'000'000'000 + c.mem.read<int64_t>(c.arg(0) + 8);
        else
            ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        c.ret(uint64_t(-(ns + int64_t(c.arg(1)))));
    });

    h.fn("_dispatch_retain", [](Cpu& c) { c.rt.objc->retain(c.arg(0)); });
    h.fn("_dispatch_release", [](Cpu& c) { c.rt.objc->release(c, c.arg(0)); });
    h.fn("_os_release", [](Cpu& c) { c.rt.objc->release(c, c.arg(0)); });
    h.fn("_voucher_adopt", [](Cpu& c) { c.ret(0); });
}

std::shared_ptr<Sema> semaphore(GuestAddr obj)
{
    std::lock_guard g(st().m);
    auto& p = st().semas[obj];
    if (!p) p = std::make_shared<Sema>();
    return p;
}

void register_sync(Hle& h)
{
    h.fn("_dispatch_semaphore_create", [](Cpu& c) {
        if (int64_t(c.arg(0)) < 0) return c.ret(0);
        GuestAddr obj = new_object(c, "OS_dispatch_semaphore");
        semaphore(obj)->count = int64_t(c.arg(0));
        c.ret(obj);
    });
    h.fn("_dispatch_semaphore_wait", [](Cpu& c) {
        auto sem = semaphore(c.arg(0));
        std::unique_lock l(sem->m);
        bool ok = c.rt.wait(sem->cv, l, [&] { return sem->count > 0; }, deadline_of(c.arg(1)));
        if (!ok && c.rt.halted()) return c.exit_thread();
        if (ok) --sem->count;
        if (sem->count > 0) sem->cv.notify_one();
        c.ret(ok ? 0 : KERN_OPERATION_TIMED_OUT);
    });
    h.fn("_dispatch_semaphore_signal", [](Cpu& c) {
        auto sem = semaphore(c.arg(0));
        {
            std::lock_guard g(sem->m);
            ++sem->count;
        }
        sem->cv.notify_one();
        c.ret(0);
    });

    h.fn("_dispatch_group_create", [](Cpu& c) {
        GuestAddr obj = new_object(c, "OS_dispatch_group");
        std::lock_guard g(st().m);
        st().groups[obj];
        c.ret(obj);
    });
    h.fn("_dispatch_group_enter", [](Cpu& c) {
        std::lock_guard g(st().m);
        ++st().groups[c.arg(0)].count;
    });
    h.fn("_dispatch_group_leave", [](Cpu& c) { leave_group(c.arg(0)); });
    h.fn("_dispatch_group_async", [](Cpu& c) {
        {
            std::lock_guard g(st().m);
            ++st().groups[c.arg(0)].count;
        }
        enqueue(queue_of(c.arg(1)), block_work(c, c.arg(2), c.arg(0)));
    });
    h.fn("_dispatch_group_notify", [](Cpu& c) {
        Work w = block_work(c, c.arg(2));
        Queue* q = queue_of(c.arg(1));
        {
            std::lock_guard g(st().m);
            Group& grp = st().groups[c.arg(0)];
            if (grp.count > 0)
            {
                grp.notify.emplace_back(q, w);
                return;
            }
        }
        enqueue(q, w);
    });
    h.fn("_dispatch_group_wait", [](Cpu& c) {
        State& s = st();
        std::unique_lock l(s.m);
        Group& grp = s.groups[c.arg(0)];
        bool ok = c.rt.wait(s.done_cv, l, [&] { return grp.count <= 0; }, deadline_of(c.arg(1)));
        if (!ok && c.rt.halted()) return c.exit_thread();
        c.ret(ok ? 0 : KERN_OPERATION_TIMED_OUT);
    });
}

void register_sources(Hle& h)
{
    h.fn("_dispatch_source_create", [](Cpu& c) {
        static GuestAddr timer_type = c.rt.hle.resolve("__dispatch_source_type_timer", "libdispatch");
        GuestAddr obj = new_object(c, "OS_dispatch_source");
        std::lock_guard g(st().m);
        Source& src = st().sources[obj];
        src.timer = c.arg(0) == timer_type;
        src.queue = queue_of(c.arg(3));
        c.ret(obj);
    });
    h.fn("_dispatch_source_set_timer", [](Cpu& c) {
        {
            std::lock_guard g(st().m);
            Source& src = st().sources[c.arg(0)];
            src.start = c.arg(1);
            src.interval_ns = c.arg(2);
        }
        arm_source(c.arg(0));
    });
    h.fn("_dispatch_source_set_event_handler", [](Cpu& c) {
        GuestAddr blk = block_copy(c, c.arg(1));
        std::lock_guard g(st().m);
        st().sources[c.arg(0)].handler = blk;
    });
    h.fn("_dispatch_source_set_cancel_handler", [](Cpu& c) {
        GuestAddr blk = block_copy(c, c.arg(1));
        std::lock_guard g(st().m);
        st().sources[c.arg(0)].cancel_handler = blk;
    });
    h.fn("_dispatch_source_cancel", [](Cpu& c) {
        GuestAddr handler = 0;
        Queue* q = nullptr;
        {
            std::lock_guard g(st().m);
            Source& src = st().sources[c.arg(0)];
            if (src.cancelled) return;
            src.cancelled = true;
            handler = src.cancel_handler;
            q = src.queue;
        }
        if (handler) enqueue(q, {c.mem.read<uint64_t>(handler + 16), handler});
    });
    h.fn("_dispatch_source_testcancel", [](Cpu& c) {
        std::lock_guard g(st().m);
        c.ret(st().sources[c.arg(0)].cancelled);
    });

    auto resume = [](Cpu& c) {
        GuestAddr obj = c.arg(0);
        {
            std::lock_guard g(st().m);
            if (auto it = st().sources.find(obj); it != st().sources.end())
            {
                if (it->second.suspended > 0) --it->second.suspended;
            }
            else if (Queue* q = queue_of(obj))
            {
                if (q->suspended > 0 && --q->suspended == 0 && q->serial && !q->busy) schedule_next(q);
                return;
            }
        }
        arm_source(obj);
    };
    h.fn("_dispatch_resume", resume);
    h.fn("_dispatch_activate", resume);
    h.fn("_dispatch_suspend", [](Cpu& c) {
        std::lock_guard g(st().m);
        if (auto it = st().sources.find(c.arg(0)); it != st().sources.end())
            ++it->second.suspended;
        else if (Queue* q = queue_of(c.arg(0)))
            ++q->suspended;
    });

    h.fn("_dispatch_block_create", [](Cpu& c) { c.ret(block_copy(c, c.arg(1))); });
    h.fn("_dispatch_block_cancel", [](Cpu& c) {
        std::lock_guard g(st().m);
        st().cancelled_blocks.insert(c.arg(0));
    });
    h.fn("_dispatch_block_testcancel", [](Cpu& c) {
        std::lock_guard g(st().m);
        c.ret(st().cancelled_blocks.count(c.arg(0)));
    });
    h.fn("_dispatch_block_wait", [](Cpu& c) { c.ret(0); });
}

}

size_t drain_main_queue(Cpu& main)
{
    std::deque<Work> items;
    {
        std::lock_guard g(st().m);
        if (!st().main) return 0;
        items.swap(st().main->pending);
    }
    for (auto& w : items)
    {
        if (main.stopped()) break;
        run_work(main, st().main, w);
    }
    return items.size();
}

GuestAddr main_queue_object(Cpu& c)
{
    return c.rt.hle.resolve("__dispatch_main_q", "libdispatch");
}

GuestAddr global_queue_object()
{
    return st().global->obj;
}

void dispatch_block_async(Cpu& c, GuestAddr queue, GuestAddr block)
{
    enqueue(queue_of(queue), block_work(c, block));
}

void dispatch_function_async(GuestAddr queue, GuestAddr fn, GuestAddr ctx)
{
    enqueue(queue ? queue_of(queue) : nullptr, {fn, ctx});
}

GuestAddr create_serial_queue(Cpu& c, const char* label)
{
    GuestAddr obj = new_object(c, "OS_dispatch_queue_serial");
    make_queue(c.rt, obj, label, true);
    return obj;
}

bool main_queue_has_work()
{
    std::lock_guard g(st().m);
    return st().main && !st().main->pending.empty();
}

void register_dispatch_classes(objc::ObjcRuntime& o)
{
    o.define("OS_object", "NSObject");
    o.define("OS_dispatch_object", "OS_object");
    o.define("OS_dispatch_queue", "OS_dispatch_object");
    for (const char* k : {"OS_dispatch_queue_serial", "OS_dispatch_queue_concurrent", "OS_dispatch_queue_main", "OS_dispatch_queue_global"})
        o.define(k, "OS_dispatch_queue");
    for (const char* k : {"OS_dispatch_semaphore", "OS_dispatch_group", "OS_dispatch_source", "OS_dispatch_data"})
        o.define(k, "OS_dispatch_object");
    o.define("OS_os_log", "OS_object");

    Runtime& rt = o.rt;
    GuestAddr global = rt.mem.alloc_system(128, 16);
    rt.mem.write<uint64_t>(global, o.host_class("OS_dispatch_queue_global")->addr);
    st().rt = &rt;
    st().global = make_queue(rt, global, "com.apple.root.default-qos", false);
}

void register_dispatch(Hle& h)
{
    register_queues(h);
    register_sync(h);
    register_sources(h);
}

}
