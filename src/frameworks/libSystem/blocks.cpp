#include "frameworks/libSystem/blocks.h"

#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <vector>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard
{
namespace
{
constexpr uint32_t BLOCK_DEALLOCATING = 0x0001;
constexpr uint32_t BLOCK_REFCOUNT_MASK = 0xfffe;
constexpr uint32_t BLOCK_NEEDS_FREE = 1u << 24;
constexpr uint32_t BLOCK_HAS_COPY_DISPOSE = 1u << 25;
constexpr uint32_t BLOCK_IS_GLOBAL = 1u << 28;
constexpr uint32_t BLOCK_BYREF_LAYOUT_EXTENDED = 1u << 28;

constexpr int BLOCK_FIELD_IS_OBJECT = 3, BLOCK_FIELD_IS_BLOCK = 7, BLOCK_FIELD_IS_BYREF = 8, BLOCK_FIELD_IS_WEAK = 16,
              BLOCK_BYREF_CALLER = 128;

constexpr uint64_t B_FLAGS = 8, B_INVOKE = 16, B_DESC = 24;
constexpr uint64_t R_FORWARDING = 8, R_FLAGS = 16, R_SIZE = 20, R_KEEP = 24, R_DESTROY = 32, R_LAYOUT = 40;

long* flags_ptr(Cpu& c, GuestAddr obj, uint64_t off)
{
    return reinterpret_cast<long*>(c.mem.host(obj + off));
}

uint32_t adjust_refcount(Cpu& c, GuestAddr obj, uint64_t off, int delta)
{
    long* p = flags_ptr(c, obj, off);
    for (;;)
    {
        long old = *p;
        uint32_t f = uint32_t(old);
        uint32_t rc = f & BLOCK_REFCOUNT_MASK;
        if (delta < 0 && rc == 0) return f;
        if (delta > 0 && rc == BLOCK_REFCOUNT_MASK) return f;
        uint32_t nf = (f & ~BLOCK_REFCOUNT_MASK) | ((rc + delta) & BLOCK_REFCOUNT_MASK);
        if (_InterlockedCompareExchange(p, long(nf), old) == old) return nf;
    }
}

GuestAddr byref_copy(Cpu& c, GuestAddr src)
{
    GuestAddr fwd = c.mem.read<uint64_t>(src + R_FORWARDING);
    uint32_t fwd_flags = c.mem.read<uint32_t>(fwd + R_FLAGS);
    if ((fwd_flags & BLOCK_REFCOUNT_MASK) == 0)
    {
        uint32_t flags = c.mem.read<uint32_t>(src + R_FLAGS);
        uint32_t size = c.mem.read<uint32_t>(src + R_SIZE);
        GuestAddr copy = c.rt.heap.alloc(size);
        c.mem.write<uint64_t>(copy, 0);
        c.mem.write<uint64_t>(copy + R_FORWARDING, copy);
        c.mem.write<uint32_t>(copy + R_FLAGS, flags | BLOCK_NEEDS_FREE | 4);
        c.mem.write<uint32_t>(copy + R_SIZE, size);
        c.mem.write<uint64_t>(src + R_FORWARDING, copy);
        if (flags & BLOCK_HAS_COPY_DISPOSE)
        {
            GuestAddr keep = c.mem.read<uint64_t>(src + R_KEEP);
            c.mem.write<uint64_t>(copy + R_KEEP, keep);
            c.mem.write<uint64_t>(copy + R_DESTROY, c.mem.read<uint64_t>(src + R_DESTROY));
            if (flags & BLOCK_BYREF_LAYOUT_EXTENDED) c.mem.write<uint64_t>(copy + R_LAYOUT, c.mem.read<uint64_t>(src + R_LAYOUT));
            c.call(keep, {copy, src});
        }
        else if (size > 24)
        {
            std::memmove(c.mem.host(copy + 24), c.mem.host(src + 24), size - 24);
        }
        return copy;
    }
    if (fwd_flags & BLOCK_NEEDS_FREE) adjust_refcount(c, fwd, R_FLAGS, 2);
    return fwd;
}

void byref_release(Cpu& c, GuestAddr byref)
{
    byref = c.mem.read<uint64_t>(byref + R_FORWARDING);
    uint32_t flags = c.mem.read<uint32_t>(byref + R_FLAGS);
    if (!(flags & BLOCK_NEEDS_FREE)) return;
    if ((adjust_refcount(c, byref, R_FLAGS, -2) & BLOCK_REFCOUNT_MASK) != 0) return;
    if (flags & BLOCK_HAS_COPY_DISPOSE) c.call(c.mem.read<uint64_t>(byref + R_DESTROY), {byref});
    c.rt.heap.free(byref);
}

}

GuestAddr block_copy(Cpu& c, GuestAddr block)
{
    if (!block) return 0;
    uint32_t flags = c.mem.read<uint32_t>(block + B_FLAGS);
    if (flags & BLOCK_NEEDS_FREE)
    {
        adjust_refcount(c, block, B_FLAGS, 2);
        return block;
    }
    if (flags & BLOCK_IS_GLOBAL) return block;

    GuestAddr desc = c.mem.read<uint64_t>(block + B_DESC);
    if (!desc || !c.mem.is_mapped(desc, 16))
    {
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            auto* k = c.rt.objc->class_at(c.mem.read<uint64_t>(block));
            std::fprintf(stderr, "[blocks] copy of a block without a descriptor (isa %s, flags 0x%x) from %s, used as is\n",
                         k ? k->name.c_str() : "?", flags, c.rt.describe(c.lr()).c_str());
        }
        return block;
    }
    uint64_t size = c.mem.read<uint64_t>(desc + 8);
    GuestAddr copy = c.rt.heap.alloc(size);
    std::memcpy(c.mem.host(copy), c.mem.host(block), size);
    c.mem.write<uint32_t>(copy + B_FLAGS, (flags & ~(BLOCK_REFCOUNT_MASK | BLOCK_DEALLOCATING)) | BLOCK_NEEDS_FREE | 2);
    c.mem.write<uint64_t>(copy, c.rt.objc->host_class("__NSMallocBlock__")->addr);
    if (flags & BLOCK_HAS_COPY_DISPOSE) c.call(c.mem.read<uint64_t>(desc + 16), {copy, block});
    return copy;
}

