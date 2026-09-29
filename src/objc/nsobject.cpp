#include <cctype>
#include <cstdio>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "objc/runtime.h"

namespace orchard::objc
{
namespace
{
Class* self_class(Cpu& c)
{
    return objc(c).class_at(c.arg(0));
}

void perform(Cpu& c, int nargs)
{
    Id self = c.arg(0);
    SEL s = c.arg(2);
    if (nargs == 0) c.ret(objc(c).send(c, self, s));
    if (nargs == 1) c.ret(objc(c).send(c, self, s, {c.arg(3)}));
    if (nargs == 2) c.ret(objc(c).send(c, self, s, {c.arg(3), c.arg(4)}));
}

bool conforms(Cpu& c, Class* cls, GuestAddr proto)
{
    if (!proto) return false;
    std::string want = c.mem.read_cstr(c.mem.read<uint64_t>(proto + 8));
    for (Class* k = cls; k; k = k->super)
    {
        for (GuestAddr p : k->protocols)
        {
            if (p == proto || c.mem.read_cstr(c.mem.read<uint64_t>(p + 8)) == want) return true;
        }
    }
    return false;
}

}

void register_nsobject(ObjcRuntime& o)
{
    o.class_method("NSObject", "alloc", [](Cpu& c) { c.ret(objc(c).alloc_instance(self_class(c))); });
    o.class_method("NSObject", "allocWithZone:", [](Cpu& c) { c.ret(objc(c).alloc_instance(self_class(c))); });
    o.class_method("NSObject", "new", [](Cpu& c) {
        Id obj = objc(c).send(c, c.arg(0), "alloc");
        c.ret(objc(c).send(c, obj, "init"));
    });
    o.class_method("NSObject", "class", [](Cpu& c) {});
    o.class_method("NSObject", "superclass", [](Cpu& c) {
        Class* k = self_class(c);
        c.ret(k && k->super ? k->super->addr : 0);
    });
    o.class_method("NSObject", "load", [](Cpu& c) {});
    o.class_method("NSObject", "initialize", [](Cpu& c) {});
    o.class_method("NSObject", "resolveInstanceMethod:", [](Cpu& c) { c.ret(0); });
    o.class_method("NSObject", "resolveClassMethod:", [](Cpu& c) { c.ret(0); });
    o.class_method("NSObject", "instancesRespondToSelector:", [](Cpu& c) { c.ret(objc(c).lookup(self_class(c), c.arg(2)) != 0); });
    o.class_method("NSObject", "isSubclassOfClass:", [](Cpu& c) { c.ret(objc(c).is_subclass(self_class(c), objc(c).class_at(c.arg(2)))); });
    o.class_method("NSObject", "instanceMethodForSelector:", [](Cpu& c) { c.ret(objc(c).lookup(self_class(c), c.arg(2))); });
    o.class_method("NSObject", "conformsToProtocol:", [](Cpu& c) { c.ret(conforms(c, self_class(c), c.arg(2))); });
    o.class_method("NSObject", "description", [](Cpu& c) { c.ret(foundation::string_autoreleased(c, self_class(c)->name)); });

    o.method("NSObject", "init", [](Cpu& c) {});
    o.method("NSObject", "self", [](Cpu& c) {});
    o.method("NSObject", "class", [](Cpu& c) {
        Class* k = objc(c).class_of(c.arg(0));
        c.ret(k && !k->is_meta ? k->addr : c.arg(0));
    });
    o.method("NSObject", "superclass", [](Cpu& c) {
        Class* k = objc(c).class_of(c.arg(0));
        c.ret(k && k->super ? k->super->addr : 0);
    });
    o.method("NSObject", "retain", [](Cpu& c) { objc(c).retain(c.arg(0)); });
    o.method("NSObject", "release", [](Cpu& c) { objc(c).release(c, c.arg(0)); });
    o.method("NSObject", "autorelease", [](Cpu& c) { objc(c).autorelease(c, c.arg(0)); });
    o.method("NSObject", "retainCount", [](Cpu& c) { c.ret(objc(c).retain_count(c.arg(0))); });
    o.method("NSObject", "dealloc", [](Cpu& c) { objc(c).dispose(c.arg(0)); });
    o.method("NSObject", "_tryRetain", [](Cpu& c) {
        objc(c).retain(c.arg(0));
        c.ret(1);
    });
    o.method("NSObject", "_isDeallocating", [](Cpu& c) { c.ret(0); });
    o.method("NSObject", "allowsWeakReference", [](Cpu& c) { c.ret(1); });
    o.method("NSObject", "retainWeakReference", [](Cpu& c) { c.ret(1); });
    o.method("NSObject", "zone", [](Cpu& c) { c.ret(0); });
    o.method("NSObject", "isProxy", [](Cpu& c) { c.ret(0); });
    o.method("NSObject",
             "isKindOfClass:", [](Cpu& c) { c.ret(objc(c).is_subclass(objc(c).class_of(c.arg(0)), objc(c).class_at(c.arg(2)))); });
    o.method("NSObject", "isMemberOfClass:", [](Cpu& c) {
        Class* k = objc(c).class_of(c.arg(0));
        c.ret(k && k->addr == (c.arg(2) & kIsaMask));
    });
    o.method("NSObject", "respondsToSelector:", [](Cpu& c) { c.ret(objc(c).lookup(objc(c).class_of(c.arg(0)), c.arg(2)) != 0); });
    o.method("NSObject", "conformsToProtocol:", [](Cpu& c) { c.ret(conforms(c, objc(c).class_of(c.arg(0)), c.arg(2))); });
    o.method("NSObject", "methodForSelector:", [](Cpu& c) { c.ret(objc(c).lookup(objc(c).class_of(c.arg(0)), c.arg(2))); });
    o.method("NSObject", "isEqual:", [](Cpu& c) { c.ret(c.arg(0) == c.arg(2)); });
    o.method("NSObject", "hash", [](Cpu& c) {});
    o.method("NSObject", "copy", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "copyWithZone:", {0})); });
    o.method("NSObject", "mutableCopy", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "mutableCopyWithZone:", {0})); });
    o.method("NSObject", "forwardingTargetForSelector:", [](Cpu& c) { c.ret(0); });
    o.method("NSObject", "performSelector:", [](Cpu& c) { perform(c, 0); });
    o.method("NSObject", "performSelector:withObject:", [](Cpu& c) { perform(c, 1); });
    o.method("NSObject", "performSelector:withObject:withObject:", [](Cpu& c) { perform(c, 2); });
    o.method("NSObject", "description", [](Cpu& c) {
        Class* k = objc(c).class_of(c.arg(0));
        char buf[96];
        std::snprintf(buf, sizeof buf, "<%s: 0x%llx>", k ? k->name.c_str() : "?", (unsigned long long)c.arg(0));
        c.ret(foundation::string_autoreleased(c, buf));
    });
    o.method("NSObject", "debugDescription", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "description")); });
    o.method("NSObject", "valueForKey:", [](Cpu& c) {
        std::string key = foundation::to_utf8(c, c.arg(2));
        Class* k = objc(c).class_of(c.arg(0));
        for (std::string sel :
             {key, "is" + std::string(1, char(std::toupper(key.empty() ? 'x' : key[0]))) + key.substr(key.empty() ? 0 : 1)})
            if (objc(c).responds(k, objc(c).sel(sel))) return c.ret(objc(c).send(c, c.arg(0), sel));
        c.ret(0);
    });
    o.method("NSObject", "setValue:forKey:", [](Cpu& c) {
        std::string key = foundation::to_utf8(c, c.arg(3));
        if (key.empty()) return;
        std::string sel = "set" + std::string(1, char(std::toupper(key[0]))) + key.substr(1) + ":";
        if (objc(c).responds(objc(c).class_of(c.arg(0)), objc(c).sel(sel))) objc(c).send(c, c.arg(0), sel, {c.arg(2)});
    });
    o.method("NSObject", "valueForKeyPath:", [](Cpu& c) {
        std::string path = foundation::to_utf8(c, c.arg(2));
        Id cur = c.arg(0);
        size_t start = 0;
        while (cur && start <= path.size())
        {
            size_t dot = path.find('.', start);
            std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            cur = objc(c).send(c, cur, "valueForKey:", {foundation::string_autoreleased(c, part)});
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        c.ret(cur);
    });

    for (const char* sel : {"addObserver:forKeyPath:options:context:", "removeObserver:forKeyPath:", "removeObserver:forKeyPath:context:",
                            "willChangeValueForKey:", "didChangeValueForKey:"})
        o.method("NSObject", sel, [](Cpu& c) {});

    o.method("NSObject", "doesNotRecognizeSelector:", [](Cpu& c) {
        Class* k = objc(c).class_of(c.arg(0));
        c.stop("-[" + (k ? k->name : std::string("?")) + " " + objc(c).sel_name(c.arg(2)) + "]: unrecognized selector");
    });
}

}
