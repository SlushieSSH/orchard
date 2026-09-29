#include <chrono>
#include <cstring>
#include <ctime>
#include <random>
#include <thread>
#include <unordered_map>

#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::Class;
using objc::objc;
using objc::SEL;

namespace
{
Id str(Cpu& c, const std::string& s)
{
    return string_autoreleased(c, s);
}
Id retain(Cpu& c, Id o)
{
    return objc(c).retain_entry(c, o);
}
void release(Cpu& c, Id o)
{
    objc(c).release_entry(c, o);
}

Id make_fields(Cpu& c, const char* cls, std::initializer_list<uint64_t> fields)
{
    Id o = objc(c).alloc_instance(objc(c).host_class(cls), fields.size() * 8);
    uint64_t off = 8;
    for (uint64_t f : fields)
    {
        c.mem.write<uint64_t>(o + off, f);
        off += 8;
    }
    return o;
}
uint64_t field(Cpu& c, Id o, int i)
{
    return c.mem.read<uint64_t>(o + 8 + uint64_t(i) * 8);
}

constexpr double kUnixToReference = 978307200.0;

double now_reference()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count() - kUnixToReference;
}

struct Observer
{
    Id observer = 0;
    SEL sel = 0;
    std::u16string name;
    Id object = 0;
    GuestAddr block = 0;
};

std::mutex observers_lock;
std::vector<Observer> observers;

void post(Cpu& c, Id note)
{
    std::u16string name = to_utf16(c, field(c, note, 0));
    Id sender = field(c, note, 1);
    std::vector<Observer> matching;
    {
        std::lock_guard g(observers_lock);
        for (auto& o : observers)
            if ((o.name.empty() || o.name == name) && (!o.object || o.object == sender)) matching.push_back(o);
    }
    for (auto& o : matching)
    {
        if (c.stopped()) return;
        if (o.block)
            call_block(c, o.block, {note});
        else
            objc(c).send(c, o.observer, o.sel, {note});
    }
}

std::mutex threads_lock;
std::unordered_map<Cpu*, Id> thread_objects;

Id thread_object(Cpu& c, Cpu* target)
{
    std::lock_guard g(threads_lock);
    Id& obj = thread_objects[target];
    if (!obj) obj = make_fields(c, "NSThread", {0, 0});
    return obj;
}

void thread_entry(Cpu& c)
{
    GuestAddr args = c.arg(0);
    Id target = c.mem.read<uint64_t>(args), obj = c.mem.read<uint64_t>(args + 16);
    SEL s = c.mem.read<uint64_t>(args + 8);
    GuestAddr block = c.mem.read<uint64_t>(args + 24);
    auto pool = objc(c).autorelease_push(c);
    if (block)
        call_block(c, block);
    else
        objc(c).send(c, target, s, {obj});
    objc(c).autorelease_pop(c, pool);
}

void start_thread(Cpu& c, Id target, SEL s, Id obj, GuestAddr block)
{
    static GuestAddr entry = c.rt.hle.make_stub("NSThread entry", thread_entry);
    GuestAddr args = c.rt.heap.alloc(32);
    c.mem.write<uint64_t>(args, retain(c, target));
    c.mem.write<uint64_t>(args + 8, s);
    c.mem.write<uint64_t>(args + 16, retain(c, obj));
    c.mem.write<uint64_t>(args + 24, block ? block_copy(c, block) : 0);
    c.rt.threads->create(entry, args, 1 << 20, true);
}

struct HostLock
{
    std::recursive_mutex m;
    std::condition_variable_any cv;
    std::atomic<uint64_t> seq = 0;
};

std::mutex locks_lock;
std::unordered_map<Id, std::unique_ptr<HostLock>> locks;

HostLock& lock_of(Id obj)
{
    std::lock_guard g(locks_lock);
    auto& p = locks[obj];
    if (!p) p = std::make_unique<HostLock>();
    return *p;
}

