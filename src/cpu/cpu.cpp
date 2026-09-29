#include "cpu/cpu.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <intrin.h>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "core/runtime.h"
#include "loader/cache_images.h"

namespace orchard
{
using Dynarmic::A64::VAddr;
using Dynarmic::A64::Vector;

class JitCallbacks final : public Dynarmic::A64::UserCallbacks
{
public:
    JitCallbacks(Cpu& cpu, Cpu::Level& level) : cpu_(cpu), level_(level) {}

    std::optional<uint32_t> MemoryReadCode(VAddr vaddr) override
    {
        if (cpu_.rt.hle.is_stub(vaddr)) return cpu_.rt.hle.stub_code(vaddr);
        if (!cpu_.mem.is_mapped(vaddr, 4)) return std::nullopt;
        return cpu_.mem.read<uint32_t>(vaddr);
    }

    uint8_t MemoryRead8(VAddr a) override { return read<uint8_t>(a); }
    uint16_t MemoryRead16(VAddr a) override { return read<uint16_t>(a); }
    uint32_t MemoryRead32(VAddr a) override { return read<uint32_t>(a); }
    uint64_t MemoryRead64(VAddr a) override { return read<uint64_t>(a); }
    Vector MemoryRead128(VAddr a) override { return read<Vector>(a); }
    void MemoryWrite8(VAddr a, uint8_t v) override { write(a, v); }
    void MemoryWrite16(VAddr a, uint16_t v) override { write(a, v); }
    void MemoryWrite32(VAddr a, uint32_t v) override { write(a, v); }
    void MemoryWrite64(VAddr a, uint64_t v) override { write(a, v); }
    void MemoryWrite128(VAddr a, Vector v) override { write(a, v); }

    bool MemoryWriteExclusive8(VAddr a, uint8_t v, uint8_t expected) override
    {
        return cas(a,
                   [&](void* p) { return _InterlockedCompareExchange8(static_cast<char*>(p), char(v), char(expected)) == char(expected); });
    }
    bool MemoryWriteExclusive16(VAddr a, uint16_t v, uint16_t expected) override
    {
        return cas(a, [&](void* p) {
            return _InterlockedCompareExchange16(static_cast<short*>(p), short(v), short(expected)) == short(expected);
        });
    }
    bool MemoryWriteExclusive32(VAddr a, uint32_t v, uint32_t expected) override
    {
        return cas(a,
                   [&](void* p) { return _InterlockedCompareExchange(static_cast<long*>(p), long(v), long(expected)) == long(expected); });
    }
    bool MemoryWriteExclusive64(VAddr a, uint64_t v, uint64_t expected) override
    {
        return cas(a, [&](void* p) {
            return _InterlockedCompareExchange64(static_cast<long long*>(p), (long long)v, (long long)expected) == (long long)expected;
        });
    }
    bool MemoryWriteExclusive128(VAddr a, Vector v, Vector expected) override
    {
        return cas(a, [&](void* p) {
            long long cmp[2] = {(long long)expected[0], (long long)expected[1]};
            return _InterlockedCompareExchange128(static_cast<long long*>(p), (long long)v[1], (long long)v[0], cmp) != 0;
        });
    }

    void InterpreterFallback(VAddr pc, size_t) override
    {
        char buf[160];
        std::snprintf(buf, sizeof buf, "unsupported instruction %08x at %s", read<uint32_t>(pc), cpu_.rt.describe(pc).c_str());
        cpu_.stop(buf);
    }

    void CallSVC(uint32_t) override { cpu_.on_svc(level_); }

    void ExceptionRaised(VAddr pc, Dynarmic::A64::Exception e) override
    {
        using E = Dynarmic::A64::Exception;
        if (e == E::Yield || e == E::WaitForEvent || e == E::SendEvent || e == E::SendEventLocal || e == E::WaitForInterrupt) return;
        const char* what = e == E::Breakpoint            ? "brk (guest trap)"
                           : e == E::NoExecuteFault      ? "jump to unmapped address"
                           : e == E::UnallocatedEncoding ? "unallocated encoding"
                                                         : "cpu exception";
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s at 0x%llx (%s), lr %s", what, (unsigned long long)pc, cpu_.rt.describe(pc).c_str(),
                      cpu_.rt.describe(cpu_.lr()).c_str());
        cpu_.stop(buf);
    }

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~uint64_t(0); }

    uint64_t GetCNTPCT() override
    {
        static const uint64_t freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return uint64_t(f.QuadPart);
        }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        uint64_t t = uint64_t(now.QuadPart);
        return (t / freq) * 24000000 + (t % freq) * 24000000 / freq;
    }

