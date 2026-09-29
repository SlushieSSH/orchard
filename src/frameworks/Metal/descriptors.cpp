#include "frameworks/Metal/descriptors.h"

#include <cfloat>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "objc/runtime.h"

namespace orchard::metal
{
using objc::Class;
using objc::objc;

namespace
{
struct Bag
{
    std::map<std::string, Value> values;
    std::map<uint64_t, Id> elements;
    std::string element_class;
};

std::recursive_mutex lock;
std::unordered_map<Id, Bag> bags;

const std::map<std::pair<std::string, std::string>, std::string>& children()
{
    static const std::map<std::pair<std::string, std::string>, std::string> m = {
        {{"MTLRenderPassDescriptor", "colorAttachments"}, "MTLRenderPassColorAttachmentDescriptor[]"},
        {{"MTLRenderPassDescriptor", "depthAttachment"}, "MTLRenderPassDepthAttachmentDescriptor"},
        {{"MTLRenderPassDescriptor", "stencilAttachment"}, "MTLRenderPassStencilAttachmentDescriptor"},
        {{"MTLRenderPipelineDescriptor", "colorAttachments"}, "MTLRenderPipelineColorAttachmentDescriptor[]"},
        {{"MTLRenderPipelineDescriptor", "vertexDescriptor"}, "MTLVertexDescriptor"},
        {{"MTLRenderPipelineDescriptor", "vertexBuffers"}, "MTLPipelineBufferDescriptor[]"},
        {{"MTLRenderPipelineDescriptor", "fragmentBuffers"}, "MTLPipelineBufferDescriptor[]"},
        {{"MTLVertexDescriptor", "attributes"}, "MTLVertexAttributeDescriptor[]"},
        {{"MTLVertexDescriptor", "layouts"}, "MTLVertexBufferLayoutDescriptor[]"},
        {{"MTLStageInputOutputDescriptor", "attributes"}, "MTLAttributeDescriptor[]"},
        {{"MTLStageInputOutputDescriptor", "layouts"}, "MTLBufferLayoutDescriptor[]"},
        {{"MTLComputePipelineDescriptor", "stageInputDescriptor"}, "MTLStageInputOutputDescriptor"},
        {{"MTLComputePipelineDescriptor", "buffers"}, "MTLPipelineBufferDescriptor[]"},
        {{"MTLDepthStencilDescriptor", "frontFaceStencil"}, "MTLStencilDescriptor"},
        {{"MTLDepthStencilDescriptor", "backFaceStencil"}, "MTLStencilDescriptor"},
    };
    return m;
}

const std::map<std::string, Value>& defaults()
{
    static const std::map<std::string, Value> m = [] {
        std::map<std::string, Value> d;
        auto x = [&](const char* k, uint64_t v) { d[k].x = v; };
        x("sampleCount", 1);
        x("rasterSampleCount", 1);
        x("mipmapLevelCount", 1);
        x("arrayLength", 1);
        x("depth", 1);
        x("width", 1);
        x("height", 1);
        x("textureType", 2);
        x("usage", 1);
        x("writeMask", 15);
        x("stepFunction", 1);
        x("stepRate", 1);
        x("maxAnisotropy", 1);
        x("depthCompareFunction", 7);
        x("stencilCompareFunction", 7);
        x("readMask", 0xffffffff);
        x("stencilWriteMask", 0xffffffff);
        x("sourceRGBBlendFactor", 1);
        x("sourceAlphaBlendFactor", 1);
        x("rasterizationEnabled", 1);
        x("normalizedCoordinates", 1);
        x("dataCollectionDefaultEnabled", 1);
        d["timeoutInterval"].d[0] = 60;
        d["clearDepth"].d[0] = 1.0;
        d["clearColor"].d[3] = 1.0;
        d["lodMaxClamp"].d[0] = FLT_MAX;
        return d;
    }();
    return m;
}

std::string property_of_getter(const std::string& sel)
{
    if (sel.size() > 2 && sel.starts_with("is") && std::isupper(uint8_t(sel[2]))) return char(std::tolower(sel[2])) + sel.substr(3);
    return sel;
}

Class* descriptor_root(Cpu& c)
{
    return objc(c).class_named("OrchardDescriptor");
}

std::string class_name(Cpu& c, Id obj)
{
    Class* k = objc(c).class_of(obj);
    for (; k; k = k->super)
        if (k->host) return k->name;
    return {};
}

Id new_object(Cpu& c, const std::string& cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}

Id child(Cpu& c, Id bag, const std::string& name)
{
    auto it = children().find({class_name(c, bag), name});
    if (it == children().end()) return 0;
    std::string cls = it->second;
    bool array = cls.ends_with("[]");
    if (array) cls.resize(cls.size() - 2);
    Id obj = new_object(c, array ? "OrchardDescriptorArray" : cls);
    std::lock_guard g(lock);
    if (array) bags[obj].element_class = cls;
    Value v;
    v.x = obj;
    v.set = true;
    bags[bag].values[name] = v;
    return obj;
}

uint64_t constant_size(uint64_t type)
{
    if (type == 53 || type == 45 || type == 49) return 1;
    if (type == 37 || type == 41 || type == 16) return 2;
    return 4;
}

uint64_t constant_bits(Cpu& c, GuestAddr p, uint64_t type)
{
    uint64_t n = constant_size(type), v = 0;
    std::memcpy(&v, c.mem.host(p), n);
    return v;
}

void accessor(Cpu& c)
{
    Id self = c.arg(0);
    std::string sel = objc(c).sel_name(c.arg(1));
    bool setter = sel.size() > 4 && sel.starts_with("set") && sel.back() == ':' && sel.find(':') == sel.size() - 1;
    if (setter)
    {
        std::string name = sel.substr(3, sel.size() - 4);
        if (name.size() < 2 || !std::isupper(uint8_t(name[1]))) name[0] = char(std::tolower(name[0]));
        Value v;
        v.set = true;
        v.x = c.arg(2);
        if (name == "clearColor")
            for (int i = 0; i < 4; ++i)
                v.d[i] = c.d(i);
        else if (name == "clearDepth" || name == "timeoutInterval")
            v.d[0] = c.d(0);
        else if (name == "lodMinClamp" || name == "lodMaxClamp")
            v.d[0] = c.s(0);
        else if (name == "label" || objc(c).class_of(v.x))
            objc(c).retain(v.x);
        std::lock_guard g(lock);
        bags[self].values[name] = v;
        return;
    }
    if (sel.find(':') != std::string::npos)
    {
        c.stop("unimplemented descriptor method -[" + class_name(c, self) + " " + sel + "]");
        return;
    }
    std::string name = property_of_getter(sel);
    Value v = prop(c, self, name);
    if (!v.set)
    {
        if (Id sub = child(c, self, name))
        {
            c.ret(sub);
            return;
        }
    }
    c.ret(v.x);
    if (name == "lodMinClamp" || name == "lodMaxClamp")
    {
        c.set_s(0, float(v.d[0]));
    }
    else
    {
        for (int i = 0; i < 4; ++i)
            c.set_d(i, v.d[i]);
    }
}

}

Value prop(Cpu& c, Id bag, const std::string& name)
{
    {
        std::lock_guard g(lock);
        auto b = bags.find(bag);
        if (b != bags.end())
            if (auto it = b->second.values.find(name); it != b->second.values.end()) return it->second;
    }
    auto d = defaults().find(name);
    return d == defaults().end() ? Value{} : d->second;
}

uint64_t prop_x(Cpu& c, Id bag, const std::string& name)
{
    return bag ? prop(c, bag, name).x : 0;
}
double prop_d(Cpu& c, Id bag, const std::string& name)
{
    return bag ? prop(c, bag, name).d[0] : 0;
}

Id prop_obj(Cpu& c, Id bag, const std::string& name)
{
    if (!bag) return 0;
    Value v = prop(c, bag, name);
    if (v.set) return v.x;
    return child(c, bag, name);
}

Id indexed(Cpu& c, Id array, uint64_t index)
{
    std::string cls;
    {
        std::lock_guard g(lock);
        Bag& b = bags[array];
        if (auto it = b.elements.find(index); it != b.elements.end()) return it->second;
        cls = b.element_class;
    }
    if (cls.empty()) return 0;
    Id e = new_object(c, cls);
    std::lock_guard g(lock);
    bags[array].elements[index] = e;
    return e;
}

std::map<std::string, Value> props(Id bag)
{
    std::lock_guard g(lock);
    auto b = bags.find(bag);
    return b == bags.end() ? std::map<std::string, Value>{} : b->second.values;
}

void set_prop(Id bag, const std::string& name, uint64_t x)
{
    Value v;
    v.x = x;
    v.set = true;
    std::lock_guard g(lock);
    bags[bag].values[name] = v;
}

bool is_descriptor(Cpu& c, Id obj)
{
    return objc(c).is_kind_of(obj, descriptor_root(c));
}

GuestAddr descriptor_accessor_stub(Cpu& c)
{
    static GuestAddr stub = c.rt.hle.make_stub("Metal descriptor accessor", accessor);
    return stub;
}

void register_descriptors(objc::ObjcRuntime& o)
{
    o.define("OrchardDescriptor", "NSObject");
    o.define("OrchardDescriptorArray", "NSObject");
    for (const char* k : {"MTLRenderPassDescriptor",
                          "MTLRenderPassAttachmentDescriptor",
                          "MTLTextureDescriptor",
                          "MTLSamplerDescriptor",
                          "MTLDepthStencilDescriptor",
                          "MTLStencilDescriptor",
                          "MTLRenderPipelineDescriptor",
                          "MTLRenderPipelineColorAttachmentDescriptor",
                          "MTLVertexDescriptor",
                          "MTLVertexAttributeDescriptor",
                          "MTLVertexBufferLayoutDescriptor",
                          "MTLComputePipelineDescriptor",
                          "MTLStageInputOutputDescriptor",
                          "MTLAttributeDescriptor",
                          "MTLBufferLayoutDescriptor",
                          "MTLCompileOptions",
                          "MTLHeapDescriptor",
                          "MTLPipelineBufferDescriptor",
                          "MTLCaptureDescriptor",
                          "MTLArgumentDescriptor",
                          "MTLBlitPassDescriptor",
                          "MTLComputePassDescriptor",
                          "MTLIndirectCommandBufferDescriptor",
                          "MTLFunctionDescriptor",
                          "MTLBinaryArchiveDescriptor",
                          "MTLTileRenderPipelineDescriptor",
                          "MTLCounterSampleBufferDescriptor",
                          "MTLRasterizationRateMapDescriptor",
                          "MTLMeshRenderPipelineDescriptor",
                          "MTLLinkedFunctions"})
        o.define(k, "OrchardDescriptor");
    for (const char* k :
         {"MTLRenderPassColorAttachmentDescriptor", "MTLRenderPassDepthAttachmentDescriptor", "MTLRenderPassStencilAttachmentDescriptor"})
        o.define(k, "MTLRenderPassAttachmentDescriptor");

    o.add_fallback([](Cpu& c, Class* cls, objc::SEL) -> GuestAddr {
        if (cls->is_meta) return 0;
        return objc(c).is_subclass(cls, descriptor_root(c)) ? descriptor_accessor_stub(c) : 0;
    });

    o.method("OrchardDescriptor", "copyWithZone:", [](Cpu& c) {
        Id copy = objc(c).alloc_instance(objc(c).class_of(c.arg(0)));
        std::lock_guard g(lock);
        bags[copy] = bags[c.arg(0)];
        c.ret(copy);
    });
    o.method("OrchardDescriptor", "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(lock);
            bags.erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });
    o.define("NSURLRequest", "OrchardDescriptor");
    o.define("NSMutableURLRequest", "NSURLRequest");
    auto request = [](Cpu& c, Id url) {
        Id r = objc(c).alloc_instance(objc(c).class_at(c.arg(0)));
        Value v;
        v.set = true;
        v.x = objc(c).retain(url);
        std::lock_guard g(lock);
        bags[r].values["URL"] = v;
        return objc(c).autorelease(c, r);
    };
    static decltype(request) s_request = request;
    o.class_method("NSURLRequest", "requestWithURL:", [](Cpu& c) { c.ret(s_request(c, c.arg(2))); });
    o.class_method("NSURLRequest", "requestWithURL:cachePolicy:timeoutInterval:", [](Cpu& c) { c.ret(s_request(c, c.arg(2))); });
    auto init_url = [](Cpu& c) {
        Value v;
        v.set = true;
        v.x = objc(c).retain(c.arg(2));
        std::lock_guard g(lock);
        bags[c.arg(0)].values["URL"] = v;
    };
    o.method("NSURLRequest", "initWithURL:", init_url);
    o.method("NSURLRequest", "initWithURL:cachePolicy:timeoutInterval:", init_url);
    auto headers = [](Cpu& c, Id req) {
        Id h = prop(c, req, "allHTTPHeaderFields").x;
        if (!h)
        {
            h = objc(c).retain(foundation::make_dict(c, {}, true));
            Value v;
            v.set = true;
            v.x = h;
            std::lock_guard g(lock);
            bags[req].values["allHTTPHeaderFields"] = v;
        }
        return h;
    };
    static decltype(headers) s_headers = headers;
    o.method("NSMutableURLRequest", "setValue:forHTTPHeaderField:", [](Cpu& c) {
        Id h = s_headers(c, c.arg(0));
        if (c.arg(2))
            objc(c).send(c, h, "setObject:forKey:", {c.arg(2), c.arg(3)});
        else
            objc(c).send(c, h, "removeObjectForKey:", {c.arg(3)});
    });
    o.method("NSMutableURLRequest", "addValue:forHTTPHeaderField:", [](Cpu& c) {
        objc(c).send(c, s_headers(c, c.arg(0)), "setObject:forKey:", {c.arg(2), c.arg(3)});
    });
    o.method("NSURLRequest",
             "valueForHTTPHeaderField:", [](Cpu& c) { c.ret(foundation::dict_lookup(c, s_headers(c, c.arg(0)), c.arg(2))); });