void register_notifications(objc::ObjcRuntime& o)
{
    o.class_method("NSNotificationCenter", "defaultCenter", [](Cpu& c) {
        static Id center = objc(c).alloc_instance(objc(c).host_class("NSNotificationCenter"));
        c.ret(center);
    });
    o.method("NSNotificationCenter", "addObserver:selector:name:object:", [](Cpu& c) {
        std::lock_guard g(observers_lock);
        observers.push_back({c.arg(2), c.arg(3), to_utf16(c, c.arg(4)), c.arg(5), 0});
    });
    o.method("NSNotificationCenter", "addObserverForName:object:queue:usingBlock:", [](Cpu& c) {
        Id token = objc(c).alloc_instance(objc(c).host_class("__NSObserver"));
        GuestAddr blk = block_copy(c, c.arg(5));
        std::lock_guard g(observers_lock);
        observers.push_back({token, 0, to_utf16(c, c.arg(2)), c.arg(3), blk});
        c.ret(objc(c).autorelease(c, token));
    });
    auto remove = [](Cpu& c, Id observer, Id name, Id object) {
        std::u16string n = to_utf16(c, name);
        std::lock_guard g(observers_lock);
        std::erase_if(observers, [&](const Observer& ob) {
            return ob.observer == observer && (n.empty() || ob.name == n) && (!object || ob.object == object);
        });
    };
    static decltype(remove) s_remove = remove;
    o.method("NSNotificationCenter", "removeObserver:", [](Cpu& c) { s_remove(c, c.arg(2), 0, 0); });
    o.method("NSNotificationCenter", "removeObserver:name:object:", [](Cpu& c) { s_remove(c, c.arg(2), c.arg(3), c.arg(4)); });
    o.method("NSNotificationCenter", "postNotification:", [](Cpu& c) { post(c, c.arg(2)); });
    o.method("NSNotificationCenter", "postNotificationName:object:", [](Cpu& c) {
        Id note = make_fields(c, "NSNotification", {retain(c, c.arg(2)), retain(c, c.arg(3)), 0});
        post(c, note);
        release(c, note);
    });
    o.method("NSNotificationCenter", "postNotificationName:object:userInfo:", [](Cpu& c) {
        Id note = make_fields(c, "NSNotification", {retain(c, c.arg(2)), retain(c, c.arg(3)), retain(c, c.arg(4))});
        post(c, note);
        release(c, note);
    });

    o.class_method("NSNotification", "notificationWithName:object:", [](Cpu& c) {
        c.ret(objc(c).autorelease(c, make_fields(c, "NSNotification", {retain(c, c.arg(2)), retain(c, c.arg(3)), 0})));
    });
    o.class_method("NSNotification", "notificationWithName:object:userInfo:", [](Cpu& c) {
        c.ret(objc(c).autorelease(c, make_fields(c, "NSNotification", {retain(c, c.arg(2)), retain(c, c.arg(3)), retain(c, c.arg(4))})));
    });
    o.method("NSNotification", "name", [](Cpu& c) { c.ret(field(c, c.arg(0), 0)); });
    o.method("NSNotification", "object", [](Cpu& c) { c.ret(field(c, c.arg(0), 1)); });
    o.method("NSNotification", "userInfo", [](Cpu& c) { c.ret(field(c, c.arg(0), 2)); });
    o.method("NSNotification", "dealloc", [](Cpu& c) {
        for (int i = 0; i < 3; ++i)
            release(c, field(c, c.arg(0), i));
        objc(c).dispose(c.arg(0));
    });
}

