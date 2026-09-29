#include <algorithm>
#include <chrono>
#include <thread>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/UIKit/uikit.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "host/window.h"
#include "objc/runtime.h"

namespace orchard::uikit
{
using objc::objc;
using objc::SEL;

namespace
{
using clock = std::chrono::steady_clock;
double now_seconds()
{
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

struct Timer
{
    double fire_at = 0, interval = 0;
    bool repeats = false, scheduled = false, valid = true;
    Id target = 0, user_info = 0;
    SEL sel = 0;
    GuestAddr block = 0;
};

struct Link
{
    Id target = 0;
    SEL sel = 0;
    bool added = false, paused = false;
    double timestamp = 0, target_timestamp = 0;
};

struct DelayedPerform
{
    Id target = 0, arg = 0;
    SEL sel = 0;
    double fire_at = 0;
    std::shared_ptr<std::atomic<bool>> done;
};

std::mutex state_lock;
std::unordered_map<Id, Timer> timers;
std::vector<DelayedPerform> performs;
std::unordered_map<Id, Link> links;
double next_frame = 0;
constexpr double kFrame = 1.0 / 60;

Id new_timer(Cpu& c, double interval, bool repeats, Id target, SEL sel, Id info, GuestAddr block)
{
    Id t = objc(c).alloc_instance(objc(c).host_class("__NSCFTimer"));
    Timer tm;
    tm.interval = std::max(interval, 0.0001);
    tm.fire_at = now_seconds() + interval;
    tm.repeats = repeats;
    tm.target = objc(c).retain(target);
    tm.sel = sel;
    tm.user_info = objc(c).retain(info);
    tm.block = block ? block_copy(c, block) : 0;
    std::lock_guard g(state_lock);
    timers[t] = tm;
    return t;
}

void schedule(Cpu& c, Id t)
{
    std::lock_guard g(state_lock);
    auto it = timers.find(t);
    if (it == timers.end() || it->second.scheduled) return;
    it->second.scheduled = true;
    objc(c).retain(t);
}

void fire_timers(Cpu& c)
{
    double now = now_seconds();
    std::vector<std::pair<Id, Timer>> due;
    {
        std::lock_guard g(state_lock);
        for (auto& [t, tm] : timers)
            if (tm.scheduled && tm.valid && tm.fire_at <= now) due.emplace_back(t, tm);
    }
    for (auto& [t, tm] : due)
    {
        if (c.stopped()) return;
        if (tm.block)
            call_block(c, tm.block, {t});
        else if (tm.target)
            objc(c).send(c, tm.target, tm.sel, {t});
        std::lock_guard g(state_lock);
        auto it = timers.find(t);
        if (it == timers.end()) continue;
        if (it->second.repeats && it->second.valid)
            it->second.fire_at = std::max(it->second.fire_at + it->second.interval, now);
        else
            it->second.valid = false;
    }
}

double next_timer_due()
{
    std::lock_guard g(state_lock);
    double t = 1e300;
    for (auto& [id, tm] : timers)
        if (tm.scheduled && tm.valid) t = std::min(t, tm.fire_at);
    return t;
}

void tick_display_links(Cpu& c, double now)
{
    std::vector<std::pair<Id, Link>> active;
    {
        std::lock_guard g(state_lock);
        for (auto& [id, l] : links)
        {
            if (!l.added || l.paused) continue;
            l.timestamp = now;
            l.target_timestamp = now + kFrame;
            active.emplace_back(id, l);
        }
    }
    for (auto& [id, l] : active)
    {
        if (c.stopped()) return;
        objc(c).send(c, l.target, l.sel, {id});
    }
}

void deliver_input(Cpu& c);

void run_performs(Cpu& c)
{
    double now = now_seconds();
    std::vector<DelayedPerform> due;
    {
        std::lock_guard g(state_lock);
        for (auto it = performs.begin(); it != performs.end();)
        {
            if (it->fire_at <= now)
            {
                due.push_back(*it);
                it = performs.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for (auto& p : due)
    {
        if (c.stopped()) return;
        objc(c).send(c, p.target, p.sel, {p.arg});
        if (p.done) *p.done = true;
        objc(c).release(c, p.target);
        objc(c).release(c, p.arg);
    }
}

void add_perform(Cpu& c, Id target, SEL sel, Id arg, double delay, std::shared_ptr<std::atomic<bool>> done = nullptr)
{
    std::lock_guard g(state_lock);
    performs.push_back({objc(c).retain(target), objc(c).retain(arg), sel, now_seconds() + delay, std::move(done)});
}

void register_performs(objc::ObjcRuntime& o)
{
    o.method("NSObject", "performSelector:withObject:afterDelay:", [](Cpu& c) { add_perform(c, c.arg(0), c.arg(2), c.arg(3), c.d(0)); });
    o.method("NSObject",
             "performSelector:withObject:afterDelay:inModes:", [](Cpu& c) { add_perform(c, c.arg(0), c.arg(2), c.arg(3), c.d(0)); });
    o.method("NSObject", "performSelectorOnMainThread:withObject:waitUntilDone:", [](Cpu& c) {
        if (c.is_main && (c.arg(4) & 1)) return (void)objc(c).send(c, c.arg(0), c.arg(2), {c.arg(3)});
        auto done = std::make_shared<std::atomic<bool>>(false);
        add_perform(c, c.arg(0), c.arg(2), c.arg(3), 0, done);
        if (!(c.arg(4) & 1)) return;
        while (!*done && !c.rt.halted())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    o.method("NSObject", "performSelectorInBackground:withObject:", [](Cpu& c) {
        objc(c).send(c, objc(c).host_class("NSThread")->addr,
                     "detachNewThreadSelector:toTarget:withObject:", {c.arg(2), c.arg(0), c.arg(3)});
    });
    auto cancel = [](Cpu& c, Id target, SEL sel, bool any_sel) {
        std::vector<DelayedPerform> removed;
        {
            std::lock_guard g(state_lock);
            for (auto it = performs.begin(); it != performs.end();)
            {
                if (it->target == target && (any_sel || it->sel == sel))
                {
                    removed.push_back(*it);
                    it = performs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        for (auto& p : removed)
        {
            objc(c).release(c, p.target);
            objc(c).release(c, p.arg);
        }
    };
    static decltype(cancel) s_cancel = cancel;
    o.class_method("NSObject", "cancelPreviousPerformRequestsWithTarget:", [](Cpu& c) { s_cancel(c, c.arg(2), 0, true); });
    o.class_method("NSObject",
                   "cancelPreviousPerformRequestsWithTarget:selector:object:", [](Cpu& c) { s_cancel(c, c.arg(2), c.arg(3), false); });
}

void register_timers(objc::ObjcRuntime& o)
{
    o.define("__NSCFTimer", "NSTimer");
    o.class_method("NSTimer", "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", [](Cpu& c) {
        Id t = new_timer(c, c.d(0), c.arg(5) & 1, c.arg(2), c.arg(3), c.arg(4), 0);
        schedule(c, t);
        c.ret(objc(c).autorelease(c, t));
    });
    o.class_method("NSTimer", "scheduledTimerWithTimeInterval:repeats:block:", [](Cpu& c) {
        Id t = new_timer(c, c.d(0), c.arg(2) & 1, 0, 0, 0, c.arg(3));
        schedule(c, t);
        c.ret(objc(c).autorelease(c, t));
    });
    o.class_method("NSTimer", "timerWithTimeInterval:target:selector:userInfo:repeats:", [](Cpu& c) {
        c.ret(objc(c).autorelease(c, new_timer(c, c.d(0), c.arg(5) & 1, c.arg(2), c.arg(3), c.arg(4), 0)));
    });
    o.class_method("NSTimer", "timerWithTimeInterval:repeats:block:", [](Cpu& c) {
        c.ret(objc(c).autorelease(c, new_timer(c, c.d(0), c.arg(2) & 1, 0, 0, 0, c.arg(3))));
    });
    o.method("NSTimer", "initWithFireDate:interval:target:selector:userInfo:repeats:", [](Cpu& c) {
        Id t = new_timer(c, c.d(0), c.arg(6) & 1, c.arg(3), c.arg(4), c.arg(5), 0);
        double secs;
        if (foundation::date_seconds(c, c.arg(2), secs))
        {
            std::lock_guard g(state_lock);
            timers[t].fire_at =
                now_seconds() +
                (secs - (std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - 978307200.0));
        }
        objc(c).dispose(c.arg(0));
        c.ret(t);
    });
    o.method("NSTimer", "invalidate", [](Cpu& c) {
        bool release = false;
        {
            std::lock_guard g(state_lock);
            auto it = timers.find(c.arg(0));
            if (it == timers.end() || !it->second.valid) return;
            it->second.valid = false;
            release = it->second.scheduled;
            it->second.scheduled = false;
        }
        if (release) objc(c).release(c, c.arg(0));
    });
    o.method("NSTimer", "isValid", [](Cpu& c) {
        std::lock_guard g(state_lock);
        auto it = timers.find(c.arg(0));
        c.ret(it != timers.end() && it->second.valid);
    });
    o.method("NSTimer", "fire", [](Cpu& c) {
        Timer tm;
        {
            std::lock_guard g(state_lock);
            tm = timers[c.arg(0)];
        }
        if (tm.block)
            call_block(c, tm.block, {c.arg(0)});
        else if (tm.target)
            objc(c).send(c, tm.target, tm.sel, {c.arg(0)});
    });
    o.method("NSTimer", "userInfo", [](Cpu& c) {
        std::lock_guard g(state_lock);
        c.ret(timers[c.arg(0)].user_info);
    });
    o.method("NSTimer", "timeInterval", [](Cpu& c) {
        std::lock_guard g(state_lock);
        c.set_d(0, timers[c.arg(0)].interval);
    });
    o.method("NSTimer", "setTolerance:", [](Cpu& c) {});
    o.method("NSTimer", "tolerance", [](Cpu& c) { c.set_d(0, 0); });
}

void register_display_links(objc::ObjcRuntime& o)
{
    o.class_method("CADisplayLink", "displayLinkWithTarget:selector:", [](Cpu& c) {
        Id l = objc(c).alloc_instance(objc(c).host_class("CADisplayLink"));
        std::lock_guard g(state_lock);
        links[l] = {objc(c).retain(c.arg(2)), c.arg(3)};
        c.ret(objc(c).autorelease(c, l));
    });
    o.method("CADisplayLink", "addToRunLoop:forMode:", [](Cpu& c) {
        {
            std::lock_guard g(state_lock);
            if (links[c.arg(0)].added) return;
            links[c.arg(0)].added = true;
        }
        objc(c).retain(c.arg(0));
        std::printf("[UIKit] display link added (target %s)\n", objc(c).class_of(links[c.arg(0)].target)->name.c_str());
    });
    auto remove = [](Cpu& c) {
        {
            std::lock_guard g(state_lock);
            auto it = links.find(c.arg(0));
            if (it == links.end() || !it->second.added) return;
            it->second.added = false;
        }
        objc(c).release(c, c.arg(0));
    };
    o.method("CADisplayLink", "removeFromRunLoop:forMode:", remove);
    o.method("CADisplayLink", "invalidate", remove);
    o.method("CADisplayLink", "isPaused", [](Cpu& c) {
        std::lock_guard g(state_lock);
        c.ret(links[c.arg(0)].paused);
    });
    o.method("CADisplayLink", "setPaused:", [](Cpu& c) {
        std::lock_guard g(state_lock);
        links[c.arg(0)].paused = c.arg(2) & 1;
    });
    o.method("CADisplayLink", "timestamp", [](Cpu& c) {
        std::lock_guard g(state_lock);
        c.set_d(0, links[c.arg(0)].timestamp);
    });
    o.method("CADisplayLink", "targetTimestamp", [](Cpu& c) {
        std::lock_guard g(state_lock);
        c.set_d(0, links[c.arg(0)].target_timestamp);
    });
    o.method("CADisplayLink", "duration", [](Cpu& c) { c.set_d(0, kFrame); });
    o.method("CADisplayLink", "preferredFramesPerSecond", [](Cpu& c) { c.ret(60); });
    o.method("CADisplayLink", "frameInterval", [](Cpu& c) { c.ret(1); });
    for (const char* sel : {"setPreferredFramesPerSecond:", "setFrameInterval:", "setPreferredFrameRateRange:"})
        o.method("CADisplayLink", sel, [](Cpu& c) {});
}

Id main_loop_object(Cpu& c)
{
    static Id loop = objc(c).alloc_instance(objc(c).host_class("NSRunLoop"));
    return loop;
}

void park_thread(Cpu& c)
{
    while (!c.rt.halted())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.exit_thread();
}

void run_until(Cpu& c, double seconds)
{
    double end = now_seconds() + seconds;
    do
    {
        if (!run_loop_turn(c, std::max(0.0, std::min(end - now_seconds(), 1.0 / 120))))
        {
            std::fflush(nullptr);
            std::_Exit(0);
        }
    } while (!c.stopped() && now_seconds() < end);
}

void register_run_loops(objc::ObjcRuntime& o)
{
    o.class_method("NSRunLoop", "mainRunLoop", [](Cpu& c) { c.ret(main_loop_object(c)); });
    o.class_method("NSRunLoop", "currentRunLoop", [](Cpu& c) { c.ret(main_loop_object(c)); });
    o.method("NSRunLoop", "addTimer:forMode:", [](Cpu& c) { schedule(c, c.arg(2)); });
    o.method("NSRunLoop", "currentMode", [](Cpu& c) { c.ret(foundation::string_autoreleased(c, "kCFRunLoopDefaultMode")); });
    o.method("NSRunLoop", "getCFRunLoop", [](Cpu& c) {});
    o.method("NSRunLoop", "run", [](Cpu& c) {
        if (!c.is_main) return park_thread(c);
        while (!c.stopped())
            run_until(c, 1);
    });
    o.method("NSRunLoop", "runUntilDate:", [](Cpu& c) {
        if (!c.is_main) return park_thread(c);
        double secs;
        foundation::date_seconds(c, c.arg(2), secs);
        double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - 978307200.0;
        run_until(c, std::min(secs - wall, 1.0));
    });
    o.method("NSRunLoop", "runMode:beforeDate:", [](Cpu& c) {
        if (!c.is_main)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return c.ret(1);
        }
        run_until(c, 0);
        c.ret(1);
    });
    o.method("NSRunLoop", "performBlock:", [](Cpu& c) { dispatch_block_async(c, main_queue_object(c), c.arg(2)); });

    Hle& h = o.rt.hle;
    h.fn("_CFRunLoopGetMain", [](Cpu& c) { c.ret(main_loop_object(c)); });
    h.fn("_CFRunLoopGetCurrent", [](Cpu& c) { c.ret(main_loop_object(c)); });
    h.fn("_CFRunLoopRunInMode", [](Cpu& c) {
        if (!c.is_main)
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(c.d(0), 0.05)));
            return c.ret(3);
        }
        run_until(c, c.d(0));
        c.ret(3);
    });
    h.fn("_CFRunLoopRun", [](Cpu& c) {
        if (!c.is_main) return park_thread(c);
        while (!c.stopped())
            run_until(c, 1);
    });
    for (const char* n : {"_CFRunLoopStop", "_CFRunLoopWakeUp", "_CFRunLoopAddObserver", "_CFRunLoopRemoveObserver", "_CFRunLoopAddSource",
                          "_CFRunLoopRemoveSource"})
        h.fn(n, [](Cpu& c) {});
    h.fn("_CFRunLoopAddTimer", [](Cpu& c) { schedule(c, c.arg(1)); });
    auto mode = [](Runtime& rt, const char16_t* value) {
        GuestAddr var = rt.mem.alloc_system(8, 8);
        rt.mem.write<uint64_t>(var, foundation::new_string(rt, value));
        return var;
    };
    static decltype(mode) s_mode = mode;
    h.data("_kCFRunLoopDefaultMode", [](Runtime& rt) { return s_mode(rt, u"kCFRunLoopDefaultMode"); });
    h.data("_NSDefaultRunLoopMode", [](Runtime& rt) { return s_mode(rt, u"kCFRunLoopDefaultMode"); });
    h.data("_kCFRunLoopCommonModes", [](Runtime& rt) { return s_mode(rt, u"kCFRunLoopCommonModes"); });
    h.data("_NSRunLoopCommonModes", [](Runtime& rt) { return s_mode(rt, u"kCFRunLoopCommonModes"); });
    h.data("_UITrackingRunLoopMode", [](Runtime& rt) { return s_mode(rt, u"UITrackingRunLoopMode"); });
}

}

bool run_loop_turn(Cpu& c, double max_wait)
{
    App& a = app();
    if (a.window && !a.window->pump()) return false;
    if (!a.started) a.started = now_seconds();
    if (a.quit_after > 0 && now_seconds() - a.started > a.quit_after)
    {
        if (a.window && !a.screenshot.empty())
            std::printf("[UIKit] screenshot %s: %s\n", a.screenshot.c_str(), a.window->save_png(a.screenshot) ? "saved" : "failed");
        return false;
    }
    drain_main_queue(c);
    run_performs(c);
    fire_timers(c);
    if (c.stopped()) return true;
    deliver_input(c);
    layout_pass(c);
    double now = now_seconds();
    if (now >= next_frame)
    {
        next_frame = std::max(next_frame + kFrame, now);
        tick_display_links(c, now);
    }
    if (main_queue_has_work()) return true;
    double wake = std::min({next_frame, next_timer_due(), now + max_wait});
    if (wake > now) std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wake - now, 0.004)));
    return true;
}

void register_runloop(objc::ObjcRuntime& o)
{
    o.define("CADisplayLink", "NSObject");
    o.define("NSRunLoop", "NSObject");
    register_timers(o);
    register_performs(o);
    register_display_links(o);
    register_run_loops(o);
}

namespace
{
void deliver_input(Cpu& c)
{
    if (!app().window) return;
    auto events = app().window->take_events();
    auto& script = app().script;
    double elapsed = now_seconds() - app().started;
    std::vector<std::pair<double, std::vector<float>>> later;
    while (!script.empty() && script.front().first <= elapsed)
    {
        std::vector<float> p = std::move(script.front().second);
        script.erase(script.begin());
        if (p.size() == 2)
        {
            events.push_back({PointerEvent::Kind::Down, p[0], p[1]});
            later.push_back({elapsed + 0.08, {-1, p[0], p[1]}});
        }
        else if (p.size() == 4)
        {
            events.push_back({PointerEvent::Kind::Down, p[0], p[1]});
            for (int i = 1; i <= 6; ++i)
                later.push_back({elapsed + 0.025 * i, {-2, p[0] + (p[2] - p[0]) * i / 6, p[1] + (p[3] - p[1]) * i / 6}});
            later.push_back({elapsed + 0.2, {-1, p[2], p[3]}});
        }
        else
        {
            events.push_back({p[0] == -1 ? PointerEvent::Kind::Up : PointerEvent::Kind::Move, p[1], p[2]});
        }
    }
    if (!later.empty())
    {
        script.insert(script.end(), later.begin(), later.end());
        std::stable_sort(script.begin(), script.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    if (!events.empty()) deliver_touches(c, events);
}
}

}
