#include <cctype>
#include <cstring>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "loader/linker.h"
#include "objc/runtime.h"

namespace orchard::objc
{
namespace
{
GuestAddr ro_of(Memory& mem, GuestAddr cls)
{
    return mem.read<uint64_t>(cls + 32) & 0x00007ffffffffff8ull;
}

void tail_to(Cpu& c, GuestAddr imp)
{
    if (!imp) return;
    if (imp == c.rt.hle.ret_instruction())
    {
        c.set_x(0, 0);
        c.set_x(1, 0);
        c.set_d(0, 0);
        c.set_d(1, 0);
    }
    c.set_x(16, imp);
}

void msg_send(Cpu& c)
{
    tail_to(c, objc(c).resolve_send(c, c.arg(0), c.arg(1)));
}

void msg_send_super2(Cpu& c)
{
    GuestAddr sup = c.arg(0);
    Id receiver = c.mem.read<uint64_t>(sup);
    Class* cur = objc(c).class_at(c.mem.read<uint64_t>(sup + 8));
    c.set_x(0, receiver);
    if (!cur || !cur->super)
    {
        c.stop("objc_msgSendSuper2 with a class that has no superclass");
        return;
    }
    tail_to(c, objc(c).resolve_send(c, receiver, c.arg(1), cur->super));
}

Method* find_method(Class* cls, SEL s, bool inherited)
{
    for (Class* k = cls; k; k = inherited ? k->super : nullptr)
    {
        auto it = k->methods.find(s);
        if (it != k->methods.end()) return &it->second;
        if (!inherited) break;
    }
    return nullptr;
}

GuestAddr copy_array(Cpu& c, const std::vector<GuestAddr>& items, GuestAddr out_count)
{
    if (out_count) c.mem.write<uint32_t>(out_count, uint32_t(items.size()));
    if (items.empty()) return 0;
    GuestAddr arr = c.rt.heap.alloc((items.size() + 1) * 8);
    for (size_t i = 0; i < items.size(); ++i)
        c.mem.write<uint64_t>(arr + i * 8, items[i]);
    c.mem.write<uint64_t>(arr + items.size() * 8, 0);
    return arr;
}

size_t skip_type(const std::string& t, size_t i)
{
    while (i < t.size() && std::strchr("rnNoORVA", t[i]))
        ++i;
    if (i >= t.size()) return i;
    char ch = t[i++];
    switch (ch)
    {
    case '^': return skip_type(t, i);
    case '@':
        if (i < t.size() && t[i] == '?') return i + 1;
        if (i < t.size() && t[i] == '"')
        {
            size_t close = t.find('"', i + 1);
            return close == std::string::npos ? t.size() : close + 1;
        }
        return i;
    case 'b':
        while (i < t.size() && std::isdigit(uint8_t(t[i])))
            ++i;
        return i;
    case '{':
    case '(':
    case '[': {
        int depth = 1;
        while (i < t.size() && depth)
        {
            char c = t[i++];
            if (c == '{' || c == '(' || c == '[')
                ++depth;
            else if (c == '}' || c == ')' || c == ']')
                --depth;
            else if (c == '"')
            {
                size_t close = t.find('"', i);
                i = close == std::string::npos ? t.size() : close + 1;
            }
        }
        return i;
    }
    default: return i;
    }
}

std::string encoding_part(const std::string& types, unsigned index)
{
    size_t i = 0;
    for (unsigned n = 0; i < types.size(); ++n)
    {
        size_t start = i;
        i = skip_type(types, i);
        std::string part = types.substr(start, i - start);
        while (i < types.size() && (std::isdigit(uint8_t(types[i])) || types[i] == '-'))
            ++i;
        if (n == index) return part;
    }
    return {};
}

unsigned encoding_count(const std::string& types)
{
    unsigned n = 0;
    while (!encoding_part(types, n).empty())
        ++n;
    return n;
}

std::unordered_map<GuestAddr, GuestAddr>& block_imps()
{
    static std::unordered_map<GuestAddr, GuestAddr> m;
    return m;
}

}

void register_libobjc(Hle& h)
{
    h.tail_fn("_objc_msgSend", msg_send);
    h.tail_fn("_objc_msgSendSuper2", msg_send_super2);

    auto zero = [](Runtime& rt) -> GuestAddr {
        GuestAddr a = rt.mem.alloc_system(64, 16);
        std::memset(rt.mem.host(a), 0, 64);
        return a;
    };
    h.data("__objc_empty_cache", zero);
    h.data("_objc_ehtype_vtable", zero);
    h.data("_OBJC_EHTYPE_id", zero);

    h.fn("_objc_alloc", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "alloc")); });
    h.fn("_objc_allocWithZone", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "allocWithZone:", {0})); });
    h.fn("_objc_alloc_init", [](Cpu& c) {
        Id o = objc(c).send(c, c.arg(0), "alloc");
        c.ret(objc(c).send(c, o, "init"));
    });
    h.fn("_objc_opt_new", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "new")); });
    h.fn("_objc_opt_class", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "class")); });
    h.fn("_objc_opt_self", [](Cpu& c) {});
    h.fn("_objc_opt_isKindOfClass", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "isKindOfClass:", {c.arg(1)}) & 1); });
    h.fn("_objc_opt_respondsToSelector", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "respondsToSelector:", {c.arg(1)}) & 1); });

    h.fn("_objc_retain", [](Cpu& c) { c.ret(objc(c).retain_entry(c, c.arg(0))); });
    h.fn("_objc_release", [](Cpu& c) { objc(c).release_entry(c, c.arg(0)); });
    h.fn("_objc_autorelease", [](Cpu& c) { objc(c).autorelease(c, c.arg(0)); });
    h.fn("_objc_autoreleaseReturnValue", [](Cpu& c) { objc(c).autorelease(c, c.arg(0)); });
    h.fn("_objc_retainAutoreleasedReturnValue", [](Cpu& c) { c.ret(objc(c).retain_entry(c, c.arg(0))); });
    h.fn("_objc_unsafeClaimAutoreleasedReturnValue", [](Cpu& c) {});
    h.fn("_objc_retainAutorelease", [](Cpu& c) { objc(c).autorelease(c, objc(c).retain_entry(c, c.arg(0))); });
    h.fn("_objc_retainAutoreleaseReturnValue", [](Cpu& c) { objc(c).autorelease(c, objc(c).retain_entry(c, c.arg(0))); });
    h.fn("_objc_retainBlock", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "copy")); });
    h.fn("_objc_storeStrong", [](Cpu& c) {
        GuestAddr loc = c.arg(0);
        Id old = c.mem.read<uint64_t>(loc);
        Id obj = objc(c).retain_entry(c, c.arg(1));
        c.mem.write<uint64_t>(loc, obj);
        objc(c).release_entry(c, old);
    });
    h.fn("_objc_autoreleasePoolPush", [](Cpu& c) { c.ret(objc(c).autorelease_push(c)); });
    h.fn("_objc_autoreleasePoolPop", [](Cpu& c) { objc(c).autorelease_pop(c, c.arg(0)); });

    h.fn("_objc_initWeak", [](Cpu& c) {
        objc(c).weak_store(c.arg(0), c.arg(1));
        c.ret(c.arg(1));
    });
    h.fn("_objc_storeWeak", [](Cpu& c) {
        objc(c).weak_store(c.arg(0), c.arg(1));
        c.ret(c.arg(1));
    });
    h.fn("_objc_loadWeakRetained", [](Cpu& c) { c.ret(objc(c).retain_entry(c, objc(c).weak_load(c.arg(0)))); });
    h.fn("_objc_destroyWeak", [](Cpu& c) {
        objc(c).weak_unregister(c.arg(0));
        c.mem.write<uint64_t>(c.arg(0), 0);
    });
    h.fn("_objc_copyWeak", [](Cpu& c) { objc(c).weak_store(c.arg(0), objc(c).weak_load(c.arg(1))); });
    h.fn("_objc_moveWeak", [](Cpu& c) {
        objc(c).weak_store(c.arg(0), objc(c).weak_load(c.arg(1)));
        objc(c).weak_unregister(c.arg(1));
        c.mem.write<uint64_t>(c.arg(1), 0);
    });

    h.fn("_objc_getClass", [](Cpu& c) {
        Class* k = objc(c).class_named(c.mem.read_cstr(c.arg(0)));
        c.ret(k ? k->addr : 0);
    });
    h.fn("_objc_lookUpClass", [](Cpu& c) {
        Class* k = objc(c).class_named(c.mem.read_cstr(c.arg(0)));
        c.ret(k ? k->addr : 0);
    });
    h.fn("_objc_getMetaClass", [](Cpu& c) {
        Class* k = objc(c).class_named(c.mem.read_cstr(c.arg(0)));
        c.ret(k ? k->meta->addr : 0);
    });
    h.fn("_objc_getClassList", [](Cpu& c) { c.ret(0); });
    h.fn("_objc_setHook_getClass", [](Cpu& c) {
        static GuestAddr fallback = c.rt.hle.make_stub("objc_getClass hook fallback", [](Cpu& k) {
            Class* cls = objc(k).class_named(k.mem.read_cstr(k.arg(0)));
            k.mem.write<uint64_t>(k.arg(1), cls ? cls->addr : 0);
            k.ret(cls != nullptr);
        });
        c.mem.write<uint64_t>(c.arg(1), fallback);
    });

    h.fn("_objc_setHook_lazyClassNamer", [](Cpu& c) {
        static GuestAddr none = c.rt.hle.make_stub("default lazyClassNamer", [](Cpu& k) { k.ret(0); });
        c.mem.write<uint64_t>(c.arg(1), none);
    });
    h.fn("_objc_setHook_getImageName", [](Cpu& c) {
        static GuestAddr none = c.rt.hle.make_stub("default getImageName", [](Cpu& k) { k.ret(0); });
        c.mem.write<uint64_t>(c.arg(1), none);
    });
    h.fn("_objc_addLoadImageFunc", [](Cpu& c) {
        GuestAddr fn = c.arg(0);
        for (auto& img : c.rt.linker->images())
        {
            if (c.stopped()) return;
            if (!img->base) continue;
            c.call(fn, {img->base});
        }
    });
    h.fn("__objc_realizeClassFromSwift", [](Cpu& c) {
        objc(c).realize(c.arg(0));
        c.ret(c.arg(0));
    });

    h.fn("_objc_getProperty", [](Cpu& c) {
        Id v = c.mem.read<uint64_t>(c.arg(0) + c.arg(2));
        c.ret(objc(c).autorelease(c, objc(c).retain(v)));
    });
    h.fn("_objc_setProperty", [](Cpu& c) {
        GuestAddr slot = c.arg(0) + c.arg(2);
        Id v = c.arg(3);
        uint64_t copy = c.arg(5) & 0xff;
        v = copy == 1 ? objc(c).send(c, v, "copy") : copy == 2 ? objc(c).send(c, v, "mutableCopy") : objc(c).retain(v);
        Id old = c.mem.read<uint64_t>(slot);
        c.mem.write<uint64_t>(slot, v);
        objc(c).release(c, old);
    });
    auto set_prop = [](Cpu& c, bool copy) {
        GuestAddr slot = c.arg(0) + c.arg(3);
        Id v = copy ? objc(c).send(c, c.arg(2), "copy") : objc(c).retain(c.arg(2));
        Id old = c.mem.read<uint64_t>(slot);
        c.mem.write<uint64_t>(slot, v);
        objc(c).release(c, old);
    };
    static decltype(set_prop) s_set_prop = set_prop;
    h.fn("_objc_setProperty_atomic", [](Cpu& c) { s_set_prop(c, false); });
    h.fn("_objc_setProperty_atomic_copy", [](Cpu& c) { s_set_prop(c, true); });
    h.fn("_objc_setProperty_nonatomic_copy", [](Cpu& c) { s_set_prop(c, true); });

    h.fn("_objc_sync_enter", [](Cpu& c) { c.ret(0); });
    h.fn("_objc_sync_exit", [](Cpu& c) { c.ret(0); });
    h.fn("_objc_enumerationMutation", [](Cpu& c) { c.stop("collection mutated while being enumerated"); });

    h.fn("_objc_exception_throw", [](Cpu& c) {
        Id e = c.arg(0);
        std::string name = foundation::to_utf8(c, objc(c).send(c, e, "name"));
        std::string reason = foundation::to_utf8(c, objc(c).send(c, e, "reason"));
        c.stop("Objective-C exception " + name + ": " + reason);
    });
    h.fn("_objc_exception_rethrow", [](Cpu& c) { c.stop("objc_exception_rethrow"); });
    h.fn("_objc_terminate", [](Cpu& c) { c.stop("objc_terminate"); });

    h.fn("_objc_setAssociatedObject", [](Cpu& c) { objc(c).set_associated(c, c.arg(0), c.arg(1), c.arg(2), c.arg(3)); });
    h.fn("_objc_getAssociatedObject", [](Cpu& c) { c.ret(objc(c).get_associated(c.arg(0), c.arg(1))); });

    h.fn("_objc_allocateClassPair", [](Cpu& c) {
        Class* sup = objc(c).class_at(c.arg(0));
        Class* k = objc(c).allocate_class_pair(sup, c.mem.read_cstr(c.arg(1)), c.arg(2));
        c.ret(k ? k->addr : 0);
    });
    h.fn("_objc_registerClassPair", [](Cpu& c) {
        if (Class* k = objc(c).class_at(c.arg(0))) objc(c).register_class_pair(k);
    });

    h.fn("_object_getClass", [](Cpu& c) {
        Id o = c.arg(0);
        c.ret(o && !(o >> 63) ? c.mem.read<uint64_t>(o) & kIsaMask : 0);
    });
    h.fn("_object_setClass", [](Cpu& c) {
        Id o = c.arg(0);
        if (!o) return;
        GuestAddr old = c.mem.read<uint64_t>(o);
        c.mem.write<uint64_t>(o, c.arg(1));
        c.ret(old);
    });
    h.fn("_object_isClass", [](Cpu& c) { c.ret(objc(c).class_at(c.arg(0)) != nullptr && c.arg(0) != 0); });
    h.fn("_object_getIvar", [](Cpu& c) {
        if (!c.arg(0) || !c.arg(1)) return c.ret(0);
        int32_t off = c.mem.read<int32_t>(c.mem.read<uint64_t>(c.arg(1)));
        c.ret(c.mem.read<uint64_t>(c.arg(0) + off));
    });

    h.fn("_class_getName", [](Cpu& c) {
        if (!c.arg(0)) return c.ret(c.mem.alloc_cstr_region("nil"));
        c.ret(c.mem.read<uint64_t>(ro_of(c.mem, c.arg(0)) + 24));
    });
    h.fn("_class_getSuperclass", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        c.ret(k && k->super ? k->super->addr : 0);
    });
    h.fn("_class_isMetaClass", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        c.ret(k && k->is_meta);
    });
    h.fn("_class_getInstanceSize", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        c.ret(k ? k->instance_size : 0);
    });
    h.fn("_class_getInstanceMethod", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        Method* m = k ? find_method(k, c.arg(1), true) : nullptr;
        c.ret(m ? objc(c).method_handle(k, *m) : 0);
    });
    h.fn("_class_getClassMethod", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        if (k && !k->is_meta) k = k->meta;
        Method* m = k ? find_method(k, c.arg(1), true) : nullptr;
        c.ret(m ? objc(c).method_handle(k, *m) : 0);
    });
    h.fn("_class_getMethodImplementation", [](Cpu& c) {
        static GuestAddr forward = c.rt.hle.make_stub(
            "_objc_msgForward", [](Cpu& k) { k.stop("_objc_msgForward: -" + objc(k).sel_name(k.arg(1)) + " has no implementation"); });
        GuestAddr imp = objc(c).lookup(objc(c).class_at(c.arg(0)), c.arg(1));
        c.ret(imp ? imp : forward);
    });
    h.fn("_class_addMethod", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        if (!k || k->methods.count(c.arg(1))) return c.ret(0);
        objc(c).set_method_imp(k, c.arg(1), c.arg(2));
        k->methods[c.arg(1)].types = c.arg(3);
        c.ret(1);
    });
    h.fn("_class_replaceMethod", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        if (!k) return c.ret(0);
        auto it = k->methods.find(c.arg(1));
        GuestAddr old = it == k->methods.end() ? 0 : it->second.imp;
        objc(c).set_method_imp(k, c.arg(1), c.arg(2));
        if (!old) k->methods[c.arg(1)].types = c.arg(3);
        c.ret(old);
    });
    h.fn("_class_copyMethodList", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        std::vector<GuestAddr> out;
        if (k)
            for (auto& [s, m] : k->methods)
                out.push_back(objc(c).method_handle(k, m));
        c.ret(copy_array(c, out, c.arg(1)));
    });
    h.fn("_class_copyIvarList", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        std::vector<GuestAddr> out;
        if (k)
            for (auto& iv : k->ivars)
                out.push_back(objc(c).ivar_handle(k, iv));
        c.ret(copy_array(c, out, c.arg(1)));
    });
    h.fn("_class_copyPropertyList", [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        std::vector<GuestAddr> out;
        if (k)
        {
            for (auto& p : k->properties)
            {
                if (!p.guest)
                {
                    p.guest = c.mem.alloc_system(16, 8);
                    c.mem.write<uint64_t>(p.guest, c.mem.alloc_cstr_region(p.name));
                    c.mem.write<uint64_t>(p.guest + 8, c.mem.alloc_cstr_region(p.attributes));
                }
                out.push_back(p.guest);
            }
        }
        c.ret(copy_array(c, out, c.arg(1)));
    });
    h.fn("_class_getInstanceVariable", [](Cpu& c) {
        std::string name = c.mem.read_cstr(c.arg(1));
        for (Class* k = objc(c).class_at(c.arg(0)); k; k = k->super)
            for (auto& iv : k->ivars)
                if (iv.name == name) return c.ret(objc(c).ivar_handle(k, iv));
        c.ret(0);
    });

    h.fn("_method_getName", [](Cpu& c) { c.ret(c.arg(0) ? c.mem.read<uint64_t>(c.arg(0)) : 0); });
    h.fn("_method_getImplementation", [](Cpu& c) { c.ret(c.arg(0) ? c.mem.read<uint64_t>(c.arg(0) + 16) : 0); });
    h.fn("_method_getTypeEncoding", [](Cpu& c) { c.ret(c.arg(0) ? c.mem.read<uint64_t>(c.arg(0) + 8) : 0); });
    h.fn("_method_setImplementation", [](Cpu& c) {
        auto [k, s] = objc(c).method_from_handle(c.arg(0));
        GuestAddr old = c.mem.read<uint64_t>(c.arg(0) + 16);
        if (k) objc(c).set_method_imp(k, s, c.arg(1));
        c.ret(old);
    });
    h.fn("_method_exchangeImplementations", [](Cpu& c) {
        auto [k1, s1] = objc(c).method_from_handle(c.arg(0));
        auto [k2, s2] = objc(c).method_from_handle(c.arg(1));
        if (!k1 || !k2) return;
        GuestAddr i1 = c.mem.read<uint64_t>(c.arg(0) + 16), i2 = c.mem.read<uint64_t>(c.arg(1) + 16);
        objc(c).set_method_imp(k1, s1, i2);
        objc(c).set_method_imp(k2, s2, i1);
    });
    auto types_of = [](Cpu& c, GuestAddr m) {
        GuestAddr t = m ? c.mem.read<uint64_t>(m + 8) : 0;
        return t ? c.mem.read_cstr(t) : std::string();
    };
    static decltype(types_of) s_types = types_of;
    h.fn("_method_getNumberOfArguments", [](Cpu& c) {
        unsigned n = encoding_count(s_types(c, c.arg(0)));
        c.ret(n ? n - 1 : 0);
    });
    h.fn("_method_copyReturnType", [](Cpu& c) {
        std::string t = encoding_part(s_types(c, c.arg(0)), 0);
        GuestAddr p = c.rt.heap.alloc(t.size() + 1);
        c.mem.write_bytes(p, t.c_str(), t.size() + 1);
        c.ret(p);
    });
    auto write_part = [](Cpu& c, const std::string& t, GuestAddr dst, uint64_t len) {
        if (!dst || !len) return;
        size_t n = std::min<size_t>(t.size(), len - 1);
        c.mem.write_bytes(dst, t.data(), n);
        c.mem.write<char>(dst + n, 0);
    };
    static decltype(write_part) s_write = write_part;
    h.fn("_method_getReturnType", [](Cpu& c) { s_write(c, encoding_part(s_types(c, c.arg(0)), 0), c.arg(1), c.arg(2)); });
    h.fn("_method_getArgumentType",
         [](Cpu& c) { s_write(c, encoding_part(s_types(c, c.arg(0)), unsigned(c.arg(1)) + 1), c.arg(2), c.arg(3)); });

    h.fn("_ivar_getOffset", [](Cpu& c) {
        GuestAddr var = c.arg(0) ? c.mem.read<uint64_t>(c.arg(0)) : 0;
        c.ret(var ? uint64_t(int64_t(c.mem.read<int32_t>(var))) : 0);
    });
    h.fn("_ivar_getTypeEncoding", [](Cpu& c) { c.ret(c.arg(0) ? c.mem.read<uint64_t>(c.arg(0) + 16) : 0); });
    h.fn("_property_getName", [](Cpu& c) { c.ret(c.mem.read<uint64_t>(c.arg(0))); });
    h.fn("_property_getAttributes", [](Cpu& c) { c.ret(c.mem.read<uint64_t>(c.arg(0) + 8)); });
    h.fn("_protocol_getMethodDescription", [](Cpu& c) {
        c.set_x(0, 0);
        c.set_x(1, 0);
    });

    h.fn("_sel_getName", [](Cpu& c) {});
    h.fn("_sel_registerName", [](Cpu& c) { c.ret(objc(c).sel(c.mem.read_cstr(c.arg(0)))); });

    h.fn("_imp_implementationWithBlock", [](Cpu& c) {
        Id block = objc(c).send(c, c.arg(0), "copy");
        GuestAddr stub = c.rt.hle.make_stub(
            "imp_implementationWithBlock trampoline",
            [](Cpu& k) {
                GuestAddr blk = block_imps()[k.pc() - 4];
                k.set_x(1, k.arg(0));
                k.set_x(0, blk);
                k.set_x(16, k.mem.read<uint64_t>(blk + 16));
            },
            true);
        block_imps()[stub] = block;
        c.ret(stub);
    });
}

}
