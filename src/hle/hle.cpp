#include "hle/hle.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "objc/runtime.h"

namespace orchard
{
namespace
{
constexpr uint32_t kSvc0 = 0xd4000001;
constexpr uint32_t kRet = 0xd65f03c0;
constexpr uint32_t kBrX16 = 0xd61f0200;

bool traced(const std::string& name)
{
    static const std::vector<std::string> parts = [] {
        std::vector<std::string> out;
        const char* env = std::getenv("ORCHARD_TRACE");
        std::string s = env ? env : "";
        for (size_t at = 0; at < s.size();)
        {
            size_t comma = s.find(',', at);
            if (comma == std::string::npos) comma = s.size();
            if (comma > at) out.push_back(s.substr(at, comma - at));
            at = comma + 1;
        }
        return out;
    }();
    for (auto& p : parts)
        if (name.find(p) != std::string::npos) return true;
    return false;
}

std::string arg_text(Cpu& cpu, uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
    std::string out = buf;
    if (v < 0x100000000ull || !cpu.mem.is_mapped(v, 1)) return out;
    std::string s;
    for (int i = 0; i < 96 && cpu.mem.is_mapped(v + i, 1); ++i)
    {
        char ch = char(cpu.mem.read<uint8_t>(v + i));
        if (!ch) break;
        if (ch < 0x20 || ch > 0x7e) return out;
        s += ch;
    }
    return s.size() >= 2 ? out + " \"" + s + "\"" : out;
}
}

Hle::Hle(Runtime& rt) : rt_(rt)
{
    add_stub({"<return to host>", "", nullptr, false});
    register_libsystem(*this);
    objc::register_libobjc(*this);
}

GuestAddr Hle::add_stub(Stub s)
{
    GuestAddr addr = layout::kStubsBase + stubs_.size() * kStubSize;
    if (addr + kStubSize > layout::kStubsEnd) throw std::runtime_error("HLE stub area exhausted");
    stubs_.push_back(std::move(s));
    return addr;
}

GuestAddr Hle::resolve(const std::string& name, const std::string& lib)
{
    std::lock_guard g(lock_);
    if (auto it = resolved_.find(name); it != resolved_.end()) return it->second;

    GuestAddr addr = 0;
    if (auto d = data_.find(name); d != data_.end())
    {
        addr = d->second(rt_);
    }
    else if (auto f = fns_.find(name); f != fns_.end())
    {
        addr = add_stub({name, lib, f->second.fn, f->second.tail});
    }
    else
    {
        for (auto& r : data_resolvers_)
            if (r.claims(name) && (addr = r.create(name))) break;
        if (!addr) addr = add_stub({name, lib, nullptr, false});
    }
    resolved_[name] = addr;
    return addr;
}

bool Hle::claims(const std::string& name) const
{
    if (fns_.count(name) || data_.count(name)) return true;
    for (auto& r : data_resolvers_)
        if (r.claims(name)) return true;
    return false;
}

bool Hle::is_tail(GuestAddr stub) const
{
    std::lock_guard g(lock_);
    size_t i = (stub - layout::kStubsBase) / kStubSize;
    return i < stubs_.size() && stubs_[i].tail;
}

GuestAddr Hle::make_stub(const std::string& name, HleFn f, bool tail)
{
    std::lock_guard g(lock_);
    return add_stub({name, "", f, tail});
}

std::string Hle::stub_name(GuestAddr addr) const
{
    std::lock_guard g(lock_);
    size_t i = (addr - layout::kStubsBase) / kStubSize;
    return i < stubs_.size() ? stubs_[i].name : "<bad stub>";
}

uint32_t Hle::stub_code(GuestAddr addr) const
{
    if (((addr - layout::kStubsBase) % kStubSize) == 0) return kSvc0;
    std::lock_guard g(lock_);
    size_t i = (addr - layout::kStubsBase) / kStubSize;
    return i < stubs_.size() && stubs_[i].tail ? kBrX16 : kRet;
}

bool call_guarded(HleFn fn, Cpu& cpu, uintptr_t* fault_addr);

void Hle::dispatch(Cpu& cpu, GuestAddr stub)
{
    Stub* s;
    {
        std::lock_guard g(lock_);
        size_t i = (stub - layout::kStubsBase) / kStubSize;
        if (i >= stubs_.size() || (stub - layout::kStubsBase) % kStubSize)
        {
            cpu.stop("jump into the middle of the HLE stub area");
            return;
        }
        s = &stubs_[i];
        ++s->calls;
    }
    if (s->fn)
    {
        bool trace = traced(s->name);
        if (trace)
            std::fprintf(stderr, "[trace] %s(%s, %s, %s) from %s\n", s->name.c_str(), arg_text(cpu, cpu.arg(0)).c_str(),
                         arg_text(cpu, cpu.arg(1)).c_str(), arg_text(cpu, cpu.arg(2)).c_str(), cpu.rt.describe(cpu.lr()).c_str());
        uintptr_t fault = 0;
        cpu.hle_stack.push_back(&s->name);
        try
        {
            if (!call_guarded(s->fn, cpu, &fault))
            {
                uintptr_t base = uintptr_t(cpu.mem.base());
                char buf[128];
                if (fault >= base && fault < base + layout::kSize)
                    std::snprintf(buf, sizeof buf, "guest address 0x%llx", (unsigned long long)(fault - base));
                else
                    std::snprintf(buf, sizeof buf, "host address 0x%llx", (unsigned long long)fault);
                cpu.stop("fault in " + s->name + " touching " + buf + ", called from " + cpu.rt.describe(cpu.lr()));
            }
        }
        catch (const std::exception& e)
        {
            cpu.stop("exception in " + s->name + ": " + e.what() + ", called from " + cpu.rt.describe(cpu.lr()));
        }
        cpu.hle_stack.pop_back();
        if (trace) std::fprintf(stderr, "[trace]   %s -> 0x%llx\n", s->name.c_str(), (unsigned long long)cpu.x(0));
        return;
    }

    std::string caller = cpu.rt.describe(cpu.lr());
    if (!rt_.lenient)
    {
        cpu.stop("unimplemented " + s->name + " (" + s->lib + ") called from " + caller);
        return;
    }
    if (s->calls == 1) std::fprintf(stderr, "[hle] unimplemented %s called from %s, returning 0\n", s->name.c_str(), caller.c_str());
    cpu.ret(0);
    if (s->tail) cpu.set_x(16, ret_instruction());
}

Hle::Stats Hle::stats() const
{
    std::lock_guard g(lock_);
    Stats st;
    st.stubs = stubs_.size() - 1;
    for (size_t i = 1; i < stubs_.size(); ++i)
    {
        if (stubs_[i].fn)
            ++st.implemented;
        else if (stubs_[i].calls)
            st.unimplemented_called.push_back(stubs_[i].name);
    }
    return st;
}

}
