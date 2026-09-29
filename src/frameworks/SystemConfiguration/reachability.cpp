#include "core/runtime.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "host/permission.h"
#include "objc/runtime.h"

namespace orchard::sc
{
using objc::objc;

void register_system_configuration(objc::ObjcRuntime& o)
{
    o.define("OrchardSCNetworkReachability", "NSObject");
    Hle& h = o.rt.hle;
    auto create = [](Cpu& c) { c.ret(objc(c).alloc_instance(objc(c).host_class("OrchardSCNetworkReachability"))); };
    h.fn("_SCNetworkReachabilityCreateWithAddress", create);
    h.fn("_SCNetworkReachabilityCreateWithAddressPair", create);
    h.fn("_SCNetworkReachabilityCreateWithName", create);
    h.fn("_SCNetworkReachabilityGetFlags", [](Cpu& c) {
        if (c.arg(1)) c.mem.write<uint32_t>(c.arg(1), network_allowed() ? 2 : 0);
        c.ret(1);
    });
    for (const char* n : {"_SCNetworkReachabilitySetCallback", "_SCNetworkReachabilityScheduleWithRunLoop",
                          "_SCNetworkReachabilityUnscheduleFromRunLoop", "_SCNetworkReachabilitySetDispatchQueue"})
        h.fn(n, [](Cpu& c) { c.ret(1); });
    h.fn("_CNCopyCurrentNetworkInfo", [](Cpu& c) { c.ret(0); });
    h.fn("_CNCopySupportedInterfaces", [](Cpu& c) { c.ret(0); });
}

}
