#include <algorithm>
#include <cstdio>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/UIKit/uikit.h"
#include "frameworks/libSystem/blocks.h"
#include "objc/runtime.h"

namespace orchard::uikit
{
using objc::Class;
using objc::objc;

StateTable<ViewState>& views()
{
    static StateTable<ViewState> t;
    return t;
}
StateTable<LayerState>& layers()
{
    static StateTable<LayerState> t;
    return t;
}
StateTable<ControllerState>& controllers()
{
    static StateTable<ControllerState> t;
    return t;
}

namespace
{
Id make(Cpu& c, const char* cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}
Id send(Cpu& c, Id o, const char* sel, std::initializer_list<uint64_t> args = {})
{
    return objc(c).send(c, o, sel, args);
}
bool responds(Cpu& c, Id obj, const char* sel)
{
    return obj && objc(c).responds(objc(c).class_of(obj), objc(c).sel(sel));
}

void send_rect(Cpu& c, Id obj, const char* sel, const Rect& r)
{
    GuestAddr imp = objc(c).resolve_send(c, obj, objc(c).sel(sel));
    if (!imp || imp == c.rt.hle.ret_instruction()) return;
    c.set_d(0, r.x);
    c.set_d(1, r.y);
    c.set_d(2, r.w);
    c.set_d(3, r.h);
    c.call(imp, {obj, objc(c).sel(sel)});
}

void layer_set_frame(Id layer, const Rect& r)
{
    if (!layer) return;
    LayerState& l = layers().get(layer);
    l.frame = r;
    l.bounds = {0, 0, r.w, r.h};
}

void set_view_frame(Cpu& c, Id view, const Rect& r)
{
    ViewState& v = views().get(view);
    bool resized = v.frame.w != r.w || v.frame.h != r.h;
    v.frame = r;
    v.bounds.w = r.w;
    v.bounds.h = r.h;
    layer_set_frame(v.layer, r);
    if (resized) v.needs_layout = true;
}

uint64_t send_point(Cpu& c, Id obj, const char* sel, double x, double y, uint64_t arg)
{
    GuestAddr imp = objc(c).resolve_send(c, obj, objc(c).sel(sel));
    if (!imp || imp == c.rt.hle.ret_instruction()) return 0;
    c.set_d(0, x);
    c.set_d(1, y);
    return c.call(imp, {obj, objc(c).sel(sel), arg});
}

void convert_point(Id from, Id to, double& x, double& y)
{
    if (from) point_to_window(from, x, y);
    if (to) point_from_window(to, x, y);
}

bool is_window(Cpu& c, Id view)
{
    return objc(c).is_kind_of(view, objc(c).class_named("UIWindow"));
}

Id window_of(Cpu& c, Id view)
{
    for (Id v = view; v; v = views().get(v).superview)
        if (is_window(c, v)) return v;
    return 0;
}

void notify_moved(Cpu& c, Id view)
{
    if (responds(c, view, "didMoveToSuperview")) send(c, view, "didMoveToSuperview");
    if (responds(c, view, "didMoveToWindow")) send(c, view, "didMoveToWindow");
    for (Id sub : std::vector<Id>(views().get(view).subviews))
        notify_moved(c, sub);
}

void add_subview(Cpu& c, Id parent, Id child, size_t index)
{
    if (!child || child == parent) return;
    ViewState& cv = views().get(child);
    if (cv.superview)
    {
        auto& siblings = views().get(cv.superview).subviews;
        std::erase(siblings, child);
    }
    else
    {
        objc(c).retain(child);
    }
    cv.superview = parent;
    auto& subs = views().get(parent).subviews;
    subs.insert(subs.begin() + std::min(index, subs.size()), child);
    Id pl = views().get(parent).layer, cl = cv.layer;
    if (pl && cl)
    {
        layers().get(pl).sublayers.push_back(cl);
        layers().get(cl).superlayer = pl;
    }
    views().get(parent).needs_layout = true;
    notify_moved(c, child);
}

void layout_view(Cpu& c, Id view)
{
    if (c.stopped()) return;
    ViewState& v = views().get(view);
    if (v.needs_layout)
    {
        v.needs_layout = false;
        if (v.controller && responds(c, v.controller, "viewWillLayoutSubviews")) send(c, v.controller, "viewWillLayoutSubviews");
        send(c, view, "layoutSubviews");
        if (v.controller && responds(c, v.controller, "viewDidLayoutSubviews")) send(c, v.controller, "viewDidLayoutSubviews");
    }
    for (Id sub : std::vector<Id>(views().get(view).subviews))
        layout_view(c, sub);
}

void register_responder(objc::ObjcRuntime& o)
{
    o.method("UIResponder", "nextResponder", [](Cpu& c) {
        ViewState* v = views().find(c.arg(0));
        c.ret(v ? (v->controller ? v->controller : v->superview) : 0);
    });
    o.method("UIResponder", "becomeFirstResponder", [](Cpu& c) { c.ret(1); });
    o.method("UIResponder", "resignFirstResponder", [](Cpu& c) { c.ret(1); });
    o.method("UIResponder", "canBecomeFirstResponder", [](Cpu& c) { c.ret(0); });
    o.method("UIResponder", "isFirstResponder", [](Cpu& c) { c.ret(0); });
    for (const char* sel : {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesEnded:withEvent:", "touchesCancelled:withEvent:"})
        o.method("UIResponder", sel, [](Cpu& c) {
            Id next = send(c, c.arg(0), "nextResponder");
            if (next) objc(c).send(c, next, objc::SEL(c.arg(1)), {c.arg(2), c.arg(3)});
        });
    for (const char* sel : {"pressesBegan:withEvent:", "pressesEnded:withEvent:", "motionBegan:withEvent:", "motionEnded:withEvent:"})
        o.method("UIResponder", sel, [](Cpu& c) {});
}

void register_view(objc::ObjcRuntime& o)
{
    o.class_method("UIView", "layerClass", [](Cpu& c) { c.ret(objc(c).host_class("CALayer")->addr); });
    o.class_method("UIView", "areAnimationsEnabled", [](Cpu& c) { c.ret(1); });
    for (const char* sel : {"setAnimationsEnabled:", "beginAnimations:context:", "commitAnimations", "setAnimationDuration:"})
        o.class_method("UIView", sel, [](Cpu& c) {});
    o.class_method("UIView", "performWithoutAnimation:", [](Cpu& c) { call_block(c, c.arg(2)); });
    o.class_method("UIView", "animateWithDuration:animations:", [](Cpu& c) { call_block(c, c.arg(2)); });
    o.class_method("UIView", "animateWithDuration:animations:completion:", [](Cpu& c) {
        call_block(c, c.arg(2));
        if (c.arg(3)) call_block(c, c.arg(3), {1});
    });

    o.method("UIView", "initWithFrame:", [](Cpu& c) {
        Id self = c.arg(0);
        Rect r = rect_arg(c);
        Class* k = objc(c).class_of(self);
        Id layer_class = send(c, k->addr, "layerClass");
        Id layer = send(c, send(c, layer_class, "alloc"), "init");
        ViewState& v = views().get(self);
        v.layer = layer;
        v.interactive =
            !objc(c).is_kind_of(self, objc(c).class_named("UIImageView")) && !objc(c).is_kind_of(self, objc(c).class_named("UILabel"));
        layers().get(layer).delegate = self;
        set_view_frame(c, self, r);
        c.ret(self);
    });
    o.method("UIView", "init", [](Cpu& c) {
        send_rect(c, c.arg(0), "initWithFrame:", {});
        c.ret(c.arg(0));
    });
    o.method("UIView", "initWithCoder:", [](Cpu& c) {
        send_rect(c, c.arg(0), "initWithFrame:", {});
        c.ret(c.arg(0));
    });
    o.method("UIView", "dealloc", [](Cpu& c) {
        ViewState* v = views().find(c.arg(0));
        if (v)
        {
            for (Id s : v->subviews)
                objc(c).release(c, s);
            if (v->layer) objc(c).release(c, v->layer);
            views().erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });

    o.method("UIView", "frame", [](Cpu& c) { ret_rect(c, views().get(c.arg(0)).frame); });
    o.method("UIView", "setFrame:", [](Cpu& c) { set_view_frame(c, c.arg(0), rect_arg(c)); });
    o.method("UIView", "bounds", [](Cpu& c) { ret_rect(c, views().get(c.arg(0)).bounds); });
    o.method("UIView", "setBounds:", [](Cpu& c) {
        Rect r = rect_arg(c);
        ViewState& v = views().get(c.arg(0));
        Rect f = v.frame;
        f.x += (f.w - r.w) / 2;
        f.y += (f.h - r.h) / 2;
        f.w = r.w;
        f.h = r.h;
        set_view_frame(c, c.arg(0), f);
        views().get(c.arg(0)).bounds = r;
    });
    o.method("UIView", "center", [](Cpu& c) {
        Rect f = views().get(c.arg(0)).frame;
        ret_point(c, f.x + f.w / 2, f.y + f.h / 2);
    });
    o.method("UIView", "setCenter:", [](Cpu& c) {
        Rect f = views().get(c.arg(0)).frame;
        f.x = c.d(0) - f.w / 2;
        f.y = c.d(1) - f.h / 2;
        set_view_frame(c, c.arg(0), f);
    });
    o.method("UIView", "layer", [](Cpu& c) { c.ret(views().get(c.arg(0)).layer); });
    o.method("UIView", "superview", [](Cpu& c) { c.ret(views().get(c.arg(0)).superview); });
    o.method("UIView", "subviews", [](Cpu& c) { c.ret(foundation::make_array(c, views().get(c.arg(0)).subviews)); });
    o.method("UIView", "window", [](Cpu& c) { c.ret(window_of(c, c.arg(0))); });
    o.method("UIView", "addSubview:", [](Cpu& c) { add_subview(c, c.arg(0), c.arg(2), SIZE_MAX); });
    o.method("UIView", "insertSubview:atIndex:", [](Cpu& c) { add_subview(c, c.arg(0), c.arg(2), c.arg(3)); });
    o.method("UIView", "insertSubview:belowSubview:", [](Cpu& c) {
        auto& subs = views().get(c.arg(0)).subviews;
        auto it = std::find(subs.begin(), subs.end(), c.arg(3));
        add_subview(c, c.arg(0), c.arg(2), it == subs.end() ? 0 : size_t(it - subs.begin()));
    });
    o.method("UIView", "insertSubview:aboveSubview:", [](Cpu& c) {
        auto& subs = views().get(c.arg(0)).subviews;
        auto it = std::find(subs.begin(), subs.end(), c.arg(3));
        add_subview(c, c.arg(0), c.arg(2), it == subs.end() ? SIZE_MAX : size_t(it - subs.begin()) + 1);
    });
    o.method("UIView", "removeFromSuperview", [](Cpu& c) {
        ViewState& v = views().get(c.arg(0));
        if (!v.superview) return;
        std::erase(views().get(v.superview).subviews, c.arg(0));
        v.superview = 0;
        if (responds(c, c.arg(0), "didMoveToWindow")) send(c, c.arg(0), "didMoveToWindow");
        objc(c).release(c, c.arg(0));
    });
    o.method("UIView", "bringSubviewToFront:", [](Cpu& c) {
        auto& subs = views().get(c.arg(0)).subviews;
        if (std::erase(subs, c.arg(2))) subs.push_back(c.arg(2));
    });
    o.method("UIView", "sendSubviewToBack:", [](Cpu& c) {
        auto& subs = views().get(c.arg(0)).subviews;
        if (std::erase(subs, c.arg(2))) subs.insert(subs.begin(), c.arg(2));
    });
    o.method("UIView", "isDescendantOfView:", [](Cpu& c) {
        for (Id v = c.arg(0); v; v = views().get(v).superview)
            if (v == c.arg(2)) return c.ret(1);
        c.ret(0);
    });
    o.method("UIView", "setNeedsLayout", [](Cpu& c) { views().get(c.arg(0)).needs_layout = true; });
    for (const char* sel : {"topAnchor", "bottomAnchor", "leadingAnchor", "trailingAnchor", "leftAnchor", "rightAnchor", "widthAnchor",
                            "heightAnchor", "centerXAnchor", "centerYAnchor", "firstBaselineAnchor", "lastBaselineAnchor"})
        o.method("UIView", sel, [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("NSLayoutXAxisAnchor")->addr, "sharedAnchor")); });
    for (const char* sel : {"safeAreaLayoutGuide", "layoutMarginsGuide", "readableContentGuide", "keyboardLayoutGuide"})
        o.method("UIView", sel, [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("UILayoutGuide")->addr, "sharedGuide")); });
    o.method("UIView", "constraints", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });
    o.method("UIView", "layoutIfNeeded", [](Cpu& c) { layout_view(c, c.arg(0)); });
    o.method("UIView", "layoutSubviews", [](Cpu& c) {});
    o.method("UIView", "setNeedsDisplay", [](Cpu& c) {});
    o.method("UIView", "setNeedsDisplayInRect:", [](Cpu& c) {});
    o.method("UIView", "sizeToFit", [](Cpu& c) {});
    o.method("UIView", "isHidden", [](Cpu& c) { c.ret(views().get(c.arg(0)).hidden); });
    o.method("UIView", "setHidden:", [](Cpu& c) { views().get(c.arg(0)).hidden = c.arg(2) & 1; });
    o.method("UIView", "alpha", [](Cpu& c) { c.set_d(0, views().get(c.arg(0)).alpha); });
    o.method("UIView", "setAlpha:", [](Cpu& c) { views().get(c.arg(0)).alpha = c.d(0); });
    o.method("UIView", "contentScaleFactor", [](Cpu& c) { c.set_d(0, views().get(c.arg(0)).content_scale); });
    o.method("UIView", "setContentScaleFactor:", [](Cpu& c) {
        ViewState& v = views().get(c.arg(0));
        v.content_scale = c.d(0);
        if (v.layer) layers().get(v.layer).contents_scale = c.d(0);
    });
    o.method("UIView", "tag", [](Cpu& c) { c.ret(uint64_t(views().get(c.arg(0)).tag)); });
    o.method("UIView", "setTag:", [](Cpu& c) { views().get(c.arg(0)).tag = int64_t(c.arg(2)); });
    o.method("UIView", "safeAreaInsets", [](Cpu& c) { ret_rect(c, {44, 0, 34, 0}); });
    o.method("UIView", "layoutMargins", [](Cpu& c) { ret_rect(c, {8, 8, 8, 8}); });
    o.method("UIView", "traitCollection",
             [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("UITraitCollection")->addr, "currentTraitCollection")); });
    o.method("UIView", "isUserInteractionEnabled", [](Cpu& c) { c.ret(views().get(c.arg(0)).interactive); });
    o.method("UIView", "setUserInteractionEnabled:", [](Cpu& c) { views().get(c.arg(0)).interactive = c.arg(2) & 1; });
    o.method("UIView", "isMultipleTouchEnabled", [](Cpu& c) { c.ret(1); });
    o.method("UIView", "gestureRecognizers", [](Cpu& c) { c.ret(0); });
    o.method("UIView", "backgroundColor", [](Cpu& c) { c.ret(0); });
    for (const char* sel : {"convertPoint:toView:", "convertPoint:toCoordinateSpace:"})
        o.method("UIView", sel, [](Cpu& c) {
            double x = c.d(0), y = c.d(1);
            convert_point(c.arg(0), c.arg(2), x, y);
            ret_point(c, x, y);
        });
    for (const char* sel : {"convertPoint:fromView:", "convertPoint:fromCoordinateSpace:"})
        o.method("UIView", sel, [](Cpu& c) {
            double x = c.d(0), y = c.d(1);
            convert_point(c.arg(2), c.arg(0), x, y);
            ret_point(c, x, y);
        });
    for (const char* sel : {"convertRect:toView:", "convertRect:toCoordinateSpace:"})
        o.method("UIView", sel, [](Cpu& c) {
            Rect r = rect_arg(c);
            convert_point(c.arg(0), c.arg(2), r.x, r.y);
            ret_rect(c, r);
        });
    for (const char* sel : {"convertRect:fromView:", "convertRect:fromCoordinateSpace:"})
        o.method("UIView", sel, [](Cpu& c) {
            Rect r = rect_arg(c);
            convert_point(c.arg(2), c.arg(0), r.x, r.y);
            ret_rect(c, r);
        });
    o.method("UIView", "hitTest:withEvent:", [](Cpu& c) {
        Id self = c.arg(0), event = c.arg(2);
        double x = c.d(0), y = c.d(1);
        ViewState v = views().get(self);
        if (v.hidden || !v.interactive || v.alpha < 0.01) return c.ret(0);
        if (!(send_point(c, self, "pointInside:withEvent:", x, y, event) & 0xff)) return c.ret(0);
        for (auto it = v.subviews.rbegin(); it != v.subviews.rend(); ++it)
        {
            const ViewState& s = views().get(*it);
            if (Id hit = send_point(c, *it, "hitTest:withEvent:", x - s.frame.x + s.bounds.x, y - s.frame.y + s.bounds.y, event))
                return c.ret(hit);
        }
        c.ret(self);
    });
    o.method("UIView", "pointInside:withEvent:", [](Cpu& c) {
        Rect b = views().get(c.arg(0)).bounds;
        double x = c.d(0), y = c.d(1);
        c.ret(x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h);
    });
    for (const char* sel : {"setBackgroundColor:",
                            "setMultipleTouchEnabled:",
                            "setExclusiveTouch:",
                            "setAutoresizingMask:",
                            "setAutoresizesSubviews:",
                            "setTranslatesAutoresizingMaskIntoConstraints:",
                            "setClipsToBounds:",
                            "setOpaque:",
                            "setContentMode:",
                            "setTintColor:",
                            "addGestureRecognizer:",
                            "removeGestureRecognizer:",
                            "setAccessibilityLabel:",
                            "setAccessibilityIdentifier:",
                            "setIsAccessibilityElement:",
                            "setAccessibilityTraits:",
                            "setPreservesSuperviewLayoutMargins:",
                            "setInsetsLayoutMarginsFromSafeArea:",
                            "setSemanticContentAttribute:",
                            "setOverrideUserInterfaceStyle:",
                            "addConstraint:",
                            "addConstraints:",
                            "removeConstraints:",
                            "setTransform:",
                            "addInteraction:"})
        o.method("UIView", sel, [](Cpu& c) {});
}

