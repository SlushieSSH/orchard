#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/plist.h"
#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/UIKit/uikit.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/Metal/metal.h"
#include "host/window.h"
#include "objc/runtime.h"

namespace fs = std::filesystem;

namespace orchard::audio
{
void register_audio(objc::ObjcRuntime& o);
}
namespace orchard::sdk
{
void register_firebase(objc::ObjcRuntime& o);
}
namespace orchard::sc
{
void register_system_configuration(objc::ObjcRuntime& o);
}

namespace orchard::uikit
{
using foundation::string_autoreleased;
using objc::Class;
using objc::objc;

Rect rect_arg(Cpu& c, int d)
{
    return {c.d(d), c.d(d + 1), c.d(d + 2), c.d(d + 3)};
}
void ret_rect(Cpu& c, const Rect& r)
{
    c.set_d(0, r.x);
    c.set_d(1, r.y);
    c.set_d(2, r.w);
    c.set_d(3, r.h);
}
void ret_size(Cpu& c, double w, double h)
{
    c.set_d(0, w);
    c.set_d(1, h);
}
void ret_point(Cpu& c, double x, double y)
{
    c.set_d(0, x);
    c.set_d(1, y);
}

App& app()
{
    static App a;
    return a;
}

namespace
{
Id str(Cpu& c, const std::string& s)
{
    return string_autoreleased(c, s);
}
Id make(Cpu& c, const char* cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}
Id singleton(Cpu& c, const char* cls)
{
    static std::mutex m;
    static std::unordered_map<std::string, Id> made;
    std::lock_guard g(m);
    Id& o = made[cls];
    if (!o) o = make(c, cls);
    return o;
}

bool responds(Cpu& c, Id obj, const char* sel)
{
    return obj && objc(c).responds(objc(c).class_of(obj), objc(c).sel(sel));
}

void post(Cpu& c, const char* name, Id object)
{
    Id center = objc(c).send(c, objc(c).host_class("NSNotificationCenter")->addr, "defaultCenter");
    objc(c).send(c, center, "postNotificationName:object:", {str(c, name), object});
}

Id shared_application(Cpu& c)
{
    App& a = app();
    if (!a.application) a.application = make(c, "UIApplication");
    return a.application;
}

std::optional<Plist> bundle_info(Cpu& c)
{
    auto host = c.rt.vfs.to_host(c.rt.vfs.bundle_path + "/Info.plist");
    if (!host) return std::nullopt;
    std::ifstream f(*host, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    return parse_plist(bytes);
}

void show_launch_screen(Cpu& c, HostWindow& w)
{
    for (const char* name : {"LaunchScreen-iPhonePortrait.png", "LaunchScreen-iPhone.png", "Default.png"})
    {
        auto host = c.rt.vfs.to_host(c.rt.vfs.bundle_path + "/" + name);
        Image img;
        if (host && load_image(*host, img))
        {
            w.show_image(img);
            return;
        }
    }
}

void connect_scene(Cpu& c, const Plist& info)
{
    App& a = app();
    std::string delegate_class, scene_class = "UIWindowScene";
    if (const Plist* manifest = info.get("UIApplicationSceneManifest"))
        if (const Plist* configs = manifest->get("UISceneConfigurations"))
            if (const Plist* role = configs->get("UIWindowSceneSessionRoleApplication"); role && !role->array.empty())
            {
                delegate_class = role->array[0].string_or("UISceneDelegateClassName");
                scene_class = role->array[0].string_or("UISceneClassName", scene_class);
            }
    if (delegate_class.empty()) return;

    Id session = make(c, "UISceneSession");
    Id options = make(c, "UISceneConnectionOptions");
    if (responds(c, a.delegate, "application:configurationForConnectingSceneSession:options:"))
        objc(c).send(c, a.delegate, "application:configurationForConnectingSceneSession:options:", {a.application, session, options});

    Class* sk = objc(c).class_named(scene_class);
    a.scene = objc(c).alloc_instance(sk ? sk : objc(c).host_class("UIWindowScene"));
    Class* dk = objc(c).class_named(delegate_class);
    if (!dk)
    {
        c.stop("scene delegate class " + delegate_class + " not found");
        return;
    }
    a.scene_delegate = objc(c).send(c, objc(c).send(c, dk->addr, "alloc"), "init");
    std::printf("[UIKit] connecting scene %s with delegate %s\n", scene_class.c_str(), delegate_class.c_str());

    auto call = [&](const char* sel, std::initializer_list<uint64_t> args) {
        if (!c.stopped() && responds(c, a.scene_delegate, sel)) objc(c).send(c, a.scene_delegate, sel, args);
    };
    call("scene:willConnectToSession:options:", {a.scene, session, options});
    post(c, "UISceneWillConnectNotification", a.scene);
    call("sceneWillEnterForeground:", {a.scene});
    post(c, "UISceneWillEnterForegroundNotification", a.scene);
    call("sceneDidBecomeActive:", {a.scene});
    post(c, "UISceneDidActivateNotification", a.scene);
}

void application_main(Cpu& c)
{
    App& a = app();
    static HostWindow window;
    if (!window.create(L"Crossy Road - orchard", int(kScreenW), int(kScreenH)))
    {
        c.stop("could not create the host window");
        return;
    }
    a.window = &window;
    show_launch_screen(c, window);

    Id principal = c.arg(2), delegate_name = c.arg(3);
    if (principal)
    {
        Class* k = objc(c).class_named(foundation::to_utf8(c, principal));
        if (k) a.application = objc(c).send(c, objc(c).send(c, k->addr, "alloc"), "init");
    }
    shared_application(c);
    if (delegate_name)
    {
        std::string name = foundation::to_utf8(c, delegate_name);
        Class* k = objc(c).class_named(name);
        if (!k) return c.stop("app delegate class " + name + " not found");
        Id d = objc(c).send(c, objc(c).send(c, k->addr, "alloc"), "init");
        objc(c).send(c, a.application, "setDelegate:", {d});
        std::printf("[UIKit] UIApplicationMain with delegate %s\n", name.c_str());
    }

    auto info = bundle_info(c);
    if (responds(c, a.delegate, "application:willFinishLaunchingWithOptions:"))
        objc(c).send(c, a.delegate, "application:willFinishLaunchingWithOptions:", {a.application, 0});
    if (!c.stopped() && responds(c, a.delegate, "application:didFinishLaunchingWithOptions:"))
        objc(c).send(c, a.delegate, "application:didFinishLaunchingWithOptions:", {a.application, 0});
    if (c.stopped()) return;
    post(c, "UIApplicationDidFinishLaunchingNotification", a.application);
    if (info) connect_scene(c, *info);
    if (c.stopped()) return;
    if (!a.scene && responds(c, a.delegate, "applicationDidBecomeActive:"))
        objc(c).send(c, a.delegate, "applicationDidBecomeActive:", {a.application});
    post(c, "UIApplicationDidBecomeActiveNotification", a.application);

    std::printf("[UIKit] launch finished, entering the run loop\n");
    while (!c.stopped())
    {
        if (!run_loop_turn(c, 1.0 / 120))
        {
            std::printf("[UIKit] window closed\n");
            std::fflush(stdout);
            std::_Exit(0);
        }
    }
}

void register_application(objc::ObjcRuntime& o)
{
    o.rt.hle.fn("_UIApplicationMain", application_main);

    o.class_method("UIApplication", "sharedApplication", [](Cpu& c) { c.ret(shared_application(c)); });
    o.method("UIApplication", "delegate", [](Cpu& c) { c.ret(app().delegate); });
    o.method("UIApplication", "setDelegate:", [](Cpu& c) { app().delegate = objc(c).retain(c.arg(2)); });
    o.method("UIApplication", "applicationState", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "keyWindow", [](Cpu& c) { c.ret(app().key_window); });
    o.method("UIApplication", "windows", [](Cpu& c) { c.ret(foundation::make_array(c, app().windows)); });
    o.method("UIApplication", "connectedScenes", [](Cpu& c) {
        Id set = objc(c).send(c, objc(c).host_class("NSMutableSet")->addr, "set");
        if (app().scene) objc(c).send(c, set, "addObject:", {app().scene});
        c.ret(set);
    });
    o.method("UIApplication", "statusBarOrientation", [](Cpu& c) { c.ret(1); });
    o.method("UIApplication", "isStatusBarHidden", [](Cpu& c) { c.ret(1); });
    o.method("UIApplication", "isIdleTimerDisabled", [](Cpu& c) { c.ret(1); });
    o.method("UIApplication", "canOpenURL:", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "openURL:", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "openURL:options:completionHandler:", [](Cpu& c) {
        if (c.arg(4)) call_block(c, c.arg(4), {0});
    });
    o.method("UIApplication", "isRegisteredForRemoteNotifications", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "beginBackgroundTaskWithExpirationHandler:", [](Cpu& c) { c.ret(1); });
    o.method("UIApplication", "beginBackgroundTaskWithName:expirationHandler:", [](Cpu& c) { c.ret(1); });
    o.method("UIApplication", "backgroundRefreshStatus", [](Cpu& c) { c.ret(2); });
    o.method("UIApplication", "isProtectedDataAvailable", [](Cpu& c) { c.ret(1); });
    o.method("UIApplication", "supportsAlternateIcons", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "alternateIconName", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "applicationIconBadgeNumber", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "userInterfaceLayoutDirection", [](Cpu& c) { c.ret(0); });
    o.method("UIApplication", "preferredContentSizeCategory", [](Cpu& c) { c.ret(str(c, "UICTContentSizeCategoryL")); });
    o.method("UIApplication", "supportedInterfaceOrientationsForWindow:", [](Cpu& c) { c.ret(30); });
    o.method("UIApplication", "sendAction:to:from:forEvent:", [](Cpu& c) {
        if (c.arg(3)) objc(c).send(c, c.arg(3), c.arg(2), {c.arg(4), c.arg(5)});
        c.ret(c.arg(3) != 0);
    });
    for (const char* sel :
         {"setIdleTimerDisabled:", "registerForRemoteNotifications", "unregisterForRemoteNotifications",
          "endBackgroundTask:", "setApplicationIconBadgeNumber:", "setStatusBarHidden:", "setStatusBarHidden:withAnimation:",
          "setStatusBarOrientation:", "setNetworkActivityIndicatorVisible:", "registerUserNotificationSettings:",
          "setMinimumBackgroundFetchInterval:", "beginReceivingRemoteControlEvents", "endReceivingRemoteControlEvents"})
        o.method("UIApplication", sel, [](Cpu& c) {});

    o.define("UIWindowScene", "UIScene");
    o.method("UIScene", "delegate", [](Cpu& c) { c.ret(app().scene_delegate); });
    o.method("UIScene", "setDelegate:", [](Cpu& c) { app().scene_delegate = objc(c).retain(c.arg(2)); });
    o.method("UIScene", "activationState", [](Cpu& c) { c.ret(0); });
    o.method("UIScene", "session", [](Cpu& c) { c.ret(singleton(c, "UISceneSession")); });
    o.method("UIScene", "title", [](Cpu& c) { c.ret(0); });
    o.method("UIScene", "setTitle:", [](Cpu& c) {});
    o.method("UIWindowScene", "windows", [](Cpu& c) { c.ret(foundation::make_array(c, app().windows)); });
    o.method("UIWindowScene", "keyWindow", [](Cpu& c) { c.ret(app().key_window); });
    o.method("UIWindowScene", "screen", [](Cpu& c) { c.ret(singleton(c, "UIScreen")); });
    o.method("UIWindowScene", "interfaceOrientation", [](Cpu& c) { c.ret(1); });
    o.method("UIWindowScene", "coordinateSpace", [](Cpu& c) { c.ret(singleton(c, "UIScreen")); });
    o.method("UIWindowScene", "traitCollection", [](Cpu& c) { c.ret(singleton(c, "UITraitCollection")); });
    o.method("UIWindowScene", "effectiveGeometry", [](Cpu& c) { c.ret(singleton(c, "UIWindowSceneGeometry")); });
    o.method("UIWindowScene", "requestGeometryUpdateWithPreferences:errorHandler:", [](Cpu& c) {});
    o.method("UIWindowScene", "statusBarManager", [](Cpu& c) { c.ret(singleton(c, "UIStatusBarManager")); });
    o.method("UIWindowSceneGeometry", "interfaceOrientation", [](Cpu& c) { c.ret(1); });
    o.method("UIStatusBarManager", "isStatusBarHidden", [](Cpu& c) { c.ret(1); });
    o.method("UIStatusBarManager", "statusBarFrame", [](Cpu& c) { ret_rect(c, {}); });
    o.method("UISceneSession", "role", [](Cpu& c) { c.ret(str(c, "UIWindowSceneSessionRoleApplication")); });
    o.method("UISceneSession", "persistentIdentifier", [](Cpu& c) { c.ret(str(c, "orchard-scene-0")); });
    o.method("UISceneSession", "configuration", [](Cpu& c) { c.ret(0); });
    o.method("UISceneSession", "scene", [](Cpu& c) { c.ret(app().scene); });
    o.method("UISceneSession", "userInfo", [](Cpu& c) { c.ret(0); });
    o.method("UISceneConnectionOptions", "URLContexts", [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("NSSet")->addr, "set")); });
    o.method("UISceneConnectionOptions", "userActivities",
             [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("NSSet")->addr, "set")); });
    for (const char* sel :
         {"notificationResponse", "shortcutItem", "sourceApplication", "handoffUserActivityType", "cloudKitShareMetadata"})
        o.method("UISceneConnectionOptions", sel, [](Cpu& c) { c.ret(0); });
    o.class_method("UISceneConfiguration",
                   "configurationWithName:sessionRole:", [](Cpu& c) { c.ret(objc(c).autorelease(c, make(c, "UISceneConfiguration"))); });
    o.method("UISceneConfiguration", "initWithName:sessionRole:", [](Cpu& c) {});
    for (const char* sel : {"setDelegateClass:", "setSceneClass:", "setStoryboard:"})
        o.method("UISceneConfiguration", sel, [](Cpu& c) {});
}