void register_threads(objc::ObjcRuntime& o)
{
    o.class_method("NSThread", "currentThread", [](Cpu& c) { c.ret(thread_object(c, &c)); });
    o.class_method("NSThread", "mainThread", [](Cpu& c) { c.ret(thread_object(c, c.rt.main_cpu)); });
    o.class_method("NSThread", "isMainThread", [](Cpu& c) { c.ret(c.is_main); });
    o.class_method("NSThread", "isMultiThreaded", [](Cpu& c) { c.ret(1); });
    o.class_method("NSThread", "sleepForTimeInterval:", [](Cpu& c) {
        std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.0, c.d(0))));
    });
    o.class_method("NSThread",
                   "detachNewThreadSelector:toTarget:withObject:", [](Cpu& c) { start_thread(c, c.arg(3), c.arg(2), c.arg(4), 0); });
    o.class_method("NSThread", "detachNewThreadWithBlock:", [](Cpu& c) { start_thread(c, 0, 0, 0, c.arg(2)); });
    o.class_method("NSThread", "setThreadPriority:", [](Cpu& c) { c.ret(1); });
    o.class_method("NSThread", "callStackSymbols", [](Cpu& c) { c.ret(make_array(c, {})); });

    o.method("NSThread", "initWithTarget:selector:object:", [](Cpu& c) {
        Id t = make_fields(c, "NSThread", {0, 0, retain(c, c.arg(2)), c.arg(3), retain(c, c.arg(4)), 0});
        c.ret(t);
    });
    o.method("NSThread", "initWithBlock:", [](Cpu& c) { c.ret(make_fields(c, "NSThread", {0, 0, 0, 0, 0, block_copy(c, c.arg(2))})); });
    o.method("NSThread", "start", [](Cpu& c) {
        Id t = c.arg(0);
        start_thread(c, field(c, t, 2), field(c, t, 3), field(c, t, 4), field(c, t, 5));
    });
    o.method("NSThread", "isMainThread", [](Cpu& c) { c.ret(c.arg(0) == thread_object(c, c.rt.main_cpu)); });
    o.method("NSThread", "name", [](Cpu& c) { c.ret(field(c, c.arg(0), 0)); });
    o.method("NSThread", "setName:", [](Cpu& c) {
        Id old = field(c, c.arg(0), 0);
        c.mem.write<uint64_t>(c.arg(0) + 8, objc(c).send(c, c.arg(2), "copy"));
        release(c, old);
    });
    o.method("NSThread", "threadDictionary", [](Cpu& c) {
        Id d = field(c, c.arg(0), 1);
        if (!d)
        {
            d = retain(c, make_dict(c, {}, true));
            c.mem.write<uint64_t>(c.arg(0) + 16, d);
        }
        c.ret(d);
    });
    o.method("NSThread", "setQualityOfService:", [](Cpu& c) {});
    o.method("NSThread", "qualityOfService", [](Cpu& c) { c.ret(0x19); });
    o.method("NSThread", "setThreadPriority:", [](Cpu& c) {});
    o.method("NSThread", "setStackSize:", [](Cpu& c) {});
    o.method("NSThread", "isExecuting", [](Cpu& c) { c.ret(1); });
    o.method("NSThread", "isFinished", [](Cpu& c) { c.ret(0); });
    o.method("NSThread", "isCancelled", [](Cpu& c) { c.ret(0); });
    o.method("NSThread", "cancel", [](Cpu& c) {});
}

