#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/UIKit/uikit.h"
#include "host/window.h"
#include "objc/runtime.h"

namespace orchard::uikit
{
using objc::objc;

namespace
{
enum Phase : uint64_t
{
    Began = 0,
    Moved = 1,
    Stationary = 2,
    Ended = 3,
    Cancelled = 4
};

struct TouchState
{
    double x = 0, y = 0, px = 0, py = 0;
    uint64_t phase = Began;
    double timestamp = 0;
    Id view = 0, window = 0;
    uint64_t taps = 1;
};

StateTable<TouchState>& touches()
{
    static StateTable<TouchState> t;
    return t;
}

Id g_active = 0;
Id g_event = 0;
double g_event_time = 0;
double g_last_tap_time = -1, g_last_tap_x = 0, g_last_tap_y = 0;
uint64_t g_last_taps = 0;

double now_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Id send(Cpu& c, Id o, const char* sel, std::initializer_list<uint64_t> args = {})
{
    return objc(c).send(c, o, sel, args);
}

Id make_set(Cpu& c, const std::vector<Id>& items)
{
    return send(c, objc(c).host_class("NSSet")->addr, "setWithArray:", {foundation::make_array(c, items)});
}

std::vector<Id> active_touches()
{
    return g_active ? std::vector<Id>{g_active} : std::vector<Id>{};
}

Id target_window()
{
    App& a = app();
    if (a.key_window) return a.key_window;
    return a.windows.empty() ? 0 : a.windows.back();
}

void location(Cpu& c, bool previous)
{
    TouchState* t = touches().find(c.arg(0));
    if (!t) return ret_point(c, 0, 0);
    double x = previous ? t->px : t->x, y = previous ? t->py : t->y;
    if (c.arg(2)) point_from_window(c.arg(2), x, y);
    ret_point(c, x, y);
}

const char* phase_selector(uint64_t phase)
{
    switch (phase)
    {
    case Began: return "touchesBegan:withEvent:";
    case Ended: return "touchesEnded:withEvent:";
    case Cancelled: return "touchesCancelled:withEvent:";
    default: return "touchesMoved:withEvent:";
    }
}

void dispatch(Cpu& c)
{
    g_event_time = touches().get(g_active).timestamp;
    if (app().application)
        send(c, app().application, "sendEvent:", {g_event});
    else if (Id w = touches().get(g_active).window)
        send(c, w, "sendEvent:", {g_event});
}

void begin(Cpu& c, double x, double y)
{
    Id window = target_window();
    if (!window) return;
    Id touch = objc(c).alloc_instance(objc(c).host_class("UITouch"));
    TouchState& t = touches().get(touch);
    t.x = t.px = x;
    t.y = t.py = y;
    t.timestamp = now_seconds();
    t.window = window;
    bool repeat = t.timestamp - g_last_tap_time < 0.35 && std::hypot(x - g_last_tap_x, y - g_last_tap_y) < 30;
    t.taps = repeat ? g_last_taps + 1 : 1;
    g_active = touch;
    Id hit = 0;
    GuestAddr imp = objc(c).resolve_send(c, window, objc(c).sel("hitTest:withEvent:"));
    if (imp)
    {
        c.set_d(0, x);
        c.set_d(1, y);
        hit = c.call(imp, {window, objc(c).sel("hitTest:withEvent:"), g_event});
    }
    touches().get(touch).view = hit ? hit : window;
    static bool log_touches = [] {
        const char* t = std::getenv("ORCHARD_TRACE");
        return t && std::string(t).find("touch") != std::string::npos;
    }();
    if (log_touches)
    {
        auto* k = objc(c).class_of(touches().get(touch).view);
        std::printf("[UIKit] touch at %.0f,%.0f -> %s\n", x, y, k ? k->name.c_str() : "?");
    }
    dispatch(c);
}

void finish(Cpu& c, uint64_t phase)
{
    TouchState& t = touches().get(g_active);
    t.phase = phase;
    t.timestamp = now_seconds();
    g_last_tap_time = t.timestamp;
    g_last_tap_x = t.x;
    g_last_tap_y = t.y;
    g_last_taps = t.taps;
    dispatch(c);
    objc(c).release(c, g_active);
    g_active = 0;
}
}

void deliver_touches(Cpu& c, const std::vector<PointerEvent>& events)
{
    auto pool = objc(c).autorelease_push(c);
    if (!g_event) g_event = objc(c).alloc_instance(objc(c).host_class("UIEvent"));
    for (size_t i = 0; i < events.size() && !c.stopped(); ++i)
    {
        const PointerEvent& e = events[i];
        switch (e.kind)
        {
        case PointerEvent::Kind::Down:
            if (g_active) finish(c, Cancelled);
            begin(c, e.x, e.y);
            break;
        case PointerEvent::Kind::Move: {
            if (!g_active) break;
            if (i + 1 < events.size() && events[i + 1].kind == PointerEvent::Kind::Move) break;
            TouchState& t = touches().get(g_active);
            if (t.x == e.x && t.y == e.y) break;
            t.px = t.x;
            t.py = t.y;
            t.x = e.x;
            t.y = e.y;
            t.phase = Moved;
            t.timestamp = now_seconds();
            dispatch(c);
            break;
        }
        case PointerEvent::Kind::Up:
            if (!g_active) break;
            {
                TouchState& t = touches().get(g_active);
                t.px = t.x;
                t.py = t.y;
                t.x = e.x;
                t.y = e.y;
            }
            finish(c, Ended);
            break;
        }
    }
    objc(c).autorelease_pop(c, pool);
}

void register_touches(objc::ObjcRuntime& o)
{
    o.method("UITouch", "dealloc", [](Cpu& c) {
        touches().erase(c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    o.method("UITouch", "locationInView:", [](Cpu& c) { location(c, false); });
    o.method("UITouch", "preciseLocationInView:", [](Cpu& c) { location(c, false); });
    o.method("UITouch", "previousLocationInView:", [](Cpu& c) { location(c, true); });
    o.method("UITouch", "precisePreviousLocationInView:", [](Cpu& c) { location(c, true); });
    o.method("UITouch", "phase", [](Cpu& c) { c.ret(touches().get(c.arg(0)).phase); });
    o.method("UITouch", "tapCount", [](Cpu& c) { c.ret(touches().get(c.arg(0)).taps); });
    o.method("UITouch", "timestamp", [](Cpu& c) { c.set_d(0, touches().get(c.arg(0)).timestamp); });
    o.method("UITouch", "view", [](Cpu& c) { c.ret(touches().get(c.arg(0)).view); });
    o.method("UITouch", "window", [](Cpu& c) { c.ret(touches().get(c.arg(0)).window); });
    o.method("UITouch", "type", [](Cpu& c) { c.ret(0); });
    o.method("UITouch", "force", [](Cpu& c) { c.set_d(0, 0); });
    o.method("UITouch", "maximumPossibleForce", [](Cpu& c) { c.set_d(0, 0); });
    o.method("UITouch", "majorRadius", [](Cpu& c) { c.set_d(0, 20); });
    o.method("UITouch", "majorRadiusTolerance", [](Cpu& c) { c.set_d(0, 5); });
    o.method("UITouch", "altitudeAngle", [](Cpu& c) { c.set_d(0, 1.5707963267948966); });
    o.method("UITouch", "azimuthAngleInView:", [](Cpu& c) { c.set_d(0, 0); });
    o.method("UITouch", "estimatedProperties", [](Cpu& c) { c.ret(0); });
    o.method("UITouch", "estimatedPropertiesExpectingUpdates", [](Cpu& c) { c.ret(0); });
    o.method("UITouch", "estimationUpdateIndex", [](Cpu& c) { c.ret(0); });
    o.method("UITouch", "gestureRecognizers", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });

    o.method("UIEvent", "type", [](Cpu& c) { c.ret(0); });
    o.method("UIEvent", "subtype", [](Cpu& c) { c.ret(0); });
    o.method("UIEvent", "timestamp", [](Cpu& c) { c.set_d(0, g_event_time); });
    o.method("UIEvent", "allTouches", [](Cpu& c) { c.ret(make_set(c, active_touches())); });
    o.method("UIEvent", "touchesForView:", [](Cpu& c) {
        std::vector<Id> out;
        for (Id t : active_touches())
            if (touches().get(t).view == c.arg(2)) out.push_back(t);
        c.ret(make_set(c, out));
    });
    o.method("UIEvent", "touchesForWindow:", [](Cpu& c) {
        std::vector<Id> out;
        for (Id t : active_touches())
            if (touches().get(t).window == c.arg(2)) out.push_back(t);
        c.ret(make_set(c, out));
    });
    o.method("UIEvent", "touchesForGestureRecognizer:", [](Cpu& c) { c.ret(make_set(c, {})); });
    o.method("UIEvent", "coalescedTouchesForTouch:", [](Cpu& c) {
        c.ret(foundation::make_array(c, c.arg(2) ? std::vector<Id>{c.arg(2)} : std::vector<Id>{}));
    });
    o.method("UIEvent", "predictedTouchesForTouch:", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });

    o.method("UIApplication", "sendEvent:", [](Cpu& c) {
        for (Id t : active_touches())
            if (Id w = touches().get(t).window) send(c, w, "sendEvent:", {c.arg(2)});
    });
    o.method("UIWindow", "sendEvent:", [](Cpu& c) {
        for (Id t : active_touches())
        {
            const TouchState& s = touches().get(t);
            if (s.window != c.arg(0) || !s.view) continue;
            Id view = s.view;
            const char* sel = phase_selector(s.phase);
            send(c, view, sel, {make_set(c, {t}), c.arg(2)});
        }
    });
    o.method("UIViewController", "nextResponder", [](Cpu& c) {
        Id view = controllers().get(c.arg(0)).view;
        c.ret(view ? views().get(view).superview : 0);
    });
}

}