void register_window(objc::ObjcRuntime& o)
{
    o.method("UIWindow", "initWithWindowScene:", [](Cpu& c) {
        send_rect(c, c.arg(0), "initWithFrame:", {0, 0, kScreenW, kScreenH});
        c.ret(c.arg(0));
    });
    o.method("UIWindow", "windowScene", [](Cpu& c) { c.ret(app().scene); });
    o.method("UIWindow", "setWindowScene:", [](Cpu& c) {});
    o.method("UIWindow", "screen", [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("UIScreen")->addr, "mainScreen")); });
    o.method("UIWindow", "setScreen:", [](Cpu& c) {});
    o.method("UIWindow", "rootViewController", [](Cpu& c) { c.ret(controllers().get(c.arg(0)).view); });
    o.method("UIWindow", "setRootViewController:", [](Cpu& c) {
        Id window = c.arg(0), vc = c.arg(2);
        controllers().get(window).view = objc(c).retain(vc);
        if (!vc) return;
        Id root = controller_view(c, vc);
        if (!root) return;
        set_view_frame(c, root, views().get(window).bounds);
        add_subview(c, window, root, SIZE_MAX);
    });
    o.method("UIWindow", "makeKeyAndVisible", [](Cpu& c) {
        App& a = app();
        a.key_window = c.arg(0);
        if (std::find(a.windows.begin(), a.windows.end(), c.arg(0)) == a.windows.end()) a.windows.push_back(objc(c).retain(c.arg(0)));
        views().get(c.arg(0)).hidden = false;
        Id vc = controllers().get(c.arg(0)).view;
        if (vc)
        {
            if (responds(c, vc, "viewWillAppear:")) send(c, vc, "viewWillAppear:", {0});
            if (responds(c, vc, "viewDidAppear:")) send(c, vc, "viewDidAppear:", {0});
        }
        std::printf("[UIKit] window 0x%llx is key and visible\n", (unsigned long long)c.arg(0));
    });
    o.method("UIWindow", "makeKeyWindow", [](Cpu& c) { app().key_window = c.arg(0); });
    o.method("UIWindow", "isKeyWindow", [](Cpu& c) { c.ret(app().key_window == c.arg(0)); });
    o.method("UIWindow", "windowLevel", [](Cpu& c) { c.set_d(0, 0); });
    o.method("UIWindow", "setWindowLevel:", [](Cpu& c) {});
    o.method("UIWindow", "becomeKeyWindow", [](Cpu& c) {});
    o.method("UIWindow", "resignKeyWindow", [](Cpu& c) {});
}

