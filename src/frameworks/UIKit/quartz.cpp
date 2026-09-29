#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/UIKit/uikit.h"
#include "objc/runtime.h"

namespace orchard::uikit
{
using objc::objc;

namespace
{
void register_layer(objc::ObjcRuntime& o)
{
    o.method("CALayer", "init", [](Cpu& c) { layers().get(c.arg(0)); });
    o.class_method("CALayer", "layer", [](Cpu& c) {
        Id l = objc(c).send(c, objc(c).send(c, c.arg(0), "alloc"), "init");
        c.ret(objc(c).autorelease(c, l));
    });
    o.method("CALayer", "dealloc", [](Cpu& c) {
        layers().erase(c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    o.method("CALayer", "frame", [](Cpu& c) { ret_rect(c, layers().get(c.arg(0)).frame); });
    o.method("CALayer", "setFrame:", [](Cpu& c) {
        Rect r = rect_arg(c);
        LayerState& l = layers().get(c.arg(0));
        l.frame = r;
        l.bounds = {0, 0, r.w, r.h};
    });
    o.method("CALayer", "bounds", [](Cpu& c) { ret_rect(c, layers().get(c.arg(0)).bounds); });
    o.method("CALayer", "setBounds:", [](Cpu& c) { layers().get(c.arg(0)).bounds = rect_arg(c); });
    o.method("CALayer", "position", [](Cpu& c) {
        Rect f = layers().get(c.arg(0)).frame;
        ret_point(c, f.x + f.w / 2, f.y + f.h / 2);
    });
    o.method("CALayer", "setPosition:", [](Cpu& c) {
        LayerState& l = layers().get(c.arg(0));
        l.frame.x = c.d(0) - l.frame.w / 2;
        l.frame.y = c.d(1) - l.frame.h / 2;
    });
    o.method("CALayer", "contentsScale", [](Cpu& c) { c.set_d(0, layers().get(c.arg(0)).contents_scale); });
    o.method("CALayer", "setContentsScale:", [](Cpu& c) { layers().get(c.arg(0)).contents_scale = c.d(0); });
    o.method("CALayer", "delegate", [](Cpu& c) { c.ret(layers().get(c.arg(0)).delegate); });
    o.method("CALayer", "setDelegate:", [](Cpu& c) { layers().get(c.arg(0)).delegate = c.arg(2); });
    o.method("CALayer", "superlayer", [](Cpu& c) { c.ret(layers().get(c.arg(0)).superlayer); });
    o.method("CALayer", "sublayers", [](Cpu& c) {
        auto& subs = layers().get(c.arg(0)).sublayers;
        c.ret(subs.empty() ? 0 : foundation::make_array(c, subs));
    });
    o.method("CALayer", "addSublayer:", [](Cpu& c) {
        layers().get(c.arg(0)).sublayers.push_back(objc(c).retain(c.arg(2)));
        layers().get(c.arg(2)).superlayer = c.arg(0);
    });
    o.method("CALayer", "removeFromSuperlayer", [](Cpu& c) {
        LayerState& l = layers().get(c.arg(0));
        if (!l.superlayer) return;
        std::erase(layers().get(l.superlayer).sublayers, c.arg(0));
        l.superlayer = 0;
    });
    o.method("CALayer", "isHidden", [](Cpu& c) { c.ret(layers().get(c.arg(0)).hidden); });
    o.method("CALayer", "setHidden:", [](Cpu& c) { layers().get(c.arg(0)).hidden = c.arg(2) & 1; });
    o.method("CALayer", "opacity", [](Cpu& c) { c.set_s(0, 1.0f); });
    for (const char* sel : {"setOpaque:",
                            "setOpacity:",
                            "setNeedsDisplay",
                            "setNeedsLayout",
                            "layoutIfNeeded",
                            "setMasksToBounds:",
                            "setBackgroundColor:",
                            "setContents:",
                            "setContentsGravity:",
                            "setAnchorPoint:",
                            "setZPosition:",
                            "setCornerRadius:",
                            "setBorderWidth:",
                            "setBorderColor:",
                            "setShadowOpacity:",
                            "setTransform:",
                            "setAffineTransform:",
                            "removeAllAnimations",
                            "addAnimation:forKey:",
                            "setDrawsAsynchronously:",
                            "setAllowsGroupOpacity:",
                            "setRasterizationScale:",
                            "setShouldRasterize:",
                            "setActions:"})
        o.method("CALayer", sel, [](Cpu& c) {});

    o.method("CAMetalLayer", "device", [](Cpu& c) { c.ret(layers().get(c.arg(0)).device); });
    o.method("CAMetalLayer", "setDevice:", [](Cpu& c) { layers().get(c.arg(0)).device = c.arg(2); });
    o.method("CAMetalLayer", "preferredDevice", [](Cpu& c) { c.ret(layers().get(c.arg(0)).device); });
    o.method("CAMetalLayer", "pixelFormat", [](Cpu& c) { c.ret(layers().get(c.arg(0)).pixel_format); });
    o.method("CAMetalLayer", "setPixelFormat:", [](Cpu& c) { layers().get(c.arg(0)).pixel_format = c.arg(2); });
    o.method("CAMetalLayer", "framebufferOnly", [](Cpu& c) { c.ret(layers().get(c.arg(0)).framebuffer_only); });
    o.method("CAMetalLayer", "setFramebufferOnly:", [](Cpu& c) { layers().get(c.arg(0)).framebuffer_only = c.arg(2) & 1; });
    o.method("CAMetalLayer", "drawableSize", [](Cpu& c) {
        LayerState& l = layers().get(c.arg(0));
        double w = l.drawable_w ? l.drawable_w : l.bounds.w * l.contents_scale;
        double h = l.drawable_h ? l.drawable_h : l.bounds.h * l.contents_scale;
        ret_size(c, w, h);
    });
    o.method("CAMetalLayer", "setDrawableSize:", [](Cpu& c) {
        LayerState& l = layers().get(c.arg(0));
        l.drawable_w = c.d(0);
        l.drawable_h = c.d(1);
    });
    o.method("CAMetalLayer", "maximumDrawableCount", [](Cpu& c) { c.ret(layers().get(c.arg(0)).max_drawables); });
    o.method("CAMetalLayer", "setMaximumDrawableCount:", [](Cpu& c) { layers().get(c.arg(0)).max_drawables = c.arg(2); });
    o.method("CAMetalLayer", "presentsWithTransaction", [](Cpu& c) { c.ret(0); });
    o.method("CAMetalLayer", "allowsNextDrawableTimeout", [](Cpu& c) { c.ret(1); });
    o.method("CAMetalLayer", "colorspace", [](Cpu& c) { c.ret(0); });
    o.method("CAMetalLayer", "wantsExtendedDynamicRangeContent", [](Cpu& c) { c.ret(0); });
    for (const char* sel : {"setPresentsWithTransaction:", "setAllowsNextDrawableTimeout:", "setColorspace:", "setDisplaySyncEnabled:",
                            "setWantsExtendedDynamicRangeContent:", "setEDRMetadata:", "setDeveloperHUDProperties:"})
        o.method("CAMetalLayer", sel, [](Cpu& c) {});

    for (const char* sel : {"begin", "commit", "flush", "setDisableActions:", "setAnimationDuration:", "setCompletionBlock:"})
        o.class_method("CATransaction", sel, [](Cpu& c) {});
    o.rt.hle.fn("_CACurrentMediaTime",
                [](Cpu& c) { c.set_d(0, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()); });
}

}

void register_quartz(objc::ObjcRuntime& o)
{
    o.define("CATransaction", "NSObject");
    register_layer(o);
}

}