    o.method("OrchardDescriptorArray", "objectAtIndexedSubscript:", [](Cpu& c) { c.ret(indexed(c, c.arg(0), c.arg(2))); });
    o.method("OrchardDescriptorArray", "setObject:atIndexedSubscript:", [](Cpu& c) {
        std::lock_guard g(lock);
        bags[c.arg(0)].elements[c.arg(3)] = objc(c).retain(c.arg(2));
    });

    auto factory = [](Cpu& c) { c.ret(objc(c).autorelease(c, objc(c).alloc_instance(objc(c).class_at(c.arg(0))))); };
    o.class_method("MTLRenderPassDescriptor", "renderPassDescriptor", factory);
    o.class_method("MTLVertexDescriptor", "vertexDescriptor", factory);
    o.class_method("MTLStageInputOutputDescriptor", "stageInputOutputDescriptor", factory);
    o.class_method("MTLComputePassDescriptor", "computePassDescriptor", factory);
    o.class_method("MTLBlitPassDescriptor", "blitPassDescriptor", factory);

    auto set = [](Cpu& c, Id obj, const char* name, uint64_t v) {
        Value val;
        val.x = v;
        val.set = true;
        std::lock_guard g(lock);
        bags[obj].values[name] = val;
    };
    static decltype(set) s_set = set;
    o.class_method("MTLTextureDescriptor", "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", [](Cpu& c) {
        Id d = objc(c).autorelease(c, objc(c).alloc_instance(objc(c).host_class("MTLTextureDescriptor")));
        s_set(c, d, "pixelFormat", c.arg(2));
        s_set(c, d, "width", c.arg(3));
        s_set(c, d, "height", c.arg(4));
        if (c.arg(5) & 1)
        {
            uint64_t levels = 1;
            for (uint64_t s = std::max(c.arg(3), c.arg(4)); s > 1; s >>= 1)
                ++levels;
            s_set(c, d, "mipmapLevelCount", levels);
        }
        c.ret(d);
    });
    o.class_method("MTLTextureDescriptor", "textureCubeDescriptorWithPixelFormat:size:mipmapped:", [](Cpu& c) {
        Id d = objc(c).autorelease(c, objc(c).alloc_instance(objc(c).host_class("MTLTextureDescriptor")));
        s_set(c, d, "textureType", 5);
        s_set(c, d, "pixelFormat", c.arg(2));
        s_set(c, d, "width", c.arg(3));
        s_set(c, d, "height", c.arg(3));
        c.ret(d);
    });

    o.define("MTLFunctionConstantValues", "OrchardDescriptor");
    o.method("MTLFunctionConstantValues", "setConstantValue:type:atIndex:", [](Cpu& c) {
        Value v;
        v.set = true;
        v.x = constant_bits(c, c.arg(2), c.arg(3));
        std::lock_guard g(lock);
        bags[c.arg(0)].values["constant" + std::to_string(c.arg(4))] = v;
    });
    o.method("MTLFunctionConstantValues", "setConstantValue:type:withName:", [](Cpu& c) {
        Value v;
        v.set = true;
        v.x = constant_bits(c, c.arg(2), c.arg(3));
        std::lock_guard g(lock);
        bags[c.arg(0)].values["constant:" + foundation::to_utf8(c, c.arg(4))] = v;
    });
    o.method("MTLFunctionConstantValues", "setConstantValues:type:withRange:", [](Cpu& c) {
        std::lock_guard g(lock);
        for (uint64_t i = 0; i < c.arg(5); ++i)
        {
            Value v;
            v.set = true;
            v.x = constant_bits(c, c.arg(2) + i * constant_size(c.arg(3)), c.arg(3));
            bags[c.arg(0)].values["constant" + std::to_string(c.arg(4) + i)] = v;
        }
    });
}

}