void register_controller(objc::ObjcRuntime& o)
{
    o.class_method("UIViewController", "attemptRotationToDeviceOrientation", [](Cpu& c) {});
    o.method("UIViewController", "init", [](Cpu& c) {});
    o.method("UIViewController", "initWithNibName:bundle:", [](Cpu& c) {});
    o.method("UIViewController", "view", [](Cpu& c) { c.ret(controller_view(c, c.arg(0))); });
    o.method("UIViewController", "viewIfLoaded", [](Cpu& c) { c.ret(controllers().get(c.arg(0)).view); });
    o.method("UIViewController", "isViewLoaded", [](Cpu& c) { c.ret(controllers().get(c.arg(0)).view != 0); });
    o.method("UIViewController", "setView:", [](Cpu& c) {
        ControllerState& s = controllers().get(c.arg(0));
        Id old = s.view;
        s.view = objc(c).retain(c.arg(2));
        if (s.view) views().get(s.view).controller = c.arg(0);
        if (old) objc(c).release(c, old);
    });
    o.method("UIViewController", "loadView", [](Cpu& c) {
        Id v = objc(c).send(c, objc(c).host_class("UIView")->addr, "alloc");
        send_rect(c, v, "initWithFrame:", {0, 0, kScreenW, kScreenH});
        objc(c).send(c, c.arg(0), "setView:", {v});
        objc(c).release(c, v);
    });
    for (const char* sel :
         {"viewDidLoad", "viewWillAppear:", "viewDidAppear:", "viewWillDisappear:", "viewDidDisappear:", "viewWillLayoutSubviews",
          "viewDidLayoutSubviews", "setNeedsStatusBarAppearanceUpdate", "setNeedsUpdateOfHomeIndicatorAutoHidden",
          "setNeedsUpdateOfScreenEdgesDeferringSystemGestures", "setNeedsUpdateOfSupportedInterfaceOrientations",
          "setModalPresentationStyle:", "setModalTransitionStyle:", "didMoveToParentViewController:", "willMoveToParentViewController:",
          "setPreferredContentSize:", "setOverrideUserInterfaceStyle:", "viewWillTransitionToSize:withTransitionCoordinator:"})
        o.method("UIViewController", sel, [](Cpu& c) {});
    o.method("UIViewController", "prefersStatusBarHidden", [](Cpu& c) { c.ret(1); });
    o.method("UIViewController", "prefersHomeIndicatorAutoHidden", [](Cpu& c) { c.ret(0); });
    o.method("UIViewController", "shouldAutorotate", [](Cpu& c) { c.ret(1); });
    o.method("UIViewController", "supportedInterfaceOrientations", [](Cpu& c) { c.ret(2); });
    o.method("UIViewController", "preferredInterfaceOrientationForPresentation", [](Cpu& c) { c.ret(1); });
    o.method("UIViewController", "interfaceOrientation", [](Cpu& c) { c.ret(1); });
    o.method("UIViewController", "presentedViewController", [](Cpu& c) { c.ret(0); });
    o.method("UIViewController", "presentingViewController", [](Cpu& c) { c.ret(0); });
    o.method("UIViewController", "parentViewController", [](Cpu& c) { c.ret(controllers().get(c.arg(0)).parent); });
    o.method("UIViewController", "childViewControllers",
             [](Cpu& c) { c.ret(foundation::make_array(c, controllers().get(c.arg(0)).children)); });
    o.method("UIViewController", "addChildViewController:", [](Cpu& c) {
        controllers().get(c.arg(0)).children.push_back(objc(c).retain(c.arg(2)));
        controllers().get(c.arg(2)).parent = c.arg(0);
    });
    o.method("UIViewController", "removeFromParentViewController", [](Cpu& c) {
        Id parent = controllers().get(c.arg(0)).parent;
        if (!parent) return;
        std::erase(controllers().get(parent).children, c.arg(0));
        controllers().get(c.arg(0)).parent = 0;
    });
    o.method("UIViewController", "traitCollection",
             [](Cpu& c) { c.ret(objc(c).send(c, objc(c).host_class("UITraitCollection")->addr, "currentTraitCollection")); });
    o.method("UIViewController", "transitionCoordinator", [](Cpu& c) { c.ret(0); });
    o.method("UIViewController", "presentViewController:animated:completion:", [](Cpu& c) {
        if (c.arg(4)) call_block(c, c.arg(4));
    });
    o.method("UIViewController", "dismissViewControllerAnimated:completion:", [](Cpu& c) {
        if (c.arg(3)) call_block(c, c.arg(3));
    });
    o.method("UIViewController", "modalPresentationStyle", [](Cpu& c) { c.ret(0); });
    o.method("UIViewController", "preferredStatusBarStyle", [](Cpu& c) { c.ret(0); });
    o.method("UIViewController", "preferredScreenEdgesDeferringSystemGestures", [](Cpu& c) { c.ret(0); });
}

}