void block_release(Cpu& c, GuestAddr block)
{
    if (!block) return;
    uint32_t flags = c.mem.read<uint32_t>(block + B_FLAGS);
    if (!(flags & BLOCK_NEEDS_FREE)) return;
    if ((adjust_refcount(c, block, B_FLAGS, -2) & BLOCK_REFCOUNT_MASK) != 0) return;
    if (flags & BLOCK_HAS_COPY_DISPOSE) c.call(c.mem.read<uint64_t>(c.mem.read<uint64_t>(block + B_DESC) + 24), {block});
    c.rt.heap.free(block);
}

uint64_t call_block(Cpu& c, GuestAddr block, std::initializer_list<uint64_t> args)
{
    std::vector<uint64_t> all{block};
    all.insert(all.end(), args.begin(), args.end());
    GuestAddr invoke = c.mem.read<uint64_t>(block + B_INVOKE);
    switch (all.size())
    {
    case 1: return c.call(invoke, {all[0]});
    case 2: return c.call(invoke, {all[0], all[1]});
    case 3: return c.call(invoke, {all[0], all[1], all[2]});
    case 4: return c.call(invoke, {all[0], all[1], all[2], all[3]});
    default: return c.call(invoke, {all[0], all[1], all[2], all[3], all[4]});
    }
}

void register_blocks(Hle& h)
{
    h.fn("__Block_copy", [](Cpu& c) { c.ret(block_copy(c, c.arg(0))); });
    h.fn("__Block_release", [](Cpu& c) { block_release(c, c.arg(0)); });
    h.fn("__Block_object_assign", [](Cpu& c) {
        GuestAddr dest = c.arg(0), obj = c.arg(1);
        int flags = int(c.arg(2));
        GuestAddr value = obj;
        if (flags & BLOCK_BYREF_CALLER)
        {
            value = obj;
        }
        else if ((flags & BLOCK_FIELD_IS_BYREF) == BLOCK_FIELD_IS_BYREF)
        {
            value = byref_copy(c, obj);
        }
        else if ((flags & BLOCK_FIELD_IS_BLOCK) == BLOCK_FIELD_IS_BLOCK)
        {
            value = block_copy(c, obj);
        }
        else if ((flags & BLOCK_FIELD_IS_OBJECT) == BLOCK_FIELD_IS_OBJECT)
        {
            c.rt.objc->retain(obj);
        }
        c.mem.write<uint64_t>(dest, value);
    });
    h.fn("__Block_object_dispose", [](Cpu& c) {
        GuestAddr obj = c.arg(0);
        int flags = int(c.arg(1));
        if (flags & BLOCK_BYREF_CALLER) return;
        if ((flags & BLOCK_FIELD_IS_BYREF) == BLOCK_FIELD_IS_BYREF)
            byref_release(c, obj);
        else if ((flags & BLOCK_FIELD_IS_BLOCK) == BLOCK_FIELD_IS_BLOCK)
            block_release(c, obj);
        else if ((flags & BLOCK_FIELD_IS_OBJECT) == BLOCK_FIELD_IS_OBJECT)
            c.rt.objc->release(c, obj);
    });

    h.data("__NSConcreteGlobalBlock", [](Runtime& rt) { return rt.objc->host_class("__NSGlobalBlock__")->addr; });
    h.data("__NSConcreteStackBlock", [](Runtime& rt) { return rt.objc->host_class("__NSStackBlock__")->addr; });
    h.data("__NSConcreteMallocBlock", [](Runtime& rt) { return rt.objc->host_class("__NSMallocBlock__")->addr; });
}

}
