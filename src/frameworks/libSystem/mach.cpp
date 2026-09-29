#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard
{
namespace
{
constexpr uint64_t KERN_SUCCESS = 0, KERN_FAILURE = 5;
constexpr uint64_t MACH_RCV_MSG = 0x2, MACH_RCV_TIMEOUT = 0x100, MACH_RCV_TIMED_OUT = 0x10004003;

std::vector<GuestAddr> frame_walk(Cpu& c, size_t max)
{
    std::vector<GuestAddr> out{c.lr()};
    GuestAddr fp = c.x(29);
    while (out.size() < max && fp && c.mem.is_mapped(fp, 16))
    {
        GuestAddr ret = c.mem.read<uint64_t>(fp + 8);
        if (!ret || c.rt.hle.is_stub(ret)) break;
        out.push_back(ret);
        GuestAddr next = c.mem.read<uint64_t>(fp);
        if (next <= fp) break;
        fp = next;
    }
    return out;
}

void register_diagnostics(Hle& h)
{
    h.fn("_backtrace", [](Cpu& c) {
        auto frames = frame_walk(c, c.arg(1));
        for (size_t i = 0; i < frames.size(); ++i)
            c.mem.write<uint64_t>(c.arg(0) + i * 8, frames[i]);
        c.ret(frames.size());
    });
    h.fn("_backtrace_symbols", [](Cpu& c) {
        uint64_t n = c.arg(1);
        std::vector<std::string> lines;
        for (uint64_t i = 0; i < n; ++i)
        {
            GuestAddr a = c.mem.read<uint64_t>(c.arg(0) + i * 8);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%-4llu 0x%016llx ", (unsigned long long)i, (unsigned long long)a);
            lines.push_back(buf + c.rt.describe(a));
        }
        uint64_t size = n * 8;
        for (auto& l : lines)
            size += l.size() + 1;
        GuestAddr block = c.rt.heap.alloc(size);
        GuestAddr str = block + n * 8;
        for (uint64_t i = 0; i < n; ++i)
        {
            c.mem.write<uint64_t>(block + i * 8, str);
            c.mem.write_bytes(str, lines[i].c_str(), lines[i].size() + 1);
            str += lines[i].size() + 1;
        }
        c.ret(block);
    });
    h.fn("_pthread_stack_frame_decode_np", [](Cpu& c) {
        GuestAddr frame = c.arg(0);
        if (c.arg(1)) c.mem.write<uint64_t>(c.arg(1), c.mem.read<uint64_t>(frame + 8));
        c.ret(c.mem.read<uint64_t>(frame));
    });

    h.fn("_os_log_create", [](Cpu& c) { c.ret(c.rt.objc->alloc_instance(c.rt.objc->host_class("OS_os_log"))); });
    h.data("__os_log_default", [](Runtime& rt) {
        GuestAddr o = rt.mem.alloc_system(16, 16);
        rt.mem.write<uint64_t>(o, rt.objc->host_class("OS_os_log")->addr);
        return o;
    });
    h.data("__os_log_disabled", [](Runtime& rt) { return rt.hle.resolve("__os_log_default", "libsystem_trace"); });
    h.fn("_os_log_type_enabled", [](Cpu& c) { c.ret(0); });
    h.fn("_os_signpost_enabled", [](Cpu& c) { c.ret(0); });
    h.fn("_os_signpost_id_generate", [](Cpu& c) { c.ret(1); });
    for (const char* n : {"__os_log_impl", "__os_log_internal", "__os_log_error_impl", "__os_log_fault_impl", "__os_log_debug_impl",
                          "__os_signpost_emit_with_name_impl", "__os_activity_initiate", "_os_activity_scope_enter",
                          "_os_activity_scope_leave", "_os_trace_set_mode"})
        h.fn(n, [](Cpu& c) {});
    h.fn("__os_activity_create", [](Cpu& c) { c.ret(c.rt.hle.resolve("__os_log_default", "libsystem_trace")); });
    h.fn("_asl_log", [](Cpu& c) { c.ret(0); });
    h.fn("_mach_error_string", [](Cpu& c) {
        static GuestAddr s = c.mem.alloc_cstr_region("(mach error)");
        c.ret(s);
    });
}

void register_ports(Hle& h)
{
    static std::atomic<uint32_t> next_port{0x2003};
    h.fn("_mach_port_allocate", [](Cpu& c) {
        c.mem.write<uint32_t>(c.arg(2), next_port.fetch_add(0x100));
        c.ret(KERN_SUCCESS);
    });
    h.fn("_mach_port_construct", [](Cpu& c) {
        c.mem.write<uint32_t>(c.arg(3), next_port.fetch_add(0x100));
        c.ret(KERN_SUCCESS);
    });
    for (const char* n : {"_mach_port_insert_right", "_mach_port_mod_refs", "_mach_port_move_member", "_mach_port_destroy",
                          "_mach_port_request_notification", "_mach_port_set_attributes", "_mach_port_destruct"})
        h.fn(n, [](Cpu& c) { c.ret(KERN_SUCCESS); });
    h.fn("_mach_msg", [](Cpu& c) {
        uint64_t option = c.arg(1);
        if (!(option & MACH_RCV_MSG)) return c.ret(KERN_SUCCESS);
        if (option & MACH_RCV_TIMEOUT)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(c.arg(5)));
            return c.ret(MACH_RCV_TIMED_OUT);
        }
        while (!c.rt.halted())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        c.exit_thread();
    });

    h.fn("_task_get_exception_ports", [](Cpu& c) {
        if (c.arg(3)) c.mem.write<uint32_t>(c.arg(3), 0);
        c.ret(KERN_SUCCESS);
    });
    h.fn("_thread_get_exception_ports", [](Cpu& c) {
        if (c.arg(3)) c.mem.write<uint32_t>(c.arg(3), 0);
        c.ret(KERN_SUCCESS);
    });
    h.fn("_task_swap_exception_ports", [](Cpu& c) {
        if (c.arg(5)) c.mem.write<uint32_t>(c.arg(5), 0);
        c.ret(KERN_SUCCESS);
    });
    h.fn("_thread_swap_exception_ports", [](Cpu& c) {
        if (c.arg(5)) c.mem.write<uint32_t>(c.arg(5), 0);
        c.ret(KERN_SUCCESS);
    });
    for (const char* n : {"_task_set_exception_ports", "_thread_set_exception_ports", "_thread_policy_set", "_task_policy_set"})
        h.fn(n, [](Cpu& c) { c.ret(KERN_SUCCESS); });
    for (const char* n : {"_thread_set_state", "_exception_raise", "_exception_raise_state", "_exception_raise_state_identity"})
        h.fn(n, [](Cpu& c) { c.ret(KERN_FAILURE); });

    h.fn("_thread_suspend", [](Cpu& c) {
        Cpu* t = c.rt.find_cpu(c.arg(0) - 0x1000);
        if (!t || t == &c) return c.ret(KERN_FAILURE);
        t->request_suspend();
        if (!t->wait_until_parked()) return c.exit_thread();
        c.ret(KERN_SUCCESS);
    });
    h.fn("_thread_resume", [](Cpu& c) {
        Cpu* t = c.rt.find_cpu(c.arg(0) - 0x1000);
        if (!t) return c.ret(KERN_FAILURE);
        t->resume();
        c.ret(KERN_SUCCESS);
    });
    h.fn("_thread_get_state", [](Cpu& c) {
        Cpu* t = c.rt.find_cpu(c.arg(0) - 0x1000);
        if (!t || c.arg(1) != 6) return c.ret(KERN_FAILURE);
        uint64_t st[34];
        t->read_thread_state(st);
        GuestAddr out = c.arg(2);
        for (int i = 0; i < 33; ++i)
            c.mem.write<uint64_t>(out + i * 8, st[i]);
        c.mem.write<uint32_t>(out + 264, uint32_t(st[33]));
        c.mem.write<uint32_t>(out + 268, 0);
        if (c.arg(3)) c.mem.write<uint32_t>(c.arg(3), 68);
        c.ret(KERN_SUCCESS);
    });
    h.fn("_task_threads", [](Cpu& c) {
        auto cpus = c.rt.all_cpus();
        GuestAddr list = c.rt.heap.alloc(std::max<size_t>(cpus.size(), 1) * 4);
        for (size_t i = 0; i < cpus.size(); ++i)
            c.mem.write<uint32_t>(list + i * 4, uint32_t(0x1000 + cpus[i]->thread_id));
        c.mem.write<uint64_t>(c.arg(1), list);
        c.mem.write<uint32_t>(c.arg(2), uint32_t(cpus.size()));
        c.ret(KERN_SUCCESS);
    });
    auto zero_info = [](Cpu& c, GuestAddr info, GuestAddr count) {
        uint32_t n = count ? c.mem.read<uint32_t>(count) : 0;
        if (info && n) std::memset(c.mem.host(info), 0, uint64_t(n) * 4);
        c.ret(KERN_SUCCESS);
    };
    static decltype(zero_info) s_zero = zero_info;
    h.fn("_task_info", [](Cpu& c) { s_zero(c, c.arg(2), c.arg(3)); });
    h.fn("_thread_info", [](Cpu& c) { s_zero(c, c.arg(2), c.arg(3)); });
    h.fn("_host_statistics", [](Cpu& c) { s_zero(c, c.arg(2), c.arg(3)); });
    h.fn("_host_statistics64", [](Cpu& c) { s_zero(c, c.arg(2), c.arg(3)); });
    h.fn("_host_info", [](Cpu& c) { s_zero(c, c.arg(2), c.arg(3)); });
}

