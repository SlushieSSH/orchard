#include <chrono>
#include <cstdio>
#include <vector>

#include "core/runtime.h"
#include "cpu/cpu.h"

using namespace orchard;

namespace
{
int failures = 0;

void expect(const char* what, uint64_t got, uint64_t want)
{
    bool ok = got == want;
    if (!ok) ++failures;
    std::printf("%-44s %s (got %llu, want %llu)\n", what, ok ? "ok  " : "FAIL", (unsigned long long)got, (unsigned long long)want);
}

GuestAddr put_code(Memory& mem, GuestAddr& at, std::vector<uint32_t> words)
{
    GuestAddr start = at;
    for (uint32_t w : words)
    {
        mem.write(at, w);
        at += 4;
    }
    at = (at + 15) & ~GuestAddr(15);
    return start;
}

}

int main()
{
    Runtime rt;
    Cpu cpu(rt, 0);

    GuestAddr code = rt.mem.map_anywhere(0x10000, "test-code");
    GuestAddr stack = rt.mem.map_anywhere(0x100000, "test-stack");
    cpu.set_initial_stack(stack + 0x100000);
    GuestAddr at = code;

    GuestAddr add = put_code(rt.mem, at,
                             {
                                 0x8b010000,
                                 0xd65f03c0,
                             });
    expect("add x0, x1", cpu.call(add, {40, 2}), 42);

    GuestAddr sum = put_code(rt.mem, at,
                             {
                                 0xd2800002,
                                 0x8b000042,
                                 0xf1000400,
                                 0x54ffffc1,
                                 0xaa0203e0,
                                 0xd65f03c0,
                             });
    auto t0 = std::chrono::steady_clock::now();
    uint64_t n = 200'000'000;
    uint64_t got = cpu.call(sum, {n});
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    expect("sum 1..200M loop", got, n * (n + 1) / 2);
    std::printf("  %.0f M guest instructions/s\n", (3.0 * n) / secs / 1e6);

    GuestAddr call_x1 = put_code(rt.mem, at,
                                 {
                                     0xa9bf7bfd,
                                     0xd63f0020,
                                     0xa8c17bfd,
                                     0xd65f03c0,
                                 });
    GuestAddr str = rt.mem.alloc_cstr_region("orchard");
    GuestAddr strlen_stub = rt.hle.resolve("_strlen", "/usr/lib/libSystem.B.dylib");
    expect("guest -> HLE strlen", cpu.call(call_x1, {str, strlen_stub}), 7);

    static GuestAddr s_add = add;
    GuestAddr nest = rt.hle.make_stub("test_nest", [](Cpu& c) { c.ret(c.call(s_add, {c.arg(0), 100}) * 2); });
    expect("guest -> HLE -> guest (nested JIT)", cpu.call(call_x1, {5, nest}), 210);

    GuestAddr p = rt.heap.alloc(100);
    expect("guest heap pointer in regions area", p >= layout::kRegionsBase, 1);

    GuestAddr unimpl = rt.hle.resolve("_definitely_not_implemented", "/usr/lib/libSystem.B.dylib");
    cpu.call(call_x1, {0, unimpl});
    expect("unimplemented import stops the cpu", cpu.stopped(), 1);
    std::printf("  stop reason: %s\n", cpu.stop_reason().c_str());

    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
