#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/memory.h"

namespace orchard
{
class Cpu;
class HostWindow;
struct PointerEvent;
class Hle;
namespace objc
{
class ObjcRuntime;
}
}

namespace orchard::uikit
{
using Id = GuestAddr;

constexpr double kScreenW = 375, kScreenH = 812, kScreenScale = 3;

struct Rect
{
    double x = 0, y = 0, w = 0, h = 0;
};

Rect rect_arg(Cpu& c, int first_d = 0);
void ret_rect(Cpu& c, const Rect& r);
void ret_size(Cpu& c, double w, double h);
void ret_point(Cpu& c, double x, double y);

struct ViewState
{
    Rect frame, bounds;
    Id layer = 0;
    Id superview = 0;
    std::vector<Id> subviews;
    Id controller = 0;
    bool hidden = false;
    bool interactive = true;
    double alpha = 1;
    double content_scale = kScreenScale;
    bool needs_layout = true;
    int64_t tag = 0;
};

struct LayerState
{
    Rect frame, bounds;
    double contents_scale = 1;
    Id delegate = 0;
    Id superlayer = 0;
    std::vector<Id> sublayers;
    bool hidden = false;
    Id device = 0;
    uint64_t pixel_format = 80;
    double drawable_w = 0, drawable_h = 0;
    bool framebuffer_only = true;
    uint64_t max_drawables = 3;
};

struct ControllerState
{
    Id view = 0;
    Id parent = 0;
    std::vector<Id> children;
};

template <typename T> class StateTable
{
public:
    T& get(Id obj)
    {
        std::lock_guard g(lock_);
        auto& p = items_[obj];
        if (!p) p = std::make_unique<T>();
        return *p;
    }
    T* find(Id obj)
    {
        std::lock_guard g(lock_);
        auto it = items_.find(obj);
        return it == items_.end() ? nullptr : it->second.get();
    }
    void erase(Id obj)
    {
        std::lock_guard g(lock_);
        items_.erase(obj);
    }
    template <typename F> void each(F&& f)
    {
        std::vector<Id> keys;
        {
            std::lock_guard g(lock_);
            for (auto& [k, v] : items_)
                keys.push_back(k);
        }
        for (Id k : keys)
            f(k);
    }

private:
    std::mutex lock_;
    std::unordered_map<Id, std::unique_ptr<T>> items_;
};

StateTable<ViewState>& views();
StateTable<LayerState>& layers();
StateTable<ControllerState>& controllers();

struct App
{
    Id application = 0;
    Id delegate = 0;
    Id scene = 0;
    Id scene_delegate = 0;
    Id key_window = 0;
    std::vector<Id> windows;
    HostWindow* window = nullptr;
    double quit_after = 0;
    std::string screenshot;
    double started = 0;
    std::vector<std::pair<double, std::vector<float>>> script;
};
App& app();

void layout_pass(Cpu& c);
void point_to_window(Id view, double& x, double& y);
void point_from_window(Id view, double& x, double& y);
void deliver_touches(Cpu& c, const std::vector<PointerEvent>& events);
Id controller_view(Cpu& c, Id vc);

void register_uikit(objc::ObjcRuntime& o);
void register_views(objc::ObjcRuntime& o);
void register_quartz(objc::ObjcRuntime& o);
void register_runloop(objc::ObjcRuntime& o);
void register_touches(objc::ObjcRuntime& o);
void register_cg(Hle& h);

bool run_loop_turn(Cpu& c, double max_wait_seconds);

}