void register_vm(Hle& h)
{
    h.fn("_vm_read_overwrite", [](Cpu& c) {
        GuestAddr src = c.arg(1), dst = c.arg(3);
        uint64_t n = c.arg(2);
        if (!c.mem.is_mapped(src, n) || !c.mem.is_mapped(dst, n)) return c.ret(KERN_FAILURE);
        std::memmove(c.mem.host(dst), c.mem.host(src), n);
        if (c.arg(4)) c.mem.write<uint64_t>(c.arg(4), n);
        c.ret(KERN_SUCCESS);
    });
    h.fn("_vm_allocate", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(1), c.mem.map_anywhere(c.arg(2), "vm_allocate"));
        c.ret(KERN_SUCCESS);
    });
    h.fn("_vm_deallocate", [](Cpu& c) {
        c.mem.unmap(c.arg(1), c.arg(2));
        c.ret(KERN_SUCCESS);
    });
    h.fn("_vm_protect", [](Cpu& c) { c.ret(KERN_SUCCESS); });
    h.fn("_vm_map", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(1), c.mem.map_anywhere(c.arg(2), "vm_map"));
        c.ret(KERN_SUCCESS);
    });
    h.fn("_mach_make_memory_entry_64", [](Cpu& c) { c.ret(KERN_FAILURE); });
    h.fn("_mach_vm_region", [](Cpu& c) { c.ret(KERN_FAILURE); });
    h.fn("_vm_region_64", [](Cpu& c) { c.ret(KERN_FAILURE); });
}

}

void register_mach(Hle& h)
{
    register_diagnostics(h);
    register_ports(h);
    register_vm(h);
}

}
