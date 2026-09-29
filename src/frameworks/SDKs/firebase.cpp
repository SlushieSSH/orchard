#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "core/plist.h"
#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/Metal/descriptors.h"
#include "frameworks/libSystem/blocks.h"
#include "objc/runtime.h"

namespace orchard::sdk
{
using foundation::Id;
using metal::prop_x;
using metal::set_prop;
using objc::objc;

namespace
{
constexpr const char* kDefaultAppName = "__FIRAPP_DEFAULT";

std::mutex lock;
std::map<std::string, Id> apps;

Id new_options(Cpu& c)
{
    return objc(c).alloc_instance(objc(c).host_class("FIROptions"));
}

void set_string(Cpu& c, Id obj, const char* prop, const std::string& value)
{
    set_prop(obj, prop, objc(c).retain(foundation::string_autoreleased(c, value)));
}

bool load_plist(Cpu& c, Id options, const std::string& guest_path)
{
    auto host = c.rt.vfs.to_host(guest_path);
    if (!host) return false;
    std::ifstream f(*host, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto plist = parse_plist(bytes);
    if (!plist || plist->kind != Plist::Kind::Dict) return false;
    static const std::pair<const char*, const char*> kKeys[] = {{"API_KEY", "APIKey"},
                                                                {"GCM_SENDER_ID", "GCMSenderID"},
                                                                {"BUNDLE_ID", "bundleID"},
                                                                {"PROJECT_ID", "projectID"},
                                                                {"GOOGLE_APP_ID", "googleAppID"},
                                                                {"STORAGE_BUCKET", "storageBucket"},
                                                                {"DATABASE_URL", "databaseURL"},
                                                                {"CLIENT_ID", "clientID"},
                                                                {"TRACKING_ID", "trackingID"},
                                                                {"ANDROID_CLIENT_ID", "androidClientID"}};
    for (auto& [key, prop] : kKeys)
        if (const Plist* v = plist->get(key); v && v->kind == Plist::Kind::String) set_string(c, options, prop, v->s);
    return true;
}

Id default_options(Cpu& c)
{
    static Id shared = [&]() -> Id {
        Id opt = new_options(c);
        if (load_plist(c, opt, c.rt.vfs.bundle_path + "/GoogleService-Info.plist")) return opt;
        objc(c).release(c, opt);
        return 0;
    }();
    return shared;
}

Id configure(Cpu& c, const std::string& name, Id options)
{
    Id app = objc(c).alloc_instance(objc(c).host_class("FIRApp"));
    set_string(c, app, "name", name);
    set_prop(app, "options", objc(c).send(c, options, "copy"));
    std::lock_guard g(lock);
    apps[name] = app;
    return app;
}

Id app_named(const std::string& name)
{
    std::lock_guard g(lock);
    auto it = apps.find(name);
    return it == apps.end() ? 0 : it->second;
}

}

void register_firebase(objc::ObjcRuntime& o)
{
    o.define("FIROptions", "OrchardDescriptor");
    o.define("FIRApp", "OrchardDescriptor");
    o.replace_guest_class("FIROptions");
    o.replace_guest_class("FIRApp");

    o.class_method("FIROptions", "defaultOptions", [](Cpu& c) { c.ret(default_options(c)); });
    o.method("FIROptions", "initWithContentsOfFile:", [](Cpu& c) {
        if (!load_plist(c, c.arg(0), foundation::to_utf8(c, c.arg(2))))
        {
            objc(c).release(c, c.arg(0));
            return c.ret(0);
        }
        c.ret(c.arg(0));
    });
    o.method("FIROptions", "initWithGoogleAppID:GCMSenderID:", [](Cpu& c) {
        set_prop(c.arg(0), "googleAppID", objc(c).send(c, c.arg(2), "copy"));
        set_prop(c.arg(0), "GCMSenderID", objc(c).send(c, c.arg(3), "copy"));
        c.ret(c.arg(0));
    });

    o.class_method("FIRApp", "configure", [](Cpu& c) {
        if (Id opt = default_options(c)) configure(c, kDefaultAppName, opt);
    });
    o.class_method("FIRApp", "configureWithOptions:", [](Cpu& c) { configure(c, kDefaultAppName, c.arg(2)); });
    o.class_method("FIRApp", "configureWithName:options:", [](Cpu& c) { configure(c, foundation::to_utf8(c, c.arg(2)), c.arg(3)); });
    o.class_method("FIRApp", "defaultApp", [](Cpu& c) { c.ret(app_named(kDefaultAppName)); });
    o.class_method("FIRApp", "appNamed:", [](Cpu& c) { c.ret(app_named(foundation::to_utf8(c, c.arg(2)))); });
    o.class_method("FIRApp", "allApps", [](Cpu& c) {
        std::vector<std::pair<Id, Id>> entries;
        std::map<std::string, Id> copy;
        {
            std::lock_guard g(lock);
            copy = apps;
        }
        for (auto& [name, app] : copy)
            entries.push_back({foundation::string_autoreleased(c, name), app});
        c.ret(foundation::make_dict(c, entries));
    });
    for (const char* sel : {"registerInternalLibrary:withName:", "registerInternalLibrary:withName:withVersion:",
                            "registerLibrary:withVersion:", "resetApps"})
        o.class_method("FIRApp", sel, [](Cpu& c) {});
    o.class_method("FIRApp", "firebaseUserAgent", [](Cpu& c) { c.ret(foundation::string_autoreleased(c, "")); });
    o.method("FIRApp", "deleteApp:", [](Cpu& c) {
        std::string name = foundation::to_utf8(c, prop_x(c, c.arg(0), "name"));
        {
            std::lock_guard g(lock);
            apps.erase(name);
        }
        if (c.arg(2)) call_block(c, c.arg(2), {1});
    });
}

}
