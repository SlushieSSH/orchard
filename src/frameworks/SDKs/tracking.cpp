#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard::sdk
{
using objc::objc;

namespace
{
constexpr uint64_t kDenied = 2;

GuestAddr answer_stub(Cpu& c)
{
    static GuestAddr stub = c.rt.hle.make_stub("ATTrackingManager answer", [](Cpu& k) {
        call_block(k, k.arg(0), {kDenied});
        block_release(k, k.arg(0));
    });
    return stub;
}

GuestAddr zero_uuid(Cpu& c)
{
    static GuestAddr uuid = [&] {
        GuestAddr u = objc(c).send(c, objc(c).host_class("NSUUID")->addr, "alloc");
        return objc(c).send(c, u, "initWithUUIDString:", {foundation::string_autoreleased(c, "00000000-0000-0000-0000-000000000000")});
    }();
    return uuid;
}
}

void register_tracking(objc::ObjcRuntime& o)
{
    o.define("ATTrackingManager", "NSObject");
    o.class_method("ATTrackingManager", "trackingAuthorizationStatus", [](Cpu& c) { c.ret(kDenied); });
    o.set_method_types(o.host_class("ATTrackingManager")->meta, "trackingAuthorizationStatus", "Q16@0:8");
    o.class_method("ATTrackingManager", "requestTrackingAuthorizationWithCompletionHandler:", [](Cpu& c) {
        if (c.arg(2)) dispatch_function_async(main_queue_object(c), answer_stub(c), block_copy(c, c.arg(2)));
    });

    o.define("ASIdentifierManager", "NSObject");
    o.class_method("ASIdentifierManager", "sharedManager", [](Cpu& c) {
        static GuestAddr shared = objc(c).alloc_instance(objc(c).host_class("ASIdentifierManager"));
        c.ret(shared);
    });
    o.method("ASIdentifierManager", "advertisingIdentifier", [](Cpu& c) { c.ret(zero_uuid(c)); });
    o.method("ASIdentifierManager", "isAdvertisingTrackingEnabled", [](Cpu& c) { c.ret(0); });
}

}
