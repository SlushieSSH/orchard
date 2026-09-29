#include "frameworks/Foundation/foundation.h"

#include <atomic>
#include <cstdio>
#include <regex>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "frameworks/libSystem/format.h"
#include "frameworks/Foundation/foundation.h"
#include "objc/runtime.h"

namespace orchard::objc
{
namespace
{
constexpr std::pair<const char*, const char*> kHierarchy[] = {
    {"NSValue", "NSObject"},
    {"NSNumber", "NSValue"},
    {"NSDecimalNumber", "NSNumber"},
    {"NSConstantIntegerNumber", "NSNumber"},
    {"NSConstantFloatNumber", "NSNumber"},
    {"NSConstantDoubleNumber", "NSNumber"},
    {"NSArray", "NSObject"},
    {"NSMutableArray", "NSArray"},
    {"NSConstantArray", "NSArray"},
    {"NSDictionary", "NSObject"},
    {"NSMutableDictionary", "NSDictionary"},
    {"NSConstantDictionary", "NSDictionary"},
    {"NSSet", "NSObject"},
    {"NSMutableSet", "NSSet"},
    {"NSOrderedSet", "NSObject"},
    {"NSMutableOrderedSet", "NSOrderedSet"},
    {"NSData", "NSObject"},
    {"NSMutableData", "NSData"},
    {"NSAttributedString", "NSObject"},
    {"NSMutableAttributedString", "NSAttributedString"},
    {"NSCharacterSet", "NSObject"},
    {"NSMutableCharacterSet", "NSCharacterSet"},
    {"NSIndexSet", "NSObject"},
    {"NSMutableIndexSet", "NSIndexSet"},
    {"NSURLResponse", "NSObject"},
    {"NSHTTPURLResponse", "NSURLResponse"},
    {"NSURLSessionTask", "NSObject"},
    {"NSURLSessionDataTask", "NSURLSessionTask"},
    {"NSURLSessionDownloadTask", "NSURLSessionTask"},
    {"NSURLSessionStreamTask", "NSURLSessionTask"},
    {"NSURLSessionUploadTask", "NSURLSessionDataTask"},
    {"NSURLSession", "NSObject"},
    {"NSURLSessionConfiguration", "NSObject"},
    {"NSURLRequest", "NSObject"},
    {"NSMutableURLRequest", "NSURLRequest"},
    {"NSOperation", "NSObject"},
    {"NSBlockOperation", "NSOperation"},
    {"NSInvocationOperation", "NSOperation"},
    {"NSFormatter", "NSObject"},
    {"NSDateFormatter", "NSFormatter"},
    {"NSNumberFormatter", "NSFormatter"},
    {"NSByteCountFormatter", "NSFormatter"},
    {"NSISO8601DateFormatter", "NSFormatter"},
    {"NSInputStream", "NSStream"},
    {"NSOutputStream", "NSStream"},
    {"NSStream", "NSObject"},
    {"NSKeyedArchiver", "NSCoder"},
    {"NSKeyedUnarchiver", "NSCoder"},
    {"NSUnitDuration", "NSDimension"},
    {"NSDimension", "NSUnit"},

    {"UIResponder", "NSObject"},
    {"UITouch", "NSObject"},
    {"UIEvent", "NSObject"},
    {"UIApplication", "UIResponder"},
    {"UIView", "UIResponder"},
    {"UIViewController", "UIResponder"},
    {"UIScene", "UIResponder"},
    {"UIWindowScene", "UIScene"},
    {"UIWindow", "UIView"},
    {"UIControl", "UIView"},
    {"UIButton", "UIControl"},
    {"UISlider", "UIControl"},
    {"UITextField", "UIControl"},
    {"UILabel", "UIView"},
    {"UIImageView", "UIView"},
    {"UIScrollView", "UIView"},
    {"UITableView", "UIScrollView"},
    {"UICollectionView", "UIScrollView"},
    {"UITextView", "UIScrollView"},
    {"UITableViewCell", "UIView"},
    {"UICollectionViewCell", "UIView"},
    {"UIStackView", "UIView"},
    {"UIVisualEffectView", "UIView"},
    {"UIActivityIndicatorView", "UIView"},
    {"UIProgressView", "UIView"},
    {"UINavigationBar", "UIView"},
    {"UIToolbar", "UIView"},
    {"UIEventAttributionView", "UIView"},
    {"UINavigationController", "UIViewController"},
    {"UITabBarController", "UIViewController"},
    {"UIPageViewController", "UIViewController"},
    {"UISplitViewController", "UIViewController"},
    {"UITableViewController", "UIViewController"},
    {"UIAlertController", "UIViewController"},
    {"UIActivityViewController", "UIViewController"},
    {"UITapGestureRecognizer", "UIGestureRecognizer"},
    {"UIPanGestureRecognizer", "UIGestureRecognizer"},
    {"UIPinchGestureRecognizer", "UIGestureRecognizer"},
    {"UIRotationGestureRecognizer", "UIGestureRecognizer"},
    {"UISwipeGestureRecognizer", "UIGestureRecognizer"},
    {"UILongPressGestureRecognizer", "UIGestureRecognizer"},
    {"UIBlurEffect", "UIVisualEffect"},
    {"UIVibrancyEffect", "UIVisualEffect"},
    {"UIImpactFeedbackGenerator", "UIFeedbackGenerator"},
    {"UINotificationFeedbackGenerator", "UIFeedbackGenerator"},
    {"UISelectionFeedbackGenerator", "UIFeedbackGenerator"},

    {"CAMetalLayer", "CALayer"},
    {"CAEAGLLayer", "CALayer"},
    {"CAShapeLayer", "CALayer"},
    {"WKWebView", "UIView"},
    {"MPVolumeView", "UIView"},
    {"AVPlayerLayer", "CALayer"},
    {"SKStoreProductViewController", "UIViewController"},
    {"SFSafariViewController", "UIViewController"},
    {"MFMailComposeViewController", "UINavigationController"},
    {"MFMessageComposeViewController", "UINavigationController"},
    {"AVPlayerViewController", "UIViewController"},
    {"SLComposeViewController", "UIViewController"},
    {"AVURLAsset", "AVAsset"},
    {"UNMutableNotificationContent", "UNNotificationContent"},
};

bool is_string_constant(const std::string& symbol)
{
    static const std::regex kPattern("_(NS|UI|AV|MP|SK|GK|CA|WK|UN|CT|CL|CM|CH|GC|MTL|AD|AA|AT|MX|MF|SF|SL|JS|CI|UT)[A-Za-z0-9]*"
                                     "(Notification|Key|Name|Mode|Domain|Identifier|Attribute|Option|Category|Scheme)");
    return std::regex_match(symbol, kPattern);
}

GuestAddr string_constant(Runtime& rt, const std::string& symbol)
{
    GuestAddr var = rt.mem.alloc_system(8, 8);
    rt.mem.write<uint64_t>(var, foundation::new_string(rt, foundation::utf8_to_16(symbol.substr(1))));
    return var;
}

}

void register_block_classes(ObjcRuntime& o)
{
    o.define("NSBlock", "NSObject");
    for (const char* k : {"__NSGlobalBlock__", "__NSStackBlock__", "__NSMallocBlock__"})
        o.define(k, "NSBlock");
    auto copy = [](Cpu& c) { c.ret(block_copy(c, c.arg(0))); };
    o.method("NSBlock", "copy", copy);
    o.method("NSBlock", "copyWithZone:", copy);
    o.method("NSBlock", "retain", copy);
    o.method("NSBlock", "release", [](Cpu& c) { block_release(c, c.arg(0)); });
    o.method("NSBlock", "invoke", [](Cpu& c) { call_block(c, c.arg(0)); });
}

void ns_log(Cpu& c, GuestAddr fmt, VarArgs args)
{
    std::string text = guest_format(c, foundation::to_utf8(c, fmt), args, [&](uint64_t obj) { return foundation::describe(c, obj); });
    std::printf("[NSLog] %s\n", text.c_str());
    std::fflush(stdout);
}

void register_foundation_functions(Hle& h)
{
    static std::atomic<GuestAddr> uncaught{0};
    h.fn("_NSSetUncaughtExceptionHandler", [](Cpu& c) { uncaught = c.arg(0); });
    h.fn("_NSGetUncaughtExceptionHandler", [](Cpu& c) { c.ret(uncaught.load()); });
    h.fn("_NSLog", [](Cpu& c) { ns_log(c, c.arg(0), {c.mem, c.sp()}); });
    h.fn("_NSLogv", [](Cpu& c) { ns_log(c, c.arg(0), {c.mem, c.arg(1)}); });
}

}
namespace orchard::cf
{
void register_corefoundation(objc::ObjcRuntime& o);
}
namespace orchard::objc
{
void register_foundation(ObjcRuntime& o)
{
    for (auto& [cls, super] : kHierarchy)
        o.define(cls, super);
    o.rt.hle.data_resolver(is_string_constant, [&rt = o.rt](const std::string& name) { return string_constant(rt, name); });
    register_block_classes(o);
    register_dispatch_classes(o);
    register_foundation_functions(o.rt.hle);
    foundation::register_nsstring(o);
    foundation::register_nsstring_extra(o);
    foundation::register_regex(o);
    foundation::register_map_tables(o);
    foundation::register_collections(o);
    foundation::register_json(o);
    foundation::register_system(o);
    foundation::register_url_session(o);
    foundation::register_containers_io(o);
    foundation::register_runtime_classes(o);
    cf::register_corefoundation(o);
}

}