Id controller_view(Cpu& c, Id vc)
{
    ControllerState& s = controllers().get(vc);
    if (s.view) return s.view;
    objc(c).send(c, vc, "loadView");
    if (!controllers().get(vc).view) return 0;
    objc(c).send(c, vc, "viewDidLoad");
    return controllers().get(vc).view;
}

void point_to_window(Id view, double& x, double& y)
{
    for (Id v = view; v;)
    {
        const ViewState& s = views().get(v);
        if (!s.superview) break;
        x += s.frame.x - s.bounds.x;
        y += s.frame.y - s.bounds.y;
        v = s.superview;
    }
}

void point_from_window(Id view, double& x, double& y)
{
    double ox = 0, oy = 0;
    point_to_window(view, ox, oy);
    x -= ox;
    y -= oy;
}

void layout_pass(Cpu& c)
{
    for (Id w : std::vector<Id>(app().windows))
        layout_view(c, w);
}

void register_string_drawing(objc::ObjcRuntime& o)
{
    auto size = [](Cpu& c) {
        double n = double(foundation::to_utf16(c, c.arg(0)).size());
        ret_size(c, n * 8.5, 20.5);
    };
    for (const char* sel : {"sizeWithAttributes:", "sizeWithFont:", "sizeWithFont:constrainedToSize:"})
        o.method("NSString", sel, size);
    o.method("NSString", "boundingRectWithSize:options:attributes:context:", [](Cpu& c) {
        double n = double(foundation::to_utf16(c, c.arg(0)).size());
        ret_rect(c, {0, 0, std::min(n * 8.5, c.d(0)), 20.5});
    });
    for (const char* sel : {"drawAtPoint:withAttributes:", "drawInRect:withAttributes:"})
        o.method("NSString", sel, [](Cpu& c) {});
}

void register_views(objc::ObjcRuntime& o)
{
    register_string_drawing(o);
    register_responder(o);
    register_view(o);
    register_window(o);
    register_controller(o);
}

}