void register_dates(objc::ObjcRuntime& o)
{
    o.define("__NSDate", "NSDate");
    auto alloc = [](Cpu& c) {
        Class* k = objc(c).class_at(c.arg(0));
        c.ret(objc(c).alloc_instance(k && k->host ? objc(c).host_class("__NSDate") : k, 8));
    };
    o.class_method("NSDate", "alloc", alloc);
    o.class_method("NSDate", "allocWithZone:", alloc);
    o.class_method("NSDate", "date", [](Cpu& c) { c.ret(date_with_reference_seconds(c, now_reference())); });
    o.class_method("NSDate", "now", [](Cpu& c) { c.ret(date_with_reference_seconds(c, now_reference())); });
    o.class_method("NSDate",
                   "dateWithTimeIntervalSinceNow:", [](Cpu& c) { c.ret(date_with_reference_seconds(c, now_reference() + c.d(0))); });
    o.class_method("NSDate",
                   "dateWithTimeIntervalSince1970:", [](Cpu& c) { c.ret(date_with_reference_seconds(c, c.d(0) - kUnixToReference)); });
    o.class_method("NSDate", "dateWithTimeIntervalSinceReferenceDate:", [](Cpu& c) { c.ret(date_with_reference_seconds(c, c.d(0))); });
    o.class_method("NSDate", "distantFuture", [](Cpu& c) { c.ret(date_with_reference_seconds(c, 63113904000.0)); });
    o.class_method("NSDate", "distantPast", [](Cpu& c) { c.ret(date_with_reference_seconds(c, -63114076800.0)); });
    o.class_method("NSDate", "timeIntervalSinceReferenceDate", [](Cpu& c) { c.set_d(0, now_reference()); });

    auto secs = [](Cpu& c, Id d) {
        double s = 0;
        date_seconds(c, d, s);
        return s;
    };
    static decltype(secs) s_secs = secs;
    o.method("NSDate", "init", [](Cpu& c) { c.mem.write<double>(c.arg(0) + 8, now_reference()); });
    o.method("NSDate", "initWithTimeIntervalSinceNow:", [](Cpu& c) { c.mem.write<double>(c.arg(0) + 8, now_reference() + c.d(0)); });
    o.method("NSDate", "initWithTimeIntervalSinceReferenceDate:", [](Cpu& c) { c.mem.write<double>(c.arg(0) + 8, c.d(0)); });
    o.method("NSDate", "initWithTimeIntervalSince1970:", [](Cpu& c) { c.mem.write<double>(c.arg(0) + 8, c.d(0) - kUnixToReference); });
    o.method("NSDate", "timeIntervalSinceReferenceDate", [](Cpu& c) { c.set_d(0, s_secs(c, c.arg(0))); });
    o.method("NSDate", "timeIntervalSince1970", [](Cpu& c) { c.set_d(0, s_secs(c, c.arg(0)) + kUnixToReference); });
    o.method("NSDate", "timeIntervalSinceNow", [](Cpu& c) { c.set_d(0, s_secs(c, c.arg(0)) - now_reference()); });
    o.method("NSDate", "timeIntervalSinceDate:", [](Cpu& c) { c.set_d(0, s_secs(c, c.arg(0)) - s_secs(c, c.arg(2))); });
    o.method("NSDate", "dateByAddingTimeInterval:", [](Cpu& c) { c.ret(date_with_reference_seconds(c, s_secs(c, c.arg(0)) + c.d(0))); });
    o.method("NSDate", "compare:", [](Cpu& c) {
        double a = s_secs(c, c.arg(0)), b = s_secs(c, c.arg(2));
        c.ret(uint64_t(a < b ? -1 : a > b ? 1 : 0));
    });
    o.method("NSDate", "earlierDate:", [](Cpu& c) { c.ret(s_secs(c, c.arg(0)) <= s_secs(c, c.arg(2)) ? c.arg(0) : c.arg(2)); });
    o.method("NSDate", "laterDate:", [](Cpu& c) { c.ret(s_secs(c, c.arg(0)) >= s_secs(c, c.arg(2)) ? c.arg(0) : c.arg(2)); });
    o.method("NSDate", "isEqualToDate:", [](Cpu& c) { c.ret(s_secs(c, c.arg(0)) == s_secs(c, c.arg(2))); });
    o.method("NSDate", "copyWithZone:", [](Cpu& c) { c.ret(retain(c, c.arg(0))); });
    o.method("NSDate", "description", [](Cpu& c) {
        std::time_t t = std::time_t(s_secs(c, c.arg(0)) + kUnixToReference);
        std::tm tm{};
        gmtime_s(&tm, &t);
        char buf[64];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S +0000", &tm);
        c.ret(str(c, buf));
    });
}

void register_locks(objc::ObjcRuntime& o)
{
    for (const char* cls : {"NSLock", "NSRecursiveLock", "NSCondition", "NSConditionLock"})
    {
        o.method(cls, "lock", [](Cpu& c) { lock_of(c.arg(0)).m.lock(); });
        o.method(cls, "unlock", [](Cpu& c) { lock_of(c.arg(0)).m.unlock(); });
        o.method(cls, "tryLock", [](Cpu& c) { c.ret(lock_of(c.arg(0)).m.try_lock()); });
        o.method(cls, "setName:", [](Cpu& c) {});
        o.method(cls, "dealloc", [](Cpu& c) {
            {
                std::lock_guard g(locks_lock);
                locks.erase(c.arg(0));
            }
            objc(c).dispose(c.arg(0));
        });
    }
    auto wait = [](Cpu& c, std::chrono::steady_clock::time_point deadline) {
        HostLock& l = lock_of(c.arg(0));
        uint64_t seen = l.seq;
        std::unique_lock<std::recursive_mutex> held(l.m, std::adopt_lock);
        bool ok = c.rt.wait(l.cv, held, [&] { return l.seq != seen; }, deadline);
        held.release();
        return ok;
    };
    static decltype(wait) s_wait = wait;
    o.method("NSCondition", "wait", [](Cpu& c) { s_wait(c, std::chrono::steady_clock::time_point::max()); });
    o.method("NSCondition", "waitUntilDate:", [](Cpu& c) {
        double secs = 0;
        date_seconds(c, c.arg(2), secs);
        auto delta = std::chrono::duration<double>(secs - now_reference());
        c.ret(s_wait(c, std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delta)));
    });
    auto signal = [](Cpu& c) {
        HostLock& l = lock_of(c.arg(0));
        ++l.seq;
        l.cv.notify_all();
    };
    o.method("NSCondition", "signal", signal);
    o.method("NSCondition", "broadcast", signal);
}