private:
    template <typename T> T read(VAddr a)
    {
        if (!cpu_.mem.is_mapped(a, sizeof(T)))
        {
            fault("read", a);
            return T{};
        }
        return cpu_.mem.read<T>(a);
    }

    template <typename T> void write(VAddr a, const T& v)
    {
        if (!cpu_.mem.is_mapped(a, sizeof(T)))
        {
            fault("write", a);
            return;
        }
        cpu_.mem.write(a, v);
    }

    template <typename F> bool cas(VAddr a, F&& op)
    {
        if (!cpu_.mem.is_mapped(a, 1))
        {
            fault("exclusive write", a);
            return false;
        }
        return op(cpu_.mem.host(a));
    }

    void fault(const char* kind, VAddr a)
    {
        char buf[256];
        if (cpu_.rt.hle.is_stub(a))
        {
            std::snprintf(buf, sizeof buf, "data %s of imported symbol %s (not implemented as data) from %s", kind,
                          cpu_.rt.hle.stub_name(a & ~uint64_t(7)).c_str(), cpu_.rt.describe(cpu_.pc()).c_str());
        }
        else
        {
            std::snprintf(buf, sizeof buf, "%s fault at 0x%llx from %s", kind, (unsigned long long)a, cpu_.rt.describe(cpu_.pc()).c_str());
        }
        cpu_.stop(buf);
    }

    Cpu& cpu_;
    Cpu::Level& level_;
};

struct Cpu::Level
{
    std::unique_ptr<JitCallbacks> callbacks;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
};

Cpu::Cpu(Runtime& runtime, size_t processor_id, size_t cache_mb)
    : rt(runtime), mem(runtime.mem), processor_id_(processor_id % Runtime::kMaxProcessors), cache_mb_(cache_mb)
{
    rt.register_cpu(this);
}

Cpu::~Cpu()
{
    rt.unregister_cpu(this);
}

void Cpu::park_for_reuse()
{
    rt.unregister_cpu(this);
    stopped_ = false;
    exited = false;
    exit_value = 0;
    stop_reason_.clear();
    stop_backtrace_.clear();
    stop_hle_stack_.clear();
    hle_stack.clear();
    autorelease_pool.clear();
    tlv_blocks.clear();
    suspend_count_ = 0;
    parked_ = true;
}

void Cpu::unpark_for_reuse()
{
    rt.register_cpu(this);
}

Cpu::Level& Cpu::current() const
{
    return *levels_[depth_ < 0 ? 0 : depth_];
}

uint64_t Cpu::x(int i) const
{
    return current().jit->GetRegister(i);
}
void Cpu::set_x(int i, uint64_t v)
{
    current().jit->SetRegister(i, v);
}
GuestAddr Cpu::sp() const
{
    return current().jit->GetSP();
}
void Cpu::set_sp(GuestAddr v)
{
    current().jit->SetSP(v);
}
GuestAddr Cpu::pc() const
{
    return current().jit->GetPC();
}

double Cpu::d(int i) const
{
    double v;
    uint64_t lo = current().jit->GetVector(i)[0];
    std::memcpy(&v, &lo, 8);
    return v;
}
void Cpu::set_d(int i, double v)
{
    Vector vec{};
    std::memcpy(&vec[0], &v, 8);
    current().jit->SetVector(i, vec);
}
float Cpu::s(int i) const
{
    float v;
    uint32_t lo = uint32_t(current().jit->GetVector(i)[0]);
    std::memcpy(&v, &lo, 4);
    return v;
}
void Cpu::set_s(int i, float v)
{
    Vector vec{};
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    vec[0] = bits;
    current().jit->SetVector(i, vec);
}

uint64_t Cpu::call(GuestAddr fn, std::initializer_list<uint64_t> args)
{
    if (stopped_) return 0;
    GuestAddr sp_now = depth_ >= 0 ? current().jit->GetSP() : initial_sp_;
    std::array<uint64_t, 31> caller_regs{};
    std::array<Vector, 32> caller_vectors{};
    uint32_t caller_fpcr = 0;
    if (depth_ >= 0)
    {
        caller_regs = current().jit->GetRegisters();
        caller_vectors = current().jit->GetVectors();
        caller_fpcr = current().jit->GetFpcr();
    }

    ++depth_;
    bool was_parked = parked_.exchange(false);
    if (size_t(depth_) >= levels_.size())
    {
        auto level = std::make_unique<Level>();
        level->callbacks = std::make_unique<JitCallbacks>(*this, *level);
        Dynarmic::A64::UserConfig conf;
        conf.callbacks = level->callbacks.get();
        conf.processor_id = processor_id_;
        conf.global_monitor = &rt.monitor;
        conf.fastmem_pointer = reinterpret_cast<uintptr_t>(mem.base());
        conf.fastmem_address_space_bits = layout::kAddressBits;
        conf.silently_mirror_fastmem = true;
        conf.fastmem_exclusive_access = true;
        conf.recompile_on_exclusive_fastmem_failure = true;
        conf.tpidrro_el0 = &tpidrro_;
        conf.tpidr_el0 = &tpidr_;
        conf.cntfrq_el0 = 24000000;
        conf.dczid_el0 = 4;
        conf.ctr_el0 = 0x8444c004;
        conf.enable_cycle_counting = false;
        conf.code_cache_size = (depth_ == 0 ? cache_mb_ : std::max<size_t>(cache_mb_ / 4, 16)) << 20;
        level->jit = std::make_unique<Dynarmic::A64::Jit>(conf);
        std::lock_guard g(levels_lock_);
        levels_.push_back(std::move(level));
    }

    auto& jit = *levels_[depth_]->jit;
    jit.SetRegisters(caller_regs);
    jit.SetVectors(caller_vectors);
    jit.SetFpcr(caller_fpcr);
    size_t i = 0;
    for (uint64_t a : args)
        jit.SetRegister(i++, a);
    jit.SetRegister(30, rt.hle.return_trampoline());
    jit.SetSP(sp_now & ~uint64_t(15));
    jit.SetPC(fn);
    jit.SetPstate(0);

    while (!stopped_)
    {
        park_if_suspended();
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1 | Dynarmic::HaltReason::UserDefined2);
        auto reason = jit.Run();
        if (Dynarmic::Has(reason, Dynarmic::HaltReason::UserDefined2)) break;
        if (!Dynarmic::Has(reason, Dynarmic::HaltReason::UserDefined1) && !stopped_)
        {
            stop("jit halted unexpectedly");
        }
    }

    uint64_t result = jit.GetRegister(0);
    auto result_vectors = jit.GetVectors();
    --depth_;
    if (depth_ >= 0)
    {
        auto& caller = *current().jit;
        auto v = caller.GetVectors();
        std::copy_n(result_vectors.begin(), 8, v.begin());
        caller.SetVectors(v);
    }
    parked_ = was_parked;
    return result;
}

