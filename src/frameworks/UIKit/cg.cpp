#include <cmath>
#include <cstdio>
#include <limits>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/UIKit/uikit.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard::uikit
{
namespace
{
struct Affine
{
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

Affine read_affine(Cpu& c, GuestAddr p)
{
    Affine t;
    c.mem.read_bytes(p, &t, sizeof t);
    return t;
}
void ret_affine(Cpu& c, const Affine& t)
{
    c.mem.write_bytes(c.x(8), &t, sizeof t);
}

Affine concat(const Affine& t1, const Affine& t2)
{
    return {t1.a * t2.a + t1.b * t2.c, t1.a * t2.b + t1.b * t2.d,           t1.c * t2.a + t1.d * t2.c,
            t1.c * t2.b + t1.d * t2.d, t1.tx * t2.a + t1.ty * t2.c + t2.tx, t1.tx * t2.b + t1.ty * t2.d + t2.ty};
}

Rect standardize(Rect r)
{
    if (r.w < 0) r.x += r.w, r.w = -r.w;
    if (r.h < 0) r.y += r.h, r.h = -r.h;
    return r;
}

bool is_null(const Rect& r)
{
    return std::isinf(r.x) || std::isinf(r.y);
}

GuestAddr data_doubles(Runtime& rt, std::initializer_list<double> values)
{
    GuestAddr a = rt.mem.alloc_system(values.size() * 8, 16);
    size_t i = 0;
    for (double v : values)
        rt.mem.write<double>(a + 8 * i++, v);
    return a;
}

void register_geometry(Hle& h)
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    h.data("_CGRectZero", [](Runtime& rt) { return data_doubles(rt, {0, 0, 0, 0}); });
    h.data("_CGRectNull", [](Runtime& rt) { return data_doubles(rt, {inf, inf, 0, 0}); });
    h.data("_CGPointZero", [](Runtime& rt) { return data_doubles(rt, {0, 0}); });
    h.data("_CGSizeZero", [](Runtime& rt) { return data_doubles(rt, {0, 0}); });
    h.data("_UIEdgeInsetsZero", [](Runtime& rt) { return data_doubles(rt, {0, 0, 0, 0}); });
    h.data("_CGAffineTransformIdentity", [](Runtime& rt) { return data_doubles(rt, {1, 0, 0, 1, 0, 0}); });
    h.data("_CATransform3DIdentity", [](Runtime& rt) { return data_doubles(rt, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}); });
    h.data("_UIWindowLevelNormal", [](Runtime& rt) { return data_doubles(rt, {0}); });
    h.data("_UIWindowLevelAlert", [](Runtime& rt) { return data_doubles(rt, {2000}); });
    h.data("_UIViewNoIntrinsicMetric", [](Runtime& rt) { return data_doubles(rt, {-1}); });
    h.data("_UIFontWeightBold", [](Runtime& rt) { return data_doubles(rt, {0.4}); });
    h.data("_UIBackgroundTaskInvalid", [](Runtime& rt) { return data_doubles(rt, {0}); });

    h.fn("_CGRectGetMinX", [](Cpu& c) { c.set_d(0, standardize(rect_arg(c)).x); });
    h.fn("_CGRectGetMinY", [](Cpu& c) { c.set_d(0, standardize(rect_arg(c)).y); });
    h.fn("_CGRectGetMaxX", [](Cpu& c) {
        Rect r = standardize(rect_arg(c));
        c.set_d(0, r.x + r.w);
    });
    h.fn("_CGRectGetMaxY", [](Cpu& c) {
        Rect r = standardize(rect_arg(c));
        c.set_d(0, r.y + r.h);
    });
    h.fn("_CGRectGetMidX", [](Cpu& c) {
        Rect r = standardize(rect_arg(c));
        c.set_d(0, r.x + r.w / 2);
    });
    h.fn("_CGRectGetMidY", [](Cpu& c) {
        Rect r = standardize(rect_arg(c));
        c.set_d(0, r.y + r.h / 2);
    });
    h.fn("_CGRectGetWidth", [](Cpu& c) { c.set_d(0, std::abs(c.d(2))); });
    h.fn("_CGRectGetHeight", [](Cpu& c) { c.set_d(0, std::abs(c.d(3))); });
    h.fn("_CGRectStandardize", [](Cpu& c) { ret_rect(c, standardize(rect_arg(c))); });
    h.fn("_CGRectIsEmpty", [](Cpu& c) {
        Rect r = rect_arg(c);
        c.ret(is_null(r) || r.w == 0 || r.h == 0);
    });
    h.fn("_CGRectIsNull", [](Cpu& c) { c.ret(is_null(rect_arg(c))); });
    h.fn("_CGRectEqualToRect", [](Cpu& c) {
        Rect a = standardize(rect_arg(c, 0)), b = standardize(rect_arg(c, 4));
        c.ret(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
    });
    h.fn("_CGRectOffset", [](Cpu& c) {
        Rect r = rect_arg(c);
        r.x += c.d(4);
        r.y += c.d(5);
        ret_rect(c, r);
    });
    h.fn("_CGRectInset", [](Cpu& c) {
        Rect r = standardize(rect_arg(c));
        double dx = c.d(4), dy = c.d(5);
        r = {r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy};
        if (r.w < 0 || r.h < 0) r = {inf, inf, 0, 0};
        ret_rect(c, r);
    });
    h.fn("_CGRectIntersection", [](Cpu& c) {
        Rect a = standardize(rect_arg(c, 0)), b = standardize(rect_arg(c, 4));
        double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
        double x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
        ret_rect(c, x1 < x0 || y1 < y0 ? Rect{inf, inf, 0, 0} : Rect{x0, y0, x1 - x0, y1 - y0});
    });
    h.fn("_CGRectUnion", [](Cpu& c) {
        Rect a = standardize(rect_arg(c, 0)), b = standardize(rect_arg(c, 4));
        if (is_null(a)) return ret_rect(c, b);
        if (is_null(b)) return ret_rect(c, a);
        double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        double x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
        ret_rect(c, {x0, y0, x1 - x0, y1 - y0});
    });
    h.fn("_CGRectContainsPoint", [](Cpu& c) {
        Rect r = standardize(rect_arg(c));
        double x = c.d(4), y = c.d(5);
        c.ret(x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
    });
    h.fn("_CGRectContainsRect", [](Cpu& c) {
        Rect a = standardize(rect_arg(c, 0)), b = standardize(rect_arg(c, 4));
        c.ret(b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h);
    });

    h.fn("_CGAffineTransformMakeScale", [](Cpu& c) { ret_affine(c, {c.d(0), 0, 0, c.d(1), 0, 0}); });
    h.fn("_CGAffineTransformMakeTranslation", [](Cpu& c) { ret_affine(c, {1, 0, 0, 1, c.d(0), c.d(1)}); });
    h.fn("_CGAffineTransformMakeRotation", [](Cpu& c) {
        double s = std::sin(c.d(0)), co = std::cos(c.d(0));
        ret_affine(c, {co, s, -s, co, 0, 0});
    });
    h.fn("_CGAffineTransformConcat", [](Cpu& c) { ret_affine(c, concat(read_affine(c, c.arg(0)), read_affine(c, c.arg(1)))); });
    h.fn("_CGAffineTransformTranslate", [](Cpu& c) { ret_affine(c, concat({1, 0, 0, 1, c.d(0), c.d(1)}, read_affine(c, c.arg(0)))); });
    h.fn("_CGAffineTransformRotate", [](Cpu& c) {
        double s = std::sin(c.d(0)), co = std::cos(c.d(0));
        ret_affine(c, concat({co, s, -s, co, 0, 0}, read_affine(c, c.arg(0))));
    });
    h.fn("_CGAffineTransformInvert", [](Cpu& c) {
        Affine t = read_affine(c, c.arg(0));
        double det = t.a * t.d - t.b * t.c;
        if (det == 0) return ret_affine(c, t);
        ret_affine(c, {t.d / det, -t.b / det, -t.c / det, t.a / det, (t.c * t.ty - t.d * t.tx) / det, (t.b * t.tx - t.a * t.ty) / det});
    });
    h.fn("_CGSizeApplyAffineTransform", [](Cpu& c) {
        Affine t = read_affine(c, c.arg(0));
        double w = c.d(0), hh = c.d(1);
        ret_size(c, t.a * w + t.c * hh, t.b * w + t.d * hh);
    });

    h.fn("_CGColorSpaceCreateDeviceRGB", [](Cpu& c) { c.ret(c.rt.heap.calloc(16)); });
    h.fn("_CGColorSpaceCreateWithName", [](Cpu& c) { c.ret(c.rt.heap.calloc(16)); });
    h.fn("_CGColorSpaceRelease", [](Cpu& c) {});
    h.fn("_CGColorCreate", [](Cpu& c) { c.ret(c.rt.heap.calloc(16)); });
    h.fn("_CGColorRelease", [](Cpu& c) {});

    h.fn("_CAFrameRateRangeMake", [](Cpu& c) {});
    h.fn("_CATransform3DMakeRotation", [](Cpu& c) {
        double a = c.d(0), x = c.d(1), y = c.d(2), z = c.d(3);
        double len = std::sqrt(x * x + y * y + z * z);
        if (len) x /= len, y /= len, z /= len;
        double s = std::sin(a), co = std::cos(a), t = 1 - co;
        double m[16] = {t * x * x + co,
                        t * x * y + s * z,
                        t * x * z - s * y,
                        0,
                        t * x * y - s * z,
                        t * y * y + co,
                        t * y * z + s * x,
                        0,
                        t * x * z + s * y,
                        t * y * z - s * x,
                        t * z * z + co,
                        0,
                        0,
                        0,
                        0,
                        1};
        c.mem.write_bytes(c.x(8), m, sizeof m);
    });
}

void register_uikit_functions(Hle& h)
{
    for (const char* n :
         {"_UIAccessibilityIsBoldTextEnabled", "_UIAccessibilityIsClosedCaptioningEnabled", "_UIAccessibilityIsGrayscaleEnabled",
          "_UIAccessibilityIsInvertColorsEnabled", "_UIAccessibilityIsReduceMotionEnabled", "_UIAccessibilityIsVoiceOverRunning"})
        h.fn(n, [](Cpu& c) { c.ret(0); });
    h.fn("_UIAccessibilityPostNotification", [](Cpu& c) {});
    h.fn("_UIImagePNGRepresentation", [](Cpu& c) { c.ret(0); });
    h.fn("_NSStringFromCGRect", [](Cpu& c) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "{{%g, %g}, {%g, %g}}", c.d(0), c.d(1), c.d(2), c.d(3));
        c.ret(foundation::string_autoreleased(c, buf));
    });
    h.fn("_NSStringFromCGSize", [](Cpu& c) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "{%g, %g}", c.d(0), c.d(1));
        c.ret(foundation::string_autoreleased(c, buf));
    });
    h.fn("_NSStringFromCGPoint", [](Cpu& c) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "{%g, %g}", c.d(0), c.d(1));
        c.ret(foundation::string_autoreleased(c, buf));
    });
    h.fn("_CGSizeFromString", [](Cpu& c) {
        double w = 0, hh = 0;
        std::sscanf(foundation::to_utf8(c, c.arg(0)).c_str(), "{%lf, %lf}", &w, &hh);
        ret_size(c, w, hh);
    });
    h.fn("_UIGraphicsBeginImageContextWithOptions", [](Cpu& c) {});
    h.fn("_UIGraphicsEndImageContext", [](Cpu& c) {});
    h.fn("_UIGraphicsGetImageFromCurrentImageContext", [](Cpu& c) { c.ret(0); });
    h.fn("_UIGraphicsGetCurrentContext", [](Cpu& c) { c.ret(0); });
}

}

void register_cg(Hle& h)
{
    register_geometry(h);
    register_uikit_functions(h);
}

}