void register_identity(objc::ObjcRuntime& o)
{
    o.class_method("NSUUID", "UUID", [](Cpu& c) {
        Id u = objc(c).alloc_instance(objc(c).host_class("NSUUID"), 16);
        static std::mt19937_64 rng{std::random_device{}()};
        uint64_t a = rng(), b = rng();
        a = (a & ~0xf000ull) | 0x4000;
        b = (b & ~(3ull << 6)) | (2ull << 6);
        c.mem.write<uint64_t>(u + 8, a);
        c.mem.write<uint64_t>(u + 16, b);
        c.ret(objc(c).autorelease(c, u));
    });
    o.method("NSUUID", "init", [](Cpu& c) {
        Id u = objc(c).send(c, objc(c).host_class("NSUUID")->addr, "UUID");
        objc(c).dispose(c.arg(0));
        c.ret(retain(c, u));
    });
    o.method("NSUUID", "initWithUUIDString:", [](Cpu& c) {
        std::string s = to_utf8(c, c.arg(2));
        std::string hex;
        for (char ch : s)
            if (std::isxdigit(uint8_t(ch))) hex += ch;
        if (hex.size() != 32) return c.ret(0);
        for (int i = 0; i < 16; ++i)
            c.mem.write<uint8_t>(c.arg(0) + 8 + i, uint8_t(std::stoi(hex.substr(i * 2, 2), nullptr, 16)));
    });
    o.method("NSUUID", "UUIDString", [](Cpu& c) {
        std::string out;
        char buf[4];
        for (int i = 0; i < 16; ++i)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
            std::snprintf(buf, sizeof buf, "%02X", c.mem.read<uint8_t>(c.arg(0) + 8 + i));
            out += buf;
        }
        c.ret(str(c, out));
    });
    o.method("NSUUID", "getUUIDBytes:", [](Cpu& c) {
        uint8_t b[16];
        c.mem.read_bytes(c.arg(0) + 8, b, 16);
        c.mem.write_bytes(c.arg(2), b, 16);
    });

    for (const char* sel : {"currentLocale", "autoupdatingCurrentLocale", "systemLocale"})
        o.class_method("NSLocale", sel, [](Cpu& c) {
            static Id locale = objc(c).alloc_instance(objc(c).host_class("NSLocale"));
            c.ret(locale);
        });
    o.class_method("NSLocale", "localeWithLocaleIdentifier:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "currentLocale")); });
    o.class_method("NSLocale", "preferredLanguages", [](Cpu& c) { c.ret(make_array(c, {str(c, "en-US")})); });
    o.method("NSLocale", "initWithLocaleIdentifier:", [](Cpu& c) {});
    o.method("NSLocale", "localeIdentifier", [](Cpu& c) { c.ret(str(c, "en_US")); });
    o.method("NSLocale", "languageCode", [](Cpu& c) { c.ret(str(c, "en")); });
    o.method("NSLocale", "countryCode", [](Cpu& c) { c.ret(str(c, "US")); });
    o.method("NSLocale", "regionCode", [](Cpu& c) { c.ret(str(c, "US")); });
    o.method("NSLocale", "currencyCode", [](Cpu& c) { c.ret(str(c, "USD")); });
    o.method("NSLocale", "currencySymbol", [](Cpu& c) { c.ret(str(c, "$")); });
    o.method("NSLocale", "decimalSeparator", [](Cpu& c) { c.ret(str(c, ".")); });
    o.method("NSLocale", "groupingSeparator", [](Cpu& c) { c.ret(str(c, ",")); });
    o.method("NSLocale", "usesMetricSystem", [](Cpu& c) { c.ret(0); });
    o.method("NSLocale", "objectForKey:", [](Cpu& c) {
        std::string k = to_utf8(c, c.arg(2));
        if (k == "kCFLocaleCountryCodeKey" || k == "NSLocaleCountryCode") return c.ret(str(c, "US"));
        if (k == "kCFLocaleLanguageCodeKey" || k == "NSLocaleLanguageCode") return c.ret(str(c, "en"));
        if (k == "kCFLocaleCurrencyCodeKey" || k == "NSLocaleCurrencyCode") return c.ret(str(c, "USD"));
        if (k == "kCFLocaleIdentifierKey" || k == "NSLocaleIdentifier") return c.ret(str(c, "en_US"));
        if (k == "kCFLocaleDecimalSeparatorKey" || k == "NSLocaleDecimalSeparator") return c.ret(str(c, "."));
        c.ret(0);
    });
    o.method("NSLocale", "displayNameForKey:value:", [](Cpu& c) { c.ret(c.arg(3)); });

    for (const char* sel : {"localTimeZone", "systemTimeZone", "defaultTimeZone"})
        o.class_method("NSTimeZone", sel, [](Cpu& c) {
            static Id tz = objc(c).alloc_instance(objc(c).host_class("NSTimeZone"));
            c.ret(tz);
        });
    o.class_method("NSTimeZone", "timeZoneWithName:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "localTimeZone")); });
    o.class_method("NSTimeZone", "timeZoneForSecondsFromGMT:", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "localTimeZone")); });
    o.method("NSTimeZone", "name", [](Cpu& c) { c.ret(str(c, "GMT")); });
    o.method("NSTimeZone", "abbreviation", [](Cpu& c) { c.ret(str(c, "GMT")); });
    o.method("NSTimeZone", "secondsFromGMT", [](Cpu& c) { c.ret(0); });
    o.method("NSTimeZone", "secondsFromGMTForDate:", [](Cpu& c) { c.ret(0); });
    o.method("NSTimeZone", "isDaylightSavingTime", [](Cpu& c) { c.ret(0); });
}

