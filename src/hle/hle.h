#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <deque>
#include <functional>
#include <vector>

#include "core/memory.h"

namespace orchard
{
struct Runtime;
class Cpu;

using HleFn = void (*)(Cpu&);
using HleDataInit = GuestAddr (*)(Runtime&);

class Hle
{
public:
    explicit Hle(Runtime& rt);

    void fn(const std::string& name, HleFn f) { fns_[name] = {f, false}; }
    void tail_fn(const std::string& name, HleFn f) { fns_[name] = {f, true}; }
    void data(const std::string& name, HleDataInit init) { data_[name] = init; }
    void data_resolver(std::function<bool(const std::string&)> claims, std::function<GuestAddr(const std::string&)> create)
    {
        data_resolvers_.push_back({std::move(claims), std::move(create)});
    }

    bool claims(const std::string& name) const;
    bool is_tail(GuestAddr stub) const;

    GuestAddr resolve(const std::string& name, const std::string& lib);

    GuestAddr make_stub(const std::string& name, HleFn f, bool tail = false);

    GuestAddr return_trampoline() const { return layout::kStubsBase; }
    GuestAddr ret_instruction() const { return layout::kStubsBase + 4; }
    bool is_stub(GuestAddr addr) const { return addr >= layout::kStubsBase && addr < layout::kStubsEnd; }
    std::string stub_name(GuestAddr addr) const;
    uint32_t stub_code(GuestAddr addr) const;

    void dispatch(Cpu& cpu, GuestAddr stub);

    struct Stats
    {
        size_t stubs = 0;
        size_t implemented = 0;
        std::vector<std::string> unimplemented_called;
    };
    Stats stats() const;

private:
    struct Stub
    {
        std::string name;
        std::string lib;
        HleFn fn = nullptr;
        bool tail = false;
        uint64_t calls = 0;
    };
    struct FnEntry
    {
        HleFn fn;
        bool tail;
    };

    GuestAddr add_stub(Stub s);
    static constexpr uint64_t kStubSize = 8;

    Runtime& rt_;
    std::unordered_map<std::string, FnEntry> fns_;
    struct DataResolver
    {
        std::function<bool(const std::string&)> claims;
        std::function<GuestAddr(const std::string&)> create;
    };
    std::vector<DataResolver> data_resolvers_;
    std::unordered_map<std::string, HleDataInit> data_;
    std::unordered_map<std::string, GuestAddr> resolved_;
    std::deque<Stub> stubs_;
    mutable std::recursive_mutex lock_;
};

void register_libsystem(Hle& hle);
namespace objc
{
void register_libobjc(Hle& hle);
}

}