void register_device(objc::ObjcRuntime& o)
{
    o.class_method("UIDevice", "currentDevice", [](Cpu& c) { c.ret(singleton(c, "UIDevice")); });
    o.method("UIDevice", "systemVersion", [](Cpu& c) { c.ret(str(c, "16.7.16")); });
    o.method("UIDevice", "systemName", [](Cpu& c) { c.ret(str(c, "iOS")); });
    o.method("UIDevice", "model", [](Cpu& c) { c.ret(str(c, "iPhone")); });
    o.method("UIDevice", "localizedModel", [](Cpu& c) { c.ret(str(c, "iPhone")); });
    o.method("UIDevice", "name", [](Cpu& c) { c.ret(str(c, "iPhone")); });
    o.method("UIDevice", "userInterfaceIdiom", [](Cpu& c) { c.ret(0); });
    o.method("UIDevice", "orientation", [](Cpu& c) { c.ret(1); });
    o.method("UIDevice", "identifierForVendor", [](Cpu& c) {
        static Id uuid = objc(c).retain(objc(c).send(c, objc(c).host_class("NSUUID")->addr, "UUID"));
        c.ret(uuid);
    });
    o.method("UIDevice", "batteryLevel", [](Cpu& c) { c.set_s(0, 1.0f); });
    o.method("UIDevice", "batteryState", [](Cpu& c) { c.ret(3); });
    o.method("UIDevice", "isBatteryMonitoringEnabled", [](Cpu& c) { c.ret(0); });
    o.method("UIDevice", "isMultitaskingSupported", [](Cpu& c) { c.ret(1); });
    o.method("UIDevice", "isGeneratingDeviceOrientationNotifications", [](Cpu& c) { c.ret(1); });
    o.method("UIDevice", "isProximityMonitoringEnabled", [](Cpu& c) { c.ret(0); });
    for (const char* sel : {"setBatteryMonitoringEnabled:", "beginGeneratingDeviceOrientationNotifications",
                            "endGeneratingDeviceOrientationNotifications", "setProximityMonitoringEnabled:", "playInputClick"})
        o.method("UIDevice", sel, [](Cpu& c) {});

    o.class_method("UIScreen", "mainScreen", [](Cpu& c) { c.ret(singleton(c, "UIScreen")); });
    o.class_method("UIScreen", "screens", [](Cpu& c) { c.ret(foundation::make_array(c, {singleton(c, "UIScreen")})); });
    o.method("UIScreen", "bounds", [](Cpu& c) { ret_rect(c, {0, 0, kScreenW, kScreenH}); });
    o.method("UIScreen", "applicationFrame", [](Cpu& c) { ret_rect(c, {0, 0, kScreenW, kScreenH}); });
    o.method("UIScreen", "nativeBounds", [](Cpu& c) { ret_rect(c, {0, 0, kScreenW * kScreenScale, kScreenH * kScreenScale}); });
    o.method("UIScreen", "scale", [](Cpu& c) { c.set_d(0, kScreenScale); });
    o.method("UIScreen", "nativeScale", [](Cpu& c) { c.set_d(0, kScreenScale); });
    o.method("UIScreen", "maximumFramesPerSecond", [](Cpu& c) { c.ret(60); });
    o.method("UIScreen", "brightness", [](Cpu& c) { c.set_d(0, 0.8); });
    o.method("UIScreen", "setBrightness:", [](Cpu& c) {});
    o.method("UIScreen", "setCurrentMode:", [](Cpu& c) {});
    o.method("UIScreen", "potentialEDRHeadroom", [](Cpu& c) { c.set_d(0, 1.0); });
    o.method("UIScreen", "currentEDRHeadroom", [](Cpu& c) { c.set_d(0, 1.0); });
    for (const char* sel : {"convertPoint:toCoordinateSpace:", "convertPoint:fromCoordinateSpace:", "convertRect:toCoordinateSpace:",
                            "convertRect:fromCoordinateSpace:"})
        o.method("UIScreen", sel, [](Cpu& c) {});
    o.method("UIScreen", "coordinateSpace", [](Cpu& c) {});
    o.method("UIScreen", "fixedCoordinateSpace", [](Cpu& c) {});
    o.method("UIScreen", "traitCollection", [](Cpu& c) { c.ret(singleton(c, "UITraitCollection")); });
    o.method("UIScreen", "overscanCompensation", [](Cpu& c) { c.ret(0); });
    o.method("UIScreen", "setOverscanCompensation:", [](Cpu& c) {});
    o.method("UIScreen", "isCaptured", [](Cpu& c) { c.ret(0); });
    o.method("UIScreen", "currentMode", [](Cpu& c) { c.ret(singleton(c, "UIScreenMode")); });
    o.method("UIScreen", "preferredMode", [](Cpu& c) { c.ret(singleton(c, "UIScreenMode")); });
    o.method("UIScreen", "availableModes", [](Cpu& c) { c.ret(foundation::make_array(c, {singleton(c, "UIScreenMode")})); });
    o.method("UIScreen", "displayLinkWithTarget:selector:", [](Cpu& c) {
        c.ret(objc(c).send(c, objc(c).host_class("CADisplayLink")->addr, "displayLinkWithTarget:selector:", {c.arg(2), c.arg(3)}));
    });
    o.method("UIScreenMode", "size", [](Cpu& c) { ret_size(c, kScreenW * kScreenScale, kScreenH * kScreenScale); });
    o.method("UIScreenMode", "pixelAspectRatio", [](Cpu& c) { c.set_d(0, 1); });

    o.class_method("UITraitCollection", "currentTraitCollection", [](Cpu& c) { c.ret(singleton(c, "UITraitCollection")); });
    o.method("UITraitCollection", "userInterfaceIdiom", [](Cpu& c) { c.ret(0); });
    o.method("UITraitCollection", "userInterfaceStyle", [](Cpu& c) { c.ret(1); });
    o.method("UITraitCollection", "displayScale", [](Cpu& c) { c.set_d(0, kScreenScale); });
    o.method("UITraitCollection", "horizontalSizeClass", [](Cpu& c) { c.ret(1); });
    o.method("UITraitCollection", "verticalSizeClass", [](Cpu& c) { c.ret(2); });
    o.method("UITraitCollection", "forceTouchCapability", [](Cpu& c) { c.ret(1); });
    o.method("UITraitCollection", "layoutDirection", [](Cpu& c) { c.ret(0); });
    o.method("UITraitCollection", "displayGamut", [](Cpu& c) { c.ret(1); });
    o.method("UITraitCollection", "preferredContentSizeCategory", [](Cpu& c) { c.ret(str(c, "UICTContentSizeCategoryL")); });

    auto rgba = [](Cpu& c, double r, double g, double b, double a) {
        Id col = objc(c).alloc_instance(objc(c).host_class("UIColor"), 32);
        c.mem.write<double>(col + 8, r);
        c.mem.write<double>(col + 16, g);
        c.mem.write<double>(col + 24, b);
        c.mem.write<double>(col + 32, a);
        return objc(c).autorelease(c, col);
    };
    static decltype(rgba) s_rgba = rgba;
    o.class_method("UIColor", "colorWithRed:green:blue:alpha:", [](Cpu& c) { c.ret(s_rgba(c, c.d(0), c.d(1), c.d(2), c.d(3))); });
    o.class_method("UIColor", "colorWithWhite:alpha:", [](Cpu& c) { c.ret(s_rgba(c, c.d(0), c.d(0), c.d(0), c.d(1))); });
    o.class_method("UIColor", "blackColor", [](Cpu& c) { c.ret(s_rgba(c, 0, 0, 0, 1)); });
    o.class_method("UIColor", "whiteColor", [](Cpu& c) { c.ret(s_rgba(c, 1, 1, 1, 1)); });
    o.class_method("UIColor", "clearColor", [](Cpu& c) { c.ret(s_rgba(c, 0, 0, 0, 0)); });
    o.class_method("UIColor", "systemBackgroundColor", [](Cpu& c) { c.ret(s_rgba(c, 1, 1, 1, 1)); });
    o.method("UIColor", "CGColor", [](Cpu& c) {});
    o.method("UIColor", "colorWithAlphaComponent:", [](Cpu& c) {
        c.ret(s_rgba(c, c.mem.read<double>(c.arg(0) + 8), c.mem.read<double>(c.arg(0) + 16), c.mem.read<double>(c.arg(0) + 24), c.d(0)));
    });
}

}

void register_uikit(objc::ObjcRuntime& o)
{
    for (const char* k : {"UIDevice", "UIScreen", "UIScreenMode", "UITraitCollection", "UIColor", "UISceneSession",
                          "UISceneConnectionOptions", "UISceneConfiguration", "UIWindowSceneGeometry", "UIStatusBarManager"})
        o.define(k, "NSObject");
    register_application(o);
    register_device(o);
    register_views(o);
    register_quartz(o);
    register_runloop(o);
    register_touches(o);
    register_cg(o.rt.hle);
    metal::register_metal(o);
    sdk::register_firebase(o);
    sc::register_system_configuration(o);
    audio::register_audio(o);
}

}