void register_errors(objc::ObjcRuntime& o)
{
    o.class_method("NSError", "errorWithDomain:code:userInfo:", [](Cpu& c) {
        c.ret(objc(c).autorelease(c, make_fields(c, "NSError", {retain(c, c.arg(2)), c.arg(3), retain(c, c.arg(4))})));
    });
    o.method("NSError", "initWithDomain:code:userInfo:", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(0) + 8, retain(c, c.arg(2)));
        c.mem.write<uint64_t>(c.arg(0) + 16, c.arg(3));
        c.mem.write<uint64_t>(c.arg(0) + 24, retain(c, c.arg(4)));
    });
    o.class_method("NSError", "alloc", [](Cpu& c) { c.ret(objc(c).alloc_instance(objc(c).class_at(c.arg(0)), 24)); });
    o.method("NSError", "domain", [](Cpu& c) { c.ret(field(c, c.arg(0), 0)); });
    o.method("NSError", "code", [](Cpu& c) { c.ret(field(c, c.arg(0), 1)); });
    o.method("NSError", "userInfo", [](Cpu& c) { c.ret(field(c, c.arg(0), 2)); });
    o.method("NSError", "localizedDescription", [](Cpu& c) {
        Id desc = dict_lookup(c, field(c, c.arg(0), 2), str(c, "NSLocalizedDescription"));
        if (desc) return c.ret(desc);
        c.ret(str(c, "The operation couldn't be completed. (" + to_utf8(c, field(c, c.arg(0), 0)) + " error " +
                         std::to_string(int64_t(field(c, c.arg(0), 1))) + ".)"));
    });
    o.method("NSError", "description", [](Cpu& c) { c.ret(objc(c).send(c, c.arg(0), "localizedDescription")); });

    o.class_method("NSException", "exceptionWithName:reason:userInfo:", [](Cpu& c) {
        c.ret(objc(c).autorelease(c, make_fields(c, "NSException", {retain(c, c.arg(2)), retain(c, c.arg(3)), retain(c, c.arg(4))})));
    });
    o.method("NSException", "initWithName:reason:userInfo:", [](Cpu& c) {
        Id e = make_fields(c, "NSException", {retain(c, c.arg(2)), retain(c, c.arg(3)), retain(c, c.arg(4))});
        objc(c).dispose(c.arg(0));
        c.ret(e);
    });
    o.class_method("NSException",
                   "raise:format:", [](Cpu& c) { c.stop("NSException raised: " + to_utf8(c, c.arg(2)) + ": " + to_utf8(c, c.arg(3))); });
    o.method("NSException", "name", [](Cpu& c) { c.ret(field(c, c.arg(0), 0)); });
    o.method("NSException", "reason", [](Cpu& c) { c.ret(field(c, c.arg(0), 1)); });
    o.method("NSException", "userInfo", [](Cpu& c) { c.ret(field(c, c.arg(0), 2)); });
    o.method("NSException", "raise",
             [](Cpu& c) { c.stop("NSException raised: " + to_utf8(c, field(c, c.arg(0), 0)) + ": " + to_utf8(c, field(c, c.arg(0), 1))); });
    o.method("NSException", "callStackSymbols", [](Cpu& c) { c.ret(make_array(c, {})); });
}