void Cpu::in_hle_dispatch(Level& level, GuestAddr stub)
{
    (void)level;
    parked_ = true;
    {
        std::lock_guard g(suspend_lock_);
        suspend_cv_.notify_all();
    }
    rt.hle.dispatch(*this, stub);
    parked_ = false;
    park_if_suspended();
}

void Cpu::request_suspend()
{
    ++suspend_count_;
    halt_levels();
}

void Cpu::resume()
{
    std::lock_guard g(suspend_lock_);
    if (suspend_count_ > 0) --suspend_count_;
    suspend_cv_.notify_all();
}

bool Cpu::wait_until_parked()
{
    std::unique_lock l(suspend_lock_);
    return rt.wait(suspend_cv_, l, [&] { return parked_.load() || stopped_.load(); });
}

void Cpu::park_if_suspended()
{
    if (suspend_count_ == 0) return;
    std::unique_lock l(suspend_lock_);
    parked_ = true;
    suspend_cv_.notify_all();
    rt.wait(suspend_cv_, l, [&] { return suspend_count_ == 0; });
    parked_ = false;
}

void Cpu::read_thread_state(uint64_t out[34]) const
{
    std::fill(out, out + 34, 0);
    if (levels_.empty() || depth_ < 0)
    {
        out[31] = initial_sp_;
        return;
    }
    const auto& jit = *current().jit;
    auto regs = jit.GetRegisters();
    for (int i = 0; i < 31; ++i)
        out[i] = regs[i];
    out[31] = jit.GetSP();
    out[32] = jit.GetPC();
    out[33] = jit.GetPstate();
}

void Cpu::on_svc(Level& level)
{
    ++svc_count_;
    park_if_suspended();
    GuestAddr at = level.jit->GetPC() - 4;
    if (at == rt.hle.return_trampoline())
    {
        level.jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    if (rt.hle.is_stub(at))
    {
        in_hle_dispatch(level, at);
        return;
    }
    if (rt.cache_images)
    {
        if (auto stub = rt.cache_images->patch_target(at))
        {
            in_hle_dispatch(level, *stub);
            if (!stopped_) level.jit->SetPC(rt.hle.is_tail(*stub) ? level.jit->GetRegister(16) : level.jit->GetRegister(30));
            return;
        }
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "guest system call (svc) at %s, x16=0x%llx", rt.describe(at).c_str(),
                  (unsigned long long)level.jit->GetRegister(16));
    stop(buf);
}

void Cpu::halt_levels()
{
    std::lock_guard g(levels_lock_);
    for (auto& l : levels_)
        if (l->jit->IsExecuting()) l->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void Cpu::exit_thread()
{
    stopped_ = true;
    halt_levels();
}

void Cpu::stop(std::string reason)
{
    if (!stopped_.exchange(true))
    {
        stop_reason_ = std::move(reason);
        for (const std::string* n : hle_stack)
            stop_hle_stack_.push_back(*n);
        if (depth_ >= 0)
        {
            auto& jit = *current().jit;
            stop_backtrace_ = {jit.GetPC(), jit.GetRegister(30)};
            GuestAddr fp = jit.GetRegister(29);
            for (int i = 0; i < 24 && fp && mem.is_mapped(fp, 16); ++i)
            {
                GuestAddr ret = mem.read<uint64_t>(fp + 8);
                if (!ret) break;
                stop_backtrace_.push_back(ret);
                GuestAddr next = mem.read<uint64_t>(fp);
                if (next <= fp) break;
                fp = next;
            }
        }
        rt.fail(*this);
    }
    halt_levels();
}

}
