#include <mutex>
#include <utility>
#include <unordered_map>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "hle/hle.h"
#include "host/permission.h"
#include "objc/runtime.h"

namespace orchard::network
{
using objc::objc;

namespace
{
constexpr uint64_t kSatisfied = 1, kUnsatisfied = 2, kWifi = 1;

struct Monitor
{
    GuestAddr handler = 0, cancel_handler = 0, queue = 0, path = 0;
};

std::mutex lock;
std::unordered_map<GuestAddr, Monitor> monitors;

bool online()
{
    return network_allowed();
}

GuestAddr new_object(Cpu& c, const char* cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}

GuestAddr update_stub(Cpu& c)
{
    static GuestAddr stub = c.rt.hle.make_stub("nw_path_monitor update", [](Cpu& k) {
        Monitor m;
        {
            std::lock_guard g(lock);
            auto it = monitors.find(k.arg(0));
            if (it == monitors.end()) return;
            m = it->second;
        }
        if (m.handler && m.path) call_block(k, m.handler, {m.path});
    });
    return stub;
}
}

void register_network(objc::ObjcRuntime& o)
{
    o.define("OS_nw_object", "OS_object");
    o.define("OS_nw_path_monitor", "OS_nw_object");
    o.define("OS_nw_path", "OS_nw_object");
    Hle& h = o.rt.hle;

    auto create = [](Cpu& c) {
        GuestAddr m = new_object(c, "OS_nw_path_monitor");
        GuestAddr path = new_object(c, "OS_nw_path");
        std::lock_guard g(lock);
        monitors[m].path = path;
        c.ret(m);
    };
    h.fn("_nw_path_monitor_create", create);
    h.fn("_nw_path_monitor_create_with_type", create);
    h.fn("_nw_path_monitor_create_for_ethernet_channel", create);
    h.fn("_nw_path_monitor_set_queue", [](Cpu& c) {
        std::lock_guard g(lock);
        monitors[c.arg(0)].queue = c.arg(1);
    });
    h.fn("_nw_path_monitor_set_update_handler", [](Cpu& c) {
        GuestAddr copy = c.arg(1) ? block_copy(c, c.arg(1)) : 0;
        GuestAddr old;
        {
            std::lock_guard g(lock);
            old = std::exchange(monitors[c.arg(0)].handler, copy);
        }
        if (old) block_release(c, old);
    });
    h.fn("_nw_path_monitor_set_cancel_handler", [](Cpu& c) {
        GuestAddr copy = c.arg(1) ? block_copy(c, c.arg(1)) : 0;
        std::lock_guard g(lock);
        monitors[c.arg(0)].cancel_handler = copy;
    });
    h.fn("_nw_path_monitor_start", [](Cpu& c) {
        GuestAddr queue;
        {
            std::lock_guard g(lock);
            queue = monitors[c.arg(0)].queue;
        }
        online();
        dispatch_function_async(queue ? queue : main_queue_object(c), update_stub(c), c.arg(0));
    });
    h.fn("_nw_path_monitor_cancel", [](Cpu& c) {
        Monitor m;
        {
            std::lock_guard g(lock);
            auto it = monitors.find(c.arg(0));
            if (it == monitors.end()) return;
            m = it->second;
            it->second.handler = 0;
        }
        if (m.cancel_handler) dispatch_block_async(c, m.queue ? m.queue : main_queue_object(c), m.cancel_handler);
    });
    h.fn("_nw_path_monitor_prohibit_interface_type", [](Cpu& c) {});
    h.fn("_nw_path_monitor_copy_current_path", [](Cpu& c) {
        std::lock_guard g(lock);
        auto it = monitors.find(c.arg(0));
        c.ret(it == monitors.end() ? 0 : objc(c).retain(it->second.path));
    });

    h.fn("_nw_path_get_status", [](Cpu& c) { c.ret(online() ? kSatisfied : kUnsatisfied); });
    h.fn("_nw_path_get_unsatisfied_reason", [](Cpu& c) { c.ret(0); });
    h.fn("_nw_path_uses_interface_type", [](Cpu& c) { c.ret(c.arg(1) == kWifi && online()); });
    for (const char* n : {"_nw_path_has_ipv4", "_nw_path_has_ipv6", "_nw_path_has_dns"})
        h.fn(n, [](Cpu& c) { c.ret(online()); });
    for (const char* n : {"_nw_path_is_expensive", "_nw_path_is_constrained", "_nw_path_is_ultra_constrained"})
        h.fn(n, [](Cpu& c) { c.ret(0); });
    h.fn("_nw_path_is_equal", [](Cpu& c) { c.ret(1); });
    for (const char* n : {"_nw_path_enumerate_interfaces", "_nw_path_enumerate_gateways"})
        h.fn(n, [](Cpu& c) {});
    h.fn("_nw_path_copy_effective_local_endpoint", [](Cpu& c) { c.ret(0); });
    h.fn("_nw_path_copy_effective_remote_endpoint", [](Cpu& c) { c.ret(0); });
    h.fn("_nw_retain", [](Cpu& c) { c.ret(c.arg(0) ? objc(c).retain(c.arg(0)) : 0); });
    h.fn("_nw_release", [](Cpu& c) {
        if (c.arg(0)) objc(c).release(c, c.arg(0));
    });
}

}