void register_enumerators(objc::ObjcRuntime& o)
{
    auto make_enum = [](Cpu& c, std::vector<Id> items) {
        Id arr = retain(c, make_array(c, items));
        return objc(c).autorelease(c, make_fields(c, "__NSArrayEnumerator", {arr, 0}));
    };
    static decltype(make_enum) s_make = make_enum;
    o.define("__NSArrayEnumerator", "NSEnumerator");
    o.method("NSArray", "objectEnumerator", [](Cpu& c) { c.ret(s_make(c, array_items(c, c.arg(0)))); });
    o.method("NSArray", "reverseObjectEnumerator", [](Cpu& c) {
        auto items = array_items(c, c.arg(0));
        std::reverse(items.begin(), items.end());
        c.ret(s_make(c, items));
    });
    o.method("NSSet", "objectEnumerator", [](Cpu& c) { c.ret(s_make(c, array_items(c, objc(c).send(c, c.arg(0), "allObjects")))); });
    o.method("__NSArrayEnumerator", "nextObject", [](Cpu& c) {
        auto items = array_items(c, field(c, c.arg(0), 0));
        uint64_t i = field(c, c.arg(0), 1);
        if (i >= items.size()) return c.ret(0);
        c.mem.write<uint64_t>(c.arg(0) + 16, i + 1);
        c.ret(items[i]);
    });
    o.method("__NSArrayEnumerator", "allObjects", [](Cpu& c) {
        auto items = array_items(c, field(c, c.arg(0), 0));
        uint64_t i = field(c, c.arg(0), 1);
        c.ret(make_array(c, std::vector<Id>(items.begin() + std::min<size_t>(i, items.size()), items.end())));
    });
    o.method("__NSArrayEnumerator", "dealloc", [](Cpu& c) {
        release(c, field(c, c.arg(0), 0));
        objc(c).dispose(c.arg(0));
    });
}

void register_operations(objc::ObjcRuntime& o)
{
    o.class_method("NSOperationQueue", "mainQueue", [](Cpu& c) {
        static Id q = make_fields(c, "NSOperationQueue", {main_queue_object(c)});
        c.ret(q);
    });
    o.class_method("NSOperationQueue", "currentQueue", [](Cpu& c) { c.ret(c.is_main ? objc(c).send(c, c.arg(0), "mainQueue") : 0); });
    o.method("NSOperationQueue", "init", [](Cpu& c) {
        Id q = make_fields(c, "NSOperationQueue", {global_queue_object()});
        objc(c).dispose(c.arg(0));
        c.ret(q);
    });
    o.method("NSOperationQueue", "addOperationWithBlock:", [](Cpu& c) { dispatch_block_async(c, field(c, c.arg(0), 0), c.arg(2)); });
    o.method("NSOperationQueue", "setMaxConcurrentOperationCount:", [](Cpu& c) {});
    o.method("NSOperationQueue", "setName:", [](Cpu& c) {});
    o.method("NSOperationQueue", "setQualityOfService:", [](Cpu& c) {});
    o.method("NSOperationQueue", "setUnderlyingQueue:", [](Cpu& c) {
        if (c.arg(2)) c.mem.write<uint64_t>(c.arg(0) + 8, c.arg(2));
    });
    o.method("NSOperationQueue", "cancelAllOperations", [](Cpu& c) {});
    o.method("NSOperationQueue", "waitUntilAllOperationsAreFinished", [](Cpu& c) {});
}

}

Id date_with_reference_seconds(Cpu& c, double seconds)
{
    Id d = objc(c).alloc_instance(objc(c).host_class("__NSDate"), 8);
    c.mem.write<double>(d + 8, seconds);
    return objc(c).autorelease(c, d);
}

bool date_seconds(Cpu& c, Id obj, double& out)
{
    Class* k = objc(c).class_of(obj);
    if (!k || !objc(c).is_subclass(k, objc(c).class_named("NSDate"))) return false;
    out = c.mem.read<double>(obj + 8);
    return true;
}

void register_runtime_classes(objc::ObjcRuntime& o)
{
    register_notifications(o);
    register_threads(o);
    register_dates(o);
    register_locks(o);
    register_identity(o);
    register_errors(o);
    register_enumerators(o);
    register_operations(o);
}

}
