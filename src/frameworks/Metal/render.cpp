#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Metal/astc.h"
#include "frameworks/Metal/internal.h"
#include "frameworks/Metal/msl.h"
#include "frameworks/UIKit/uikit.h"
#include "host/launcher.h"
#include "host/window.h"
#include "frameworks/libSystem/blocks.h"
#include "objc/runtime.h"

namespace orchard::metal::detail
{
using objc::objc;

namespace
{
Id make(Cpu& c, const char* cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}

template <typename T> void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

struct Shader
{
    msl::Result r;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3DBlob* code = nullptr;
    std::vector<std::pair<int, ID3D11SamplerState*>> const_samplers;
};

std::map<std::string, Shader*> shader_cache;

ID3D11SamplerState* create_sampler(D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE u, D3D11_TEXTURE_ADDRESS_MODE v,
                                   D3D11_TEXTURE_ADDRESS_MODE w, UINT aniso, D3D11_COMPARISON_FUNC cmp, float min_lod, float max_lod)
{
    D3D11_SAMPLER_DESC d{};
    d.Filter = filter;
    d.AddressU = u;
    d.AddressV = v;
    d.AddressW = w;
    d.MaxAnisotropy = std::clamp<UINT>(aniso, 1, 16);
    d.ComparisonFunc = cmp;
    d.MinLOD = min_lod;
    d.MaxLOD = max_lod;
    ID3D11SamplerState* s = nullptr;
    d3d()->CreateSamplerState(&d, &s);
    return s;
}

D3D11_FILTER make_filter(bool min_lin, bool mag_lin, bool mip_lin, bool compare, bool aniso)
{
    if (aniso) return compare ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
    int f = (min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | (mip_lin ? 0x1 : 0);
    return D3D11_FILTER(f | (compare ? 0x80 : 0));
}

ID3D11SamplerState* const_sampler(const msl::ConstSampler& s)
{
    static const D3D11_TEXTURE_ADDRESS_MODE kAddr[] = {D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP,
                                                       D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_BORDER};
    D3D11_TEXTURE_ADDRESS_MODE a = kAddr[std::clamp(s.address, 0, 3)];
    return create_sampler(make_filter(s.linear_min, s.linear_mag, s.linear_mip, s.compare != 0, false), a, a, a, 1,
                          s.compare ? D3D11_COMPARISON_FUNC(s.compare) : D3D11_COMPARISON_NEVER, 0, s.has_mip ? D3D11_FLOAT32_MAX : 0);
}

std::string ns(Cpu& c, Id s)
{
    return s ? foundation::to_utf8(c, s) : std::string();
}

std::filesystem::path shader_cache_dir()
{
    static std::filesystem::path dir = [] {
        std::filesystem::path d = local_cache_dir("shadercache");
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

std::string bytecode_key(const std::string& hlsl, const char* profile)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const char* p, size_t n) {
        for (size_t i = 0; i < n; ++i)
            h = (h ^ uint8_t(p[i])) * 1099511628211ull;
    };
    mix(hlsl.data(), hlsl.size());
    mix(profile, std::strlen(profile));
    mix("L0", 2);
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

ID3DBlob* load_blob(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return nullptr;
    auto size = f.tellg();
    if (size <= 0) return nullptr;
    ID3DBlob* blob = nullptr;
    if (FAILED(D3DCreateBlob(SIZE_T(size), &blob))) return nullptr;
    f.seekg(0);
    if (!f.read(static_cast<char*>(blob->GetBufferPointer()), size))
    {
        blob->Release();
        return nullptr;
    }
    return blob;
}

void save_blob(const std::filesystem::path& path, ID3DBlob* blob)
{
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.write(static_cast<const char*>(blob->GetBufferPointer()), std::streamsize(blob->GetBufferSize()))) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

Shader* compile(Cpu& c, Id fn, bool vertex, const std::vector<msl::Field>* vertex_outputs)
{
    Id lib = objc(c).get_associated(fn, 2);
    std::string source = ns(c, lib ? objc(c).get_associated(lib, 1) : 0);
    std::string name = ns(c, objc(c).get_associated(fn, 1));
    if (source.empty()) return nullptr;
    msl::Options opt;
    opt.vertex_outputs = vertex_outputs;
    for (auto& [k, v] : props(objc(c).get_associated(fn, 3)))
    {
        if (k.starts_with("constant:"))
            opt.named_constants[k.substr(9)] = v.x;
        else if (k.starts_with("constant"))
            opt.constants[std::stoi(k.substr(8))] = v.x;
    }
    msl::Result r = msl::translate(source, name, opt);
    auto fail = [&](const std::string& why, const std::string& text) -> Shader* {
        static std::set<std::string> reported;
        std::lock_guard g(gpu);
        if (reported.insert(why).second)
        {
            std::printf("[Metal] shader %s failed: %s\n", name.c_str(), why.substr(0, 400).c_str());
            if (const char* dir = std::getenv("ORCHARD_DUMP_SHADERS"))
            {
                std::string path = std::string(dir) + "/failed_" + std::to_string(reported.size()) + ".hlsl";
                if (FILE* f = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                }
            }
        }
        return nullptr;
    };
    if (!r.ok) return fail(r.error, source);
    if (r.vertex != vertex) return fail("wrong stage", source);
    {
        std::lock_guard g(gpu);
        if (auto it = shader_cache.find(r.hlsl); it != shader_cache.end()) return it->second;
    }
    const char* profile = vertex ? "vs_5_0" : "ps_5_0";
    std::filesystem::path cached = shader_cache_dir() / (bytecode_key(r.hlsl, profile) + ".dxbc");
    ID3DBlob *code = load_blob(cached), *err = nullptr;
    if (!code)
    {
        HRESULT hr = D3DCompile(r.hlsl.data(), r.hlsl.size(), name.c_str(), nullptr, nullptr, "main", profile,
                                D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &code, &err);
        if (FAILED(hr))
        {
            std::string msg =
                err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "D3DCompile failed";
            release(err);
            return fail(msg, r.hlsl);
        }
        release(err);
        save_blob(cached, code);
    }
    auto* s = new Shader;
    s->r = r;
    s->code = code;
    if (vertex)
        d3d()->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s->vs);
    else
        d3d()->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s->ps);
    for (auto& cs : r.const_samplers)
        s->const_samplers.push_back({cs.slot, const_sampler(cs)});
    std::lock_guard g(gpu);
    auto [it, fresh] = shader_cache.emplace(r.hlsl, s);
    return it->second;
}

DXGI_FORMAT vertex_format(uint64_t f)
{
    switch (f)
    {
    case 1: return DXGI_FORMAT_R8G8_UINT;
    case 2:
    case 3: return DXGI_FORMAT_R8G8B8A8_UINT;
    case 4: return DXGI_FORMAT_R8G8_SINT;
    case 5:
    case 6: return DXGI_FORMAT_R8G8B8A8_SINT;
    case 7: return DXGI_FORMAT_R8G8_UNORM;
    case 8:
    case 9: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 10: return DXGI_FORMAT_R8G8_SNORM;
    case 11:
    case 12: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case 13: return DXGI_FORMAT_R16G16_UINT;
    case 14:
    case 15: return DXGI_FORMAT_R16G16B16A16_UINT;
    case 16: return DXGI_FORMAT_R16G16_SINT;
    case 17:
    case 18: return DXGI_FORMAT_R16G16B16A16_SINT;
    case 19: return DXGI_FORMAT_R16G16_UNORM;
    case 20:
    case 21: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 22: return DXGI_FORMAT_R16G16_SNORM;
    case 23:
    case 24: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 25: return DXGI_FORMAT_R16G16_FLOAT;
    case 26:
    case 27: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 28: return DXGI_FORMAT_R32_FLOAT;
    case 29: return DXGI_FORMAT_R32G32_FLOAT;
    case 30: return DXGI_FORMAT_R32G32B32_FLOAT;
    case 31: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 32: return DXGI_FORMAT_R32_SINT;
    case 33: return DXGI_FORMAT_R32G32_SINT;
    case 34: return DXGI_FORMAT_R32G32B32_SINT;
    case 35: return DXGI_FORMAT_R32G32B32A32_SINT;
    case 36: return DXGI_FORMAT_R32_UINT;
    case 37: return DXGI_FORMAT_R32G32_UINT;
    case 38: return DXGI_FORMAT_R32G32B32_UINT;
    case 39: return DXGI_FORMAT_R32G32B32A32_UINT;
    case 40:
    case 41: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 42: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 45: return DXGI_FORMAT_R8_UINT;
    case 46: return DXGI_FORMAT_R8_SINT;
    case 47: return DXGI_FORMAT_R8_UNORM;
    case 48: return DXGI_FORMAT_R8_SNORM;
    case 49: return DXGI_FORMAT_R16_UINT;
    case 50: return DXGI_FORMAT_R16_SINT;
    case 51: return DXGI_FORMAT_R16_UNORM;
    case 52: return DXGI_FORMAT_R16_SNORM;
    case 53: return DXGI_FORMAT_R16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

D3D11_BLEND blend_factor(uint64_t f, bool alpha)
{
    static const D3D11_BLEND kColor[] = {D3D11_BLEND_ZERO,
                                         D3D11_BLEND_ONE,
                                         D3D11_BLEND_SRC_COLOR,
                                         D3D11_BLEND_INV_SRC_COLOR,
                                         D3D11_BLEND_SRC_ALPHA,
                                         D3D11_BLEND_INV_SRC_ALPHA,
                                         D3D11_BLEND_DEST_COLOR,
                                         D3D11_BLEND_INV_DEST_COLOR,
                                         D3D11_BLEND_DEST_ALPHA,
                                         D3D11_BLEND_INV_DEST_ALPHA,
                                         D3D11_BLEND_SRC_ALPHA_SAT,
                                         D3D11_BLEND_BLEND_FACTOR,
                                         D3D11_BLEND_INV_BLEND_FACTOR,
                                         D3D11_BLEND_BLEND_FACTOR,
                                         D3D11_BLEND_INV_BLEND_FACTOR,
                                         D3D11_BLEND_SRC1_COLOR,
                                         D3D11_BLEND_INV_SRC1_COLOR,
                                         D3D11_BLEND_SRC1_ALPHA,
                                         D3D11_BLEND_INV_SRC1_ALPHA};
    D3D11_BLEND b = f < std::size(kColor) ? kColor[f] : D3D11_BLEND_ONE;
    if (alpha)
    {
        if (b == D3D11_BLEND_SRC_COLOR) b = D3D11_BLEND_SRC_ALPHA;
        if (b == D3D11_BLEND_INV_SRC_COLOR) b = D3D11_BLEND_INV_SRC_ALPHA;
        if (b == D3D11_BLEND_DEST_COLOR) b = D3D11_BLEND_DEST_ALPHA;
        if (b == D3D11_BLEND_INV_DEST_COLOR) b = D3D11_BLEND_INV_DEST_ALPHA;
        if (b == D3D11_BLEND_SRC1_COLOR) b = D3D11_BLEND_SRC1_ALPHA;
        if (b == D3D11_BLEND_INV_SRC1_COLOR) b = D3D11_BLEND_INV_SRC1_ALPHA;
    }
    return b;
}

struct VertexSlot
{
    int slot = 0;
    uint32_t stride = 0;
    uint64_t step = 1;
    uint64_t rate = 1;
};

struct Pipeline
{
    Shader* vs = nullptr;
    Shader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11BlendState* blend = nullptr;
    std::vector<VertexSlot> slots;
    std::string blend_desc, layout_desc;
    bool ok = false;
};

std::unordered_map<Id, Pipeline> pipelines;

Pipeline build_pipeline(Cpu& c, Id desc)
{
    Pipeline p;
    Id vfn = prop_x(c, desc, "vertexFunction"), ffn = prop_x(c, desc, "fragmentFunction");
    if (!vfn) return p;
    p.vs = compile(c, vfn, true, nullptr);
    if (!p.vs) return p;
    if (ffn)
    {
        p.ps = compile(c, ffn, false, &p.vs->r.outputs);
        if (!p.ps) return p;
    }

    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    Id vd = prop_obj(c, desc, "vertexDescriptor");
    Id attrs = prop_obj(c, vd, "attributes"), layouts = prop_obj(c, vd, "layouts");
    std::set<int> used_slots;
    for (int a : p.vs->r.attributes)
    {
        Id ad = indexed(c, attrs, uint64_t(a));
        DXGI_FORMAT f = vertex_format(prop_x(c, ad, "format"));
        D3D11_INPUT_ELEMENT_DESC e{};
        e.SemanticName = "ATTRIB";
        e.SemanticIndex = UINT(a);
        if (f == DXGI_FORMAT_UNKNOWN)
        {
            e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            e.InputSlot = 31;
            e.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
            e.InstanceDataStepRate = 0;
            elems.push_back(e);
            used_slots.insert(31);
            continue;
        }
        int slot = int(prop_x(c, ad, "bufferIndex"));
        Id ld = indexed(c, layouts, uint64_t(slot));
        VertexSlot vs;
        vs.slot = slot;
        vs.stride = uint32_t(prop_x(c, ld, "stride"));
        vs.step = prop_x(c, ld, "stepFunction");
        vs.rate = std::max<uint64_t>(prop_x(c, ld, "stepRate"), 1);
        e.Format = f;
        e.InputSlot = UINT(slot);
        e.AlignedByteOffset = UINT(prop_x(c, ad, "offset"));
        e.InputSlotClass = vs.step == 1 ? D3D11_INPUT_PER_VERTEX_DATA : D3D11_INPUT_PER_INSTANCE_DATA;
        e.InstanceDataStepRate = vs.step == 1 ? 0 : vs.step == 0 ? 0x7fffffff : UINT(vs.rate);
        elems.push_back(e);
        if (used_slots.insert(slot).second) p.slots.push_back(vs);
        p.layout_desc += "a" + std::to_string(a) + ":f" + std::to_string(prop_x(c, ad, "format")) + "@" +
                         std::to_string(e.AlignedByteOffset) + "/b" + std::to_string(slot) + "s" + std::to_string(vs.stride) + " ";
    }
    if (!elems.empty() && FAILED(d3d()->CreateInputLayout(elems.data(), UINT(elems.size()), p.vs->code->GetBufferPointer(),
                                                          p.vs->code->GetBufferSize(), &p.layout)))
    {
        std::printf("[Metal] input layout rejected (%zu elements)\n", elems.size());
        return p;
    }

    D3D11_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable = prop_x(c, desc, "alphaToCoverageEnabled") != 0;
    bd.IndependentBlendEnable = TRUE;
    Id colors = prop_obj(c, desc, "colorAttachments");
    for (int i = 0; i < 8; ++i)
    {
        Id ca = indexed(c, colors, uint64_t(i));
        auto& rt = bd.RenderTarget[i];
        rt.BlendEnable = prop_x(c, ca, "blendingEnabled") != 0;
        rt.SrcBlend = blend_factor(prop_x(c, ca, "sourceRGBBlendFactor"), false);
        rt.DestBlend = blend_factor(prop_x(c, ca, "destinationRGBBlendFactor"), false);
        rt.BlendOp = D3D11_BLEND_OP(prop_x(c, ca, "rgbBlendOperation") + 1);
        rt.SrcBlendAlpha = blend_factor(prop_x(c, ca, "sourceAlphaBlendFactor"), true);
        rt.DestBlendAlpha = blend_factor(prop_x(c, ca, "destinationAlphaBlendFactor"), true);
        rt.BlendOpAlpha = D3D11_BLEND_OP(prop_x(c, ca, "alphaBlendOperation") + 1);
        uint64_t m = prop_x(c, ca, "writeMask");
        rt.RenderTargetWriteMask = UINT8(((m & 8) ? 1 : 0) | ((m & 4) ? 2 : 0) | ((m & 2) ? 4 : 0) | ((m & 1) ? 8 : 0));
    }
    d3d()->CreateBlendState(&bd, &p.blend);
    char bdesc[96];
    auto& rt0 = bd.RenderTarget[0];
    std::snprintf(bdesc, sizeof bdesc, "blend %d src %d dst %d op %d / %d %d %d mask %x", rt0.BlendEnable, rt0.SrcBlend, rt0.DestBlend,
                  rt0.BlendOp, rt0.SrcBlendAlpha, rt0.DestBlendAlpha, rt0.BlendOpAlpha, rt0.RenderTargetWriteMask);
    p.blend_desc = bdesc;
    p.ok = true;
    return p;
}

struct SamplerPair
{
    ID3D11SamplerState* plain = nullptr;
    ID3D11SamplerState* compare = nullptr;
};
std::unordered_map<Id, SamplerPair> samplers;
std::unordered_map<Id, ID3D11DepthStencilState*> depth_states;

ID3D11SamplerState* build_sampler(Cpu& c, Id d, bool compare)
{
    static const D3D11_TEXTURE_ADDRESS_MODE kAddr[] = {D3D11_TEXTURE_ADDRESS_CLAMP,  D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
                                                       D3D11_TEXTURE_ADDRESS_WRAP,   D3D11_TEXTURE_ADDRESS_MIRROR,
                                                       D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_BORDER};
    auto addr = [&](const char* k) { return kAddr[std::min<uint64_t>(prop_x(c, d, k), 5)]; };
    uint64_t mip = prop_x(c, d, "mipFilter");
    uint64_t cmp = prop_x(c, d, "compareFunction");
    uint64_t aniso = prop_x(c, d, "maxAnisotropy");
    float lo = float(prop_d(c, d, "lodMinClamp")), hi = float(prop_d(c, d, "lodMaxClamp"));
    if (mip == 0) hi = lo;
    return create_sampler(make_filter(prop_x(c, d, "minFilter") == 1, prop_x(c, d, "magFilter") == 1, mip == 2, compare, aniso > 1),
                          addr("sAddressMode"), addr("tAddressMode"), addr("rAddressMode"), UINT(aniso),
                          compare ? D3D11_COMPARISON_FUNC(cmp ? cmp + 1 : 4) : D3D11_COMPARISON_NEVER, lo, hi);
}

ID3D11DepthStencilState* build_depth_state(Cpu& c, Id d)
{
    D3D11_DEPTH_STENCIL_DESC ds{};
    uint64_t cmp = prop_x(c, d, "depthCompareFunction");
    bool write = prop_x(c, d, "depthWriteEnabled") != 0;
    ds.DepthEnable = cmp != 7 || write;
    ds.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_FUNC(cmp + 1);
    bool front_set = prop(c, d, "frontFaceStencil").set, back_set = prop(c, d, "backFaceStencil").set;
    ds.StencilEnable = front_set || back_set;
    auto face = [&](Id s, D3D11_DEPTH_STENCILOP_DESC& o) {
        o.StencilFunc = D3D11_COMPARISON_FUNC(prop_x(c, s, "stencilCompareFunction") + 1);
        o.StencilFailOp = D3D11_STENCIL_OP(prop_x(c, s, "stencilFailureOperation") + 1);
        o.StencilDepthFailOp = D3D11_STENCIL_OP(prop_x(c, s, "depthFailureOperation") + 1);
        o.StencilPassOp = D3D11_STENCIL_OP(prop_x(c, s, "depthStencilPassOperation") + 1);
    };
    if (ds.StencilEnable)
    {
        Id f = prop_obj(c, d, "frontFaceStencil"), b = prop_obj(c, d, "backFaceStencil");
        face(f, ds.FrontFace);
        face(b, ds.BackFace);
        ds.StencilReadMask = UINT8(prop_x(c, f, "readMask"));
        Value wm = prop(c, f, "writeMask");
        ds.StencilWriteMask = UINT8(wm.set ? wm.x : 0xff);
    }
    else
    {
        ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    }
    ID3D11DepthStencilState* s = nullptr;
    d3d()->CreateDepthStencilState(&ds, &s);
    return s;
}

constexpr UINT kRingSize = 64u << 20;
constexpr uint64_t kUnboundedLimit = 64u << 10;

struct Ring
{
    ID3D11Buffer* buf = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Buffer* draw_cb[2] = {};
    bool no_overwrite = false;
    ID3D11Buffer* zeros = nullptr;
    UINT head = 0;
};

Ring& ring()
{
    static Ring r = [] {
        Ring r;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kRingSize;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(d3d()->CreateBuffer(&bd, nullptr, &r.buf))) std::printf("[Metal] ring buffer creation failed\n");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.NumElements = kRingSize / 4;
        sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (r.buf) d3d()->CreateShaderResourceView(r.buf, &sv, &r.srv);
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 160;
        cd.Usage = D3D11_USAGE_DYNAMIC;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d3d()->CreateBuffer(&cd, nullptr, &r.draw_cb[0]);
        d3d()->CreateBuffer(&cd, nullptr, &r.draw_cb[1]);
        D3D11_FEATURE_DATA_D3D11_OPTIONS opts{};
        if (SUCCEEDED(d3d()->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof(opts))))
            r.no_overwrite = opts.MapNoOverwriteOnDynamicBufferSRV;
        if (!r.no_overwrite) std::printf("[Metal] no MapNoOverwriteOnDynamicBufferSRV: every draw discards the ring\n");
        D3D11_BUFFER_DESC zd{};
        zd.ByteWidth = 64;
        zd.Usage = D3D11_USAGE_DEFAULT;
        zd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        static const float z[16] = {};
        D3D11_SUBRESOURCE_DATA init{z, 0, 0};
        d3d()->CreateBuffer(&zd, &init, &r.zeros);
        return r;
    }();
    return r;
}

struct Upload
{
    const void* src;
    uint64_t size;
    UINT* out;
};

bool flush_uploads(std::vector<Upload>& ups)
{
    Ring& r = ring();
    if (!r.buf) return false;
    uint64_t total = 0;
    for (auto& u : ups)
        total += (u.size + 255) & ~uint64_t(255);
    if (total > kRingSize) return false;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!r.no_overwrite || r.head + total > kRingSize)
    {
        mode = D3D11_MAP_WRITE_DISCARD;
        r.head = 0;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx()->Map(r.buf, 0, mode, 0, &m))) return false;
    for (auto& u : ups)
    {
        std::memcpy(static_cast<uint8_t*>(m.pData) + r.head, u.src, u.size);
        *u.out = r.head;
        r.head += UINT((u.size + 255) & ~uint64_t(255));
    }
    ctx()->Unmap(r.buf, 0);
    return true;
}

struct Binding
{
    Id buffer = 0;
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;
    bool set = false;
};

struct Stage
{
    Binding buffers[31];
    Id textures[32] = {};
    Id samplers[16] = {};
};

struct RenderEncoder
{
    Id color[8] = {}, resolve[8] = {};
    Id depth = 0;
    uint64_t width = 0, height = 0;
    Id pipeline = 0, depth_state = 0;
    Stage vs, ps;
    D3D11_VIEWPORT viewport{};
    D3D11_RECT scissor{};
    uint64_t cull = 0, winding = 0, fill = 0;
    bool depth_clamp = false;
    float bias = 0, slope = 0, clamp = 0;
    uint32_t stencil_ref = 0;
    float blend_color[4] = {};
    uint64_t draws = 0, skipped = 0;
};

std::unordered_map<Id, RenderEncoder> encoders;
std::map<std::tuple<uint64_t, uint64_t, uint64_t, bool, float, float, float>, ID3D11RasterizerState*> raster_states;

ID3D11RasterizerState* raster_state(const RenderEncoder& e)
{
    auto key = std::make_tuple(e.cull, e.winding, e.fill, e.depth_clamp, e.bias, e.slope, e.clamp);
    auto& s = raster_states[key];
    if (!s)
    {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = e.fill == 1 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_MODE(e.cull + 1);
        rd.FrontCounterClockwise = e.winding == 1;
        rd.DepthBias = INT(e.bias);
        rd.SlopeScaledDepthBias = e.slope;
        rd.DepthBiasClamp = e.clamp;
        rd.DepthClipEnable = !e.depth_clamp;
        rd.ScissorEnable = TRUE;
        d3d()->CreateRasterizerState(&rd, &s);
    }
    return s;
}

ID3D11DepthStencilState* default_depth_state()
{
    static ID3D11DepthStencilState* s = [] {
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ID3D11DepthStencilState* st = nullptr;
        d3d()->CreateDepthStencilState(&ds, &st);
        return st;
    }();
    return s;
}

void begin_pass(Cpu& c, Id enc, Id rpd)
{
    RenderEncoder e;
    Id colors = prop_obj(c, rpd, "colorAttachments");
    std::lock_guard g(gpu);
    for (uint64_t i = 0; i < 8; ++i)
    {
        Id att = indexed(c, colors, i);
        Id tex = prop_x(c, att, "texture");
        if (!tex) continue;
        e.color[i] = tex;
        e.resolve[i] = prop_x(c, att, "resolveTexture");
        Texture* t = texture_of(tex);
        if (!t) continue;
        if (!e.width)
        {
            e.width = t->width;
            e.height = t->height;
        }
        if (t->rtv && prop_x(c, att, "loadAction") == 2)
        {
            Value cc = prop(c, att, "clearColor");
            float rgba[4] = {float(cc.d[0]), float(cc.d[1]), float(cc.d[2]), float(cc.d[3])};
            ctx()->ClearRenderTargetView(t->rtv, rgba);
        }
    }
    Id depth = prop_obj(c, rpd, "depthAttachment"), stencil = prop_obj(c, rpd, "stencilAttachment");
    Id dtex = prop_x(c, depth, "texture");
    if (!dtex) dtex = prop_x(c, stencil, "texture");
    if (dtex)
    {
        e.depth = dtex;
        if (Texture* t = texture_of(dtex))
        {
            if (!e.width)
            {
                e.width = t->width;
                e.height = t->height;
            }
            UINT flags = 0;
            if (prop_x(c, depth, "loadAction") == 2) flags |= D3D11_CLEAR_DEPTH;
            if (prop_x(c, stencil, "loadAction") == 2) flags |= D3D11_CLEAR_STENCIL;
            if (t->dsv && flags)
                ctx()->ClearDepthStencilView(t->dsv, flags, float(prop_d(c, depth, "clearDepth")),
                                             UINT8(prop_x(c, stencil, "clearStencil")));
        }
    }
    if (uint64_t w = prop_x(c, rpd, "renderTargetWidth")) e.width = w;
    if (uint64_t h = prop_x(c, rpd, "renderTargetHeight")) e.height = h;
    e.viewport = {0, 0, float(e.width), float(e.height), 0, 1};
    e.scissor = {0, 0, LONG(e.width), LONG(e.height)};
    encoders[enc] = std::move(e);
}

const char* dump_env()
{
    static const char* env = std::getenv("ORCHARD_METAL_DUMP");
    return env;
}

int dump_pass_index = 0;

bool dumping()
{
    static uint64_t frame = dump_env() ? std::strtoull(dump_env(), nullptr, 10) : ~0ull;
    return frames_presented == frame;
}

void dump_pass(const RenderEncoder& e)
{
    if (!dump_env() || !uikit::app().window || !dumping()) return;
    static uint64_t frame = std::strtoull(dump_env(), nullptr, 10);
    static std::string dir = std::strchr(dump_env(), ':') ? std::strchr(dump_env(), ':') + 1 : ".";
    int& pass = dump_pass_index;
    ++pass;
    Texture* dt = texture_of(e.depth);
    std::printf("[Metal] frame %llu pass %d: rt 0x%llx resolve 0x%llx %llux%llu, depth fmt %llu%s, %llu draws, %llu skipped\n",
                (unsigned long long)frame, pass, (unsigned long long)e.color[0], (unsigned long long)e.resolve[0],
                (unsigned long long)e.width, (unsigned long long)e.height, dt ? (unsigned long long)dt->format : 0ull,
                dt && dt->dsv ? "" : " (no dsv)", (unsigned long long)e.draws, (unsigned long long)e.skipped);
    for (int i = 0; i < 8; ++i)
    {
        Texture* t = texture_of(e.color[i]);
        if (!t || !t->tex) continue;
        char name[96];
        std::snprintf(name, sizeof name, "/pass%03d_c%d_%llux%llu_fmt%llu.png", pass, i, (unsigned long long)t->width,
                      (unsigned long long)t->height, (unsigned long long)t->format);
        if (!uikit::app().window->save_texture_png(t->tex, dir + name))
            std::printf("[Metal]   (format %llu not dumpable)\n", (unsigned long long)t->format);
    }
}

void end_pass(Id enc)
{
    std::lock_guard g(gpu);
    auto it = encoders.find(enc);
    if (it == encoders.end()) return;
    dump_pass(it->second);
    for (int i = 0; i < 8; ++i)
    {
        if (!it->second.resolve[i] || it->second.resolve[i] == it->second.color[i]) continue;
        Texture *s = texture_of(it->second.color[i]), *d = texture_of(it->second.resolve[i]);
        if (s && d && s->tex && d->tex && s->width == d->width && s->height == d->height) ctx()->CopyResource(d->tex, s->tex);
    }
    encoders.erase(it);
}

struct DrawCall
{
    uint64_t prim = 3;
    uint64_t start = 0, count = 0, instances = 1, base_instance = 0;
    bool indexed = false;
    uint64_t index_type = 0;
    Id index_buffer = 0;
    uint64_t index_offset = 0;
    int64_t base_vertex = 0;
};

const uint8_t* binding_data(Cpu& c, const Binding& b, uint64_t& avail)
{
    if (!b.bytes.empty())
    {
        avail = b.bytes.size();
        return b.bytes.data();
    }
    Buffer* buf = buffer_of(b.buffer);
    if (!buf || b.offset > buf->length) return nullptr;
    avail = buf->length - b.offset;
    return static_cast<const uint8_t*>(c.mem.host(buf->contents + b.offset));
}

void draw(Cpu& c, Id enc, const DrawCall& d)
{
    std::lock_guard g(gpu);
    auto eit = encoders.find(enc);
    if (eit == encoders.end() || !d.count || !d.instances) return;
    RenderEncoder& e = eit->second;
    auto pit = pipelines.find(e.pipeline);
    if (pit == pipelines.end() || !pit->second.ok)
    {
        ++draws_skipped, ++e.skipped;
        return;
    }
    Pipeline& p = pit->second;
    ID3D11DeviceContext* dc = ctx();
    Ring& r = ring();

    uint64_t max_vertex = d.start + d.count;
    std::vector<Upload> ups;
    UINT index_at = 0;
    const uint8_t* index_src = nullptr;
    uint64_t index_bytes = d.count * (d.index_type ? 4 : 2);
    if (d.indexed)
    {
        Buffer* ib = buffer_of(d.index_buffer);
        if (!ib || d.index_offset + index_bytes > ib->length)
        {
            ++draws_skipped, ++e.skipped;
            return;
        }
        index_src = static_cast<const uint8_t*>(c.mem.host(ib->contents + d.index_offset));
        uint64_t mx = 0;
        for (uint64_t i = 0; i < d.count; ++i)
        {
            uint32_t v = d.index_type ? reinterpret_cast<const uint32_t*>(index_src)[i] : reinterpret_cast<const uint16_t*>(index_src)[i];
            if ((d.index_type ? v != 0xffffffffu : v != 0xffffu) && v > mx) mx = v;
        }
        max_vertex = uint64_t(int64_t(mx) + d.base_vertex + 1);
        ups.push_back({index_src, index_bytes, &index_at});
    }
    UINT vb_at[32] = {};
    UINT vb_stride[32] = {};
    for (auto& s : p.slots)
    {
        uint64_t avail = 0;
        const uint8_t* src = binding_data(c, e.vs.buffers[s.slot], avail);
        if (!src)
        {
            ++draws_skipped, ++e.skipped;
            return;
        }
        uint64_t elems = s.step == 1 ? max_vertex : s.step == 0 ? 1 : (d.base_instance + d.instances + s.rate - 1) / s.rate;
        uint64_t size = std::min<uint64_t>(std::max<uint64_t>(elems * s.stride, s.stride), avail);
        vb_stride[s.slot] = s.stride;
        ups.push_back({src, size, &vb_at[s.slot]});
    }
    UINT offsets[2][32] = {};
    Shader* shaders[2] = {p.vs, p.ps};
    Stage* stages[2] = {&e.vs, &e.ps};
    for (int st = 0; st < 2; ++st)
    {
        if (!shaders[st]) continue;
        for (auto& [slot, need] : shaders[st]->r.buffers)
        {
            uint64_t avail = 0;
            const uint8_t* src = binding_data(c, stages[st]->buffers[slot], avail);
            if (!src) continue;
            uint64_t size = need ? std::min<uint64_t>(need, avail) : std::min(avail, kUnboundedLimit);
            ups.push_back({src, size, &offsets[st][slot]});
        }
    }
    if (!flush_uploads(ups))
    {
        ++draws_skipped, ++e.skipped;
        return;
    }

    if (dumping() && e.draws < 12)
    {
        std::printf("[Metal]   draw %llu -> rt 0x%llx: prim %llu count %llu x%llu%s, ps %p, %s\n", (unsigned long long)e.draws,
                    (unsigned long long)e.color[0], (unsigned long long)d.prim, (unsigned long long)d.count,
                    (unsigned long long)d.instances, d.indexed ? " indexed" : "", (void*)p.ps, p.blend_desc.c_str());
        std::printf("[Metal]     layout %s\n", p.layout_desc.c_str());
        for (Shader* sh : {p.vs, p.ps})
        {
            if (!sh) continue;
            std::string dir = std::strchr(dump_env(), ':') ? std::strchr(dump_env(), ':') + 1 : ".";
            char name[64];
            std::snprintf(name, sizeof name, "/%s_%p.hlsl", sh->r.vertex ? "vs" : "ps", (void*)sh);
            if (FILE* f = std::fopen((dir + name).c_str(), "wb"))
            {
                std::fwrite(sh->r.hlsl.data(), 1, sh->r.hlsl.size(), f);
                std::fclose(f);
            }
        }
        if (d.indexed && !p.slots.empty())
        {
            uint32_t first =
                d.index_type ? reinterpret_cast<const uint32_t*>(index_src)[0] : reinterpret_cast<const uint16_t*>(index_src)[0];
            uint64_t avail = 0;
            const uint8_t* vb = binding_data(c, e.vs.buffers[p.slots[0].slot], avail);
            uint64_t at = (first + d.base_vertex) * p.slots[0].stride;
            std::printf("[Metal]     vertex %u:", first);
            for (uint64_t k = 0; vb && k < p.slots[0].stride && at + k < avail; ++k)
                std::printf(" %02x", vb[at + k]);
            std::printf("\n");
        }
        if (auto ds = depth_states.find(e.depth_state); ds != depth_states.end() && ds->second)
        {
            D3D11_DEPTH_STENCIL_DESC dd;
            ds->second->GetDesc(&dd);
            std::printf(
                "[Metal]     depth %d write %d func %d | stencil %d ref %u read %x write %x front func %d pass %d fail %d zfail %d\n",
                dd.DepthEnable, dd.DepthWriteMask, dd.DepthFunc, dd.StencilEnable, e.stencil_ref, dd.StencilReadMask, dd.StencilWriteMask,
                dd.FrontFace.StencilFunc, dd.FrontFace.StencilPassOp, dd.FrontFace.StencilFailOp, dd.FrontFace.StencilDepthFailOp);
        }
        else
        {
            std::printf("[Metal]     default depth state (0x%llx)\n", (unsigned long long)e.depth_state);
        }
        for (int st = 0; st < 2; ++st)
        {
            if (!shaders[st]) continue;
            for (int t : shaders[st]->r.textures)
            {
                Id id = stages[st]->textures[t];
                Texture* tx = texture_of(id);
                std::printf("[Metal]     %s tex %d = 0x%llx %llux%llu fmt %llu%s\n", st ? "ps" : "vs", t, (unsigned long long)id,
                            tx ? (unsigned long long)tx->width : 0ull, tx ? (unsigned long long)tx->height : 0ull,
                            tx ? (unsigned long long)tx->format : 0ull, tx && tx->srv ? "" : " (no view)");
                static std::set<Id> saved;
                if (tx && tx->tex && saved.insert(id).second)
                {
                    std::string dir = std::strchr(dump_env(), ':') ? std::strchr(dump_env(), ':') + 1 : ".";
                    char name[64];
                    std::snprintf(name, sizeof name, "/tex_%llx.png", (unsigned long long)id);
                    uikit::app().window->save_texture_png(tx->tex, dir + name);
                }
            }
            for (auto& [slot, need] : shaders[st]->r.buffers)
            {
                std::printf("[Metal]     %s buf %d: %llu bytes%s", st ? "ps" : "vs", slot, (unsigned long long)need,
                            stages[st]->buffers[slot].set ? "" : " (unbound)");
                uint64_t avail = 0;
                const uint8_t* src = binding_data(c, stages[st]->buffers[slot], avail);
                for (uint64_t k = 0; src && k < std::min<uint64_t>({avail, need ? need : 16, 32}); k += 4)
                {
                    float f;
                    std::memcpy(&f, src + k, 4);
                    std::printf(" %g", f);
                }
                std::printf("\n");
            }
        }
    }

    ID3D11RenderTargetView* rtvs[8] = {};
    for (int i = 0; i < 8; ++i)
        if (Texture* t = texture_of(e.color[i])) rtvs[i] = t->rtv;
    Texture* dt = texture_of(e.depth);
    dc->OMSetRenderTargets(8, rtvs, dt ? dt->dsv : nullptr);
    dc->OMSetBlendState(p.blend, e.blend_color, 0xffffffff);
    auto ds = depth_states.find(e.depth_state);
    dc->OMSetDepthStencilState(ds != depth_states.end() && ds->second ? ds->second : default_depth_state(), e.stencil_ref);
    dc->RSSetState(raster_state(e));
    dc->RSSetViewports(1, &e.viewport);
    dc->RSSetScissorRects(1, &e.scissor);

    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(std::min<uint64_t>(d.prim, 4) + 1));
    dc->IASetInputLayout(p.layout);
    for (auto& s : p.slots)
        dc->IASetVertexBuffers(UINT(s.slot), 1, &r.buf, &vb_stride[s.slot], &vb_at[s.slot]);
    UINT zero = 0;
    dc->IASetVertexBuffers(31, 1, &r.zeros, &zero, &zero);
    if (d.indexed) dc->IASetIndexBuffer(r.buf, d.index_type ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, index_at);

    for (int st = 0; st < 2; ++st)
    {
        Shader* sh = shaders[st];
        ID3D11ShaderResourceView* srvs[96] = {};
        ID3D11SamplerState* smp[16] = {};
        UINT max_srv = 0, max_smp = 0;
        if (sh)
        {
            for (int t : sh->r.textures)
            {
                if (t < 0 || t >= 32) continue;
                if (Texture* tx = texture_of(stages[st]->textures[t])) srvs[t] = tx->srv;
                max_srv = std::max<UINT>(max_srv, UINT(t) + 1);
            }
            for (auto& [slot, need] : sh->r.buffers)
            {
                srvs[msl::kBufferRegisterBase + slot] = r.srv;
                max_srv = std::max<UINT>(max_srv, UINT(msl::kBufferRegisterBase + slot) + 1);
            }
            for (int s : sh->r.samplers)
            {
                if (s < 0 || s >= 16) continue;
                auto it = samplers.find(stages[st]->samplers[s]);
                bool cmp = std::find(sh->r.compare_samplers.begin(), sh->r.compare_samplers.end(), s) != sh->r.compare_samplers.end();
                if (it != samplers.end()) smp[s] = cmp ? it->second.compare : it->second.plain;
                max_smp = std::max<UINT>(max_smp, UINT(s) + 1);
            }
            for (auto& [slot, sampler] : sh->const_samplers)
            {
                smp[slot] = sampler;
                max_smp = std::max<UINT>(max_smp, UINT(slot) + 1);
            }
        }
        if (st == 0)
        {
            dc->VSSetShader(sh ? sh->vs : nullptr, nullptr, 0);
            if (max_srv) dc->VSSetShaderResources(0, max_srv, srvs);
            if (max_smp) dc->VSSetSamplers(0, max_smp, smp);
        }
        else
        {
            dc->PSSetShader(sh ? sh->ps : nullptr, nullptr, 0);
            if (max_srv) dc->PSSetShaderResources(0, max_srv, srvs);
            if (max_smp) dc->PSSetSamplers(0, max_smp, smp);
        }
    }
    dc->GSSetShader(nullptr, nullptr, 0);
    dc->HSSetShader(nullptr, nullptr, 0);
    dc->DSSetShader(nullptr, nullptr, 0);

    for (int st = 0; st < 2; ++st)
    {
        if (!shaders[st]) continue;
        ID3D11Buffer* cb = r.draw_cb[st];
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(dc->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        auto* u = static_cast<uint32_t*>(m.pData);
        std::memcpy(u, offsets[st], sizeof(offsets[st]));
        u[32] = uint32_t(d.base_vertex);
        u[33] = uint32_t(d.base_instance);
        dc->Unmap(cb, 0);
        if (st == 0)
            dc->VSSetConstantBuffers(0, 1, &cb);
        else
            dc->PSSetConstantBuffers(0, 1, &cb);
    }

    if (d.indexed)
        dc->DrawIndexedInstanced(UINT(d.count), UINT(d.instances), 0, INT(d.base_vertex), UINT(d.base_instance));
    else
        dc->DrawInstanced(UINT(d.count), UINT(d.instances), UINT(d.start), UINT(d.base_instance));
    ++draws_done, ++e.draws;
    if (dumping() && e.draws <= 12 && uikit::app().window)
    {
        std::string dir = std::strchr(dump_env(), ':') ? std::strchr(dump_env(), ':') + 1 : ".";
        char name[64];
        std::snprintf(name, sizeof name, "/pass%03d_draw%02llu.png", dump_pass_index + 1, (unsigned long long)e.draws);
        if (Texture* t = texture_of(e.color[0]); t && t->tex) uikit::app().window->save_texture_png(t->tex, dir + name);
    }
}

RenderEncoder* enc_of(Cpu& c)
{
    auto it = encoders.find(c.arg(0));
    return it == encoders.end() ? nullptr : &it->second;
}

uint64_t stack_arg(Cpu& c, int i)
{
    return c.mem.read<uint64_t>(c.sp() + uint64_t(i) * 8);
}

void upload(Cpu& c, Id tex, uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t level, uint64_t slice, GuestAddr bytes, uint64_t bpr)
{
    std::lock_guard g(gpu);
    Texture* t = texture_of(tex);
    if (!t || !t->tex || !w || !h) return;
    uint64_t mw = std::max<uint64_t>(t->width >> level, 1), mh = std::max<uint64_t>(t->height >> level, 1);
    if (x >= mw || y >= mh) return;
    w = std::min(w, mw - x);
    h = std::min(h, mh - y);
    UINT sub = D3D11CalcSubresource(UINT(level), UINT(slice), UINT(t->mips));
    D3D11_BOX box{UINT(x), UINT(y), 0, UINT(x + w), UINT(y + h), 1};
    const uint8_t* src = static_cast<const uint8_t*>(c.mem.host(bytes));
    if (t->block_w > 1)
    {
        std::vector<uint8_t> rgba(w * h * 4);
        astc::decode(src, bpr, int(w), int(h), t->block_w, t->block_h, rgba.data());
        ctx()->UpdateSubresource(t->tex, sub, &box, rgba.data(), UINT(w * 4), 0);
        return;
    }
    D3D11_TEXTURE2D_DESC td;
    t->tex->GetDesc(&td);
    bool bc = td.Format >= DXGI_FORMAT_BC1_TYPELESS && td.Format <= DXGI_FORMAT_BC5_SNORM ||
              td.Format >= DXGI_FORMAT_BC6H_TYPELESS && td.Format <= DXGI_FORMAT_BC7_UNORM_SRGB;
    if (bc)
    {
        box.right = UINT(std::min<uint64_t>((x + w + 3) & ~3ull, (mw + 3) & ~3ull));
        box.bottom = UINT(std::min<uint64_t>((y + h + 3) & ~3ull, (mh + 3) & ~3ull));
    }
    ctx()->UpdateSubresource(t->tex, sub, &box, src, UINT(bpr), 0);
}

struct Region
{
    uint64_t x, y, z, w, h, d;
};

Region region_at(Cpu& c, GuestAddr p)
{
    Region r;
    std::memcpy(&r, c.mem.host(p), sizeof(r));
    return r;
}

}

void register_render(orchard::objc::ObjcRuntime& o)
{
    const char* D = "OrchardMTLDevice";
    auto new_pipeline = [](Cpu& c, Id desc) {
        Id p = make(c, "OrchardMTLRenderPipelineState");
        Pipeline pl = build_pipeline(c, desc);
        std::lock_guard g(gpu);
        pipelines[p] = pl;
        return p;
    };
    static decltype(new_pipeline) s_new_pipeline = new_pipeline;
    o.method(D, "newRenderPipelineStateWithDescriptor:error:", [](Cpu& c) {
        if (c.arg(3)) c.mem.write<uint64_t>(c.arg(3), 0);
        c.ret(s_new_pipeline(c, c.arg(2)));
    });
    o.method(D, "newRenderPipelineStateWithDescriptor:options:reflection:error:", [](Cpu& c) {
        if (c.arg(4)) c.mem.write<uint64_t>(c.arg(4), 0);
        if (c.arg(5)) c.mem.write<uint64_t>(c.arg(5), 0);
        c.ret(s_new_pipeline(c, c.arg(2)));
    });
    o.method(D, "newRenderPipelineStateWithDescriptor:completionHandler:", [](Cpu& c) {
        Id p = s_new_pipeline(c, c.arg(2));
        call_block(c, c.arg(3), {p, 0});
        objc(c).release(c, p);
    });
    o.method(D, "newRenderPipelineStateWithDescriptor:options:completionHandler:", [](Cpu& c) {
        Id p = s_new_pipeline(c, c.arg(2));
        call_block(c, c.arg(4), {p, 0, 0});
        objc(c).release(c, p);
    });
    o.method("OrchardMTLRenderPipelineState", "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(gpu);
            auto it = pipelines.find(c.arg(0));
            if (it != pipelines.end())
            {
                release(it->second.layout);
                release(it->second.blend);
                pipelines.erase(it);
            }
        }
        objc(c).dispose(c.arg(0));
    });

    o.method(D, "newSamplerStateWithDescriptor:", [](Cpu& c) {
        Id s = make(c, "OrchardMTLSamplerState");
        SamplerPair st{build_sampler(c, c.arg(2), false), build_sampler(c, c.arg(2), true)};
        std::lock_guard g(gpu);
        samplers[s] = st;
        c.ret(s);
    });
    o.method("OrchardMTLSamplerState", "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(gpu);
            if (auto it = samplers.find(c.arg(0)); it != samplers.end())
            {
                release(it->second.plain);
                release(it->second.compare);
                samplers.erase(it);
            }
        }
        objc(c).dispose(c.arg(0));
    });
    o.method(D, "newDepthStencilStateWithDescriptor:", [](Cpu& c) {
        Id s = make(c, "OrchardMTLDepthStencilState");
        ID3D11DepthStencilState* st = build_depth_state(c, c.arg(2));
        std::lock_guard g(gpu);
        depth_states[s] = st;
        c.ret(s);
    });
    o.method("OrchardMTLDepthStencilState", "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(gpu);
            if (auto it = depth_states.find(c.arg(0)); it != depth_states.end())
            {
                release(it->second);
                depth_states.erase(it);
            }
        }
        objc(c).dispose(c.arg(0));
    });

    const char* T = "OrchardMTLTexture";
    o.method(T, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", [](Cpu& c) {
        Region r = region_at(c, c.arg(2));
        upload(c, c.arg(0), r.x, r.y, r.w, r.h, c.arg(3), 0, c.arg(4), c.arg(5));
    });
    o.method(T, "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:", [](Cpu& c) {
        Region r = region_at(c, c.arg(2));
        upload(c, c.arg(0), r.x, r.y, r.w, r.h, c.arg(3), c.arg(4), c.arg(5), c.arg(6));
    });
    o.method(T, "getBytes:bytesPerRow:fromRegion:mipmapLevel:", [](Cpu& c) {});

    const char* CB = "OrchardMTLCommandBuffer";
    o.method(CB, "renderCommandEncoderWithDescriptor:", [](Cpu& c) {
        Id e = objc(c).autorelease(c, make(c, "OrchardMTLRenderCommandEncoder"));
        begin_pass(c, e, c.arg(2));
        c.ret(e);
    });
    o.method(CB, "blitCommandEncoder", [](Cpu& c) { c.ret(objc(c).autorelease(c, make(c, "OrchardMTLBlitCommandEncoder"))); });
    o.method(CB,
             "blitCommandEncoderWithDescriptor:", [](Cpu& c) { c.ret(objc(c).autorelease(c, make(c, "OrchardMTLBlitCommandEncoder"))); });

    const char* E = "OrchardMTLRenderCommandEncoder";
    o.method(E, "endEncoding", [](Cpu& c) { end_pass(c.arg(0)); });
    o.method(E, "setRenderPipelineState:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->pipeline = c.arg(2);
    });
    for (int st = 0; st < 2; ++st)
    {
        std::string P = st == 0 ? "setVertex" : "setFragment";
        auto stage = [](RenderEncoder* e, int st) -> Stage& { return st == 0 ? e->vs : e->ps; };
        static decltype(stage) s_stage = stage;
        auto bind_buffer = [](Cpu& c, int st, Id buf, uint64_t off, uint64_t idx) {
            std::lock_guard g(gpu);
            auto* e = enc_of(c);
            if (!e || idx >= 31) return;
            Binding& b = s_stage(e, st).buffers[idx];
            b.buffer = buf;
            b.offset = off;
            b.bytes.clear();
            b.set = true;
        };
        static decltype(bind_buffer) s_bind = bind_buffer;
        if (st == 0)
        {
            o.method(E, P + "Buffer:offset:atIndex:", [](Cpu& c) { s_bind(c, 0, c.arg(2), c.arg(3), c.arg(4)); });
            o.method(E, P + "BufferOffset:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 31) e->vs.buffers[c.arg(3)].offset = c.arg(2);
            });
            o.method(E, P + "Buffers:offsets:withRange:", [](Cpu& c) {
                for (uint64_t i = 0; i < c.arg(5); ++i)
                    s_bind(c, 0, c.mem.read<uint64_t>(c.arg(2) + i * 8), c.mem.read<uint64_t>(c.arg(3) + i * 8), c.arg(4) + i);
            });
            o.method(E, P + "Bytes:length:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                auto* e = enc_of(c);
                if (!e || c.arg(4) >= 31) return;
                Binding& b = e->vs.buffers[c.arg(4)];
                auto* src = static_cast<const uint8_t*>(c.mem.host(c.arg(2)));
                b.bytes.assign(src, src + c.arg(3));
                b.buffer = 0;
                b.offset = 0;
                b.set = true;
            });
            o.method(E, P + "Texture:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 32) e->vs.textures[c.arg(3)] = c.arg(2);
            });
            o.method(E, P + "Textures:withRange:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c))
                    for (uint64_t i = 0; i < c.arg(4) && c.arg(3) + i < 32; ++i)
                        e->vs.textures[c.arg(3) + i] = c.mem.read<uint64_t>(c.arg(2) + i * 8);
            });
            o.method(E, P + "SamplerState:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 16) e->vs.samplers[c.arg(3)] = c.arg(2);
            });
            o.method(E, P + "SamplerState:lodMinClamp:lodMaxClamp:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 16) e->vs.samplers[c.arg(3)] = c.arg(2);
            });
            o.method(E, P + "SamplerStates:withRange:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c))
                    for (uint64_t i = 0; i < c.arg(4) && c.arg(3) + i < 16; ++i)
                        e->vs.samplers[c.arg(3) + i] = c.mem.read<uint64_t>(c.arg(2) + i * 8);
            });
        }
        else
        {
            o.method(E, P + "Buffer:offset:atIndex:", [](Cpu& c) { s_bind(c, 1, c.arg(2), c.arg(3), c.arg(4)); });
            o.method(E, P + "BufferOffset:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 31) e->ps.buffers[c.arg(3)].offset = c.arg(2);
            });
            o.method(E, P + "Buffers:offsets:withRange:", [](Cpu& c) {
                for (uint64_t i = 0; i < c.arg(5); ++i)
                    s_bind(c, 1, c.mem.read<uint64_t>(c.arg(2) + i * 8), c.mem.read<uint64_t>(c.arg(3) + i * 8), c.arg(4) + i);
            });
            o.method(E, P + "Bytes:length:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                auto* e = enc_of(c);
                if (!e || c.arg(4) >= 31) return;
                Binding& b = e->ps.buffers[c.arg(4)];
                auto* src = static_cast<const uint8_t*>(c.mem.host(c.arg(2)));
                b.bytes.assign(src, src + c.arg(3));
                b.buffer = 0;
                b.offset = 0;
                b.set = true;
            });
            o.method(E, P + "Texture:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 32) e->ps.textures[c.arg(3)] = c.arg(2);
            });
            o.method(E, P + "Textures:withRange:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c))
                    for (uint64_t i = 0; i < c.arg(4) && c.arg(3) + i < 32; ++i)
                        e->ps.textures[c.arg(3) + i] = c.mem.read<uint64_t>(c.arg(2) + i * 8);
            });
            o.method(E, P + "SamplerState:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 16) e->ps.samplers[c.arg(3)] = c.arg(2);
            });
            o.method(E, P + "SamplerState:lodMinClamp:lodMaxClamp:atIndex:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c); e && c.arg(3) < 16) e->ps.samplers[c.arg(3)] = c.arg(2);
            });
            o.method(E, P + "SamplerStates:withRange:", [](Cpu& c) {
                std::lock_guard g(gpu);
                if (auto* e = enc_of(c))
                    for (uint64_t i = 0; i < c.arg(4) && c.arg(3) + i < 16; ++i)
                        e->ps.samplers[c.arg(3) + i] = c.mem.read<uint64_t>(c.arg(2) + i * 8);
            });
        }
    }
    o.method(E, "setViewport:", [](Cpu& c) {
        double v[6];
        std::memcpy(v, c.mem.host(c.arg(2)), sizeof(v));
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c))
            e->viewport = {float(v[0]), float(v[1]), float(v[2]), float(v[3]), float(std::min(v[4], v[5])), float(std::max(v[4], v[5]))};
    });
    o.method(E, "setViewports:count:", [](Cpu& c) {
        double v[6];
        std::memcpy(v, c.mem.host(c.arg(2)), sizeof(v));
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c); e && c.arg(3))
            e->viewport = {float(v[0]), float(v[1]), float(v[2]), float(v[3]), float(std::min(v[4], v[5])), float(std::max(v[4], v[5]))};
    });
    auto scissor = [](Cpu& c) {
        Region r{};
        std::memcpy(&r, c.mem.host(c.arg(2)), 32);
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c))
        {
            uint64_t x0 = std::min(r.x, e->width), y0 = std::min(r.y, e->height);
            e->scissor = {LONG(x0), LONG(y0), LONG(std::min(r.x + r.z, e->width)), LONG(std::min(r.y + r.w, e->height))};
        }
    };
    o.method(E, "setScissorRect:", scissor);
    o.method(E, "setScissorRects:count:", scissor);
    o.method(E, "setCullMode:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->cull = std::min<uint64_t>(c.arg(2), 2);
    });
    o.method(E, "setFrontFacingWinding:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->winding = c.arg(2);
    });
    o.method(E, "setTriangleFillMode:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->fill = c.arg(2);
    });
    o.method(E, "setDepthClipMode:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->depth_clamp = c.arg(2) == 1;
    });
    o.method(E, "setDepthBias:slopeScale:clamp:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c))
        {
            e->bias = c.s(0);
            e->slope = c.s(1);
            e->clamp = c.s(2);
        }
    });
    o.method(E, "setBlendColorRed:green:blue:alpha:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c))
            for (int i = 0; i < 4; ++i)
                e->blend_color[i] = c.s(i);
    });
    o.method(E, "setDepthStencilState:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->depth_state = c.arg(2);
    });
    o.method(E, "setStencilReferenceValue:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->stencil_ref = uint32_t(c.arg(2));
    });
    o.method(E, "setStencilFrontReferenceValue:backReferenceValue:", [](Cpu& c) {
        std::lock_guard g(gpu);
        if (auto* e = enc_of(c)) e->stencil_ref = uint32_t(c.arg(2));
    });

    o.method(E, "drawPrimitives:vertexStart:vertexCount:", [](Cpu& c) {
        DrawCall d;
        d.prim = c.arg(2);
        d.start = c.arg(3);
        d.count = c.arg(4);
        draw(c, c.arg(0), d);
    });
    o.method(E, "drawPrimitives:vertexStart:vertexCount:instanceCount:", [](Cpu& c) {
        DrawCall d;
        d.prim = c.arg(2);
        d.start = c.arg(3);
        d.count = c.arg(4);
        d.instances = c.arg(5);
        draw(c, c.arg(0), d);
    });
    o.method(E, "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", [](Cpu& c) {
        DrawCall d;
        d.prim = c.arg(2);
        d.start = c.arg(3);
        d.count = c.arg(4);
        d.instances = c.arg(5);
        d.base_instance = c.arg(6);
        draw(c, c.arg(0), d);
    });
    auto indexed_call = [](Cpu& c) {
        DrawCall d;
        d.prim = c.arg(2);
        d.count = c.arg(3);
        d.index_type = c.arg(4);
        d.index_buffer = c.arg(5);
        d.index_offset = c.arg(6);
        d.indexed = true;
        return d;
    };
    static decltype(indexed_call) s_indexed = indexed_call;
    o.method(E,
             "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", [](Cpu& c) { draw(c, c.arg(0), s_indexed(c)); });
    o.method(E, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", [](Cpu& c) {
        DrawCall d = s_indexed(c);
        d.instances = c.arg(7);
        draw(c, c.arg(0), d);
    });
    o.method(E,
             "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:", [](Cpu& c) {
                 DrawCall d = s_indexed(c);
                 d.instances = c.arg(7);
                 d.base_vertex = int64_t(stack_arg(c, 0));
                 d.base_instance = stack_arg(c, 1);
                 draw(c, c.arg(0), d);
             });

    const char* B = "OrchardMTLBlitCommandEncoder";
    o.method(B, "endEncoding", [](Cpu& c) {});
    o.method(B, "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", [](Cpu& c) {
        std::lock_guard g(gpu);
        Buffer *s = buffer_of(c.arg(2)), *d = buffer_of(c.arg(4));
        if (s && d && c.arg(3) + c.arg(6) <= s->length && c.arg(5) + c.arg(6) <= d->length)
            std::memmove(c.mem.host(d->contents + c.arg(5)), c.mem.host(s->contents + c.arg(3)), c.arg(6));
    });
    o.method(B, "fillBuffer:range:value:", [](Cpu& c) {
        std::lock_guard g(gpu);
        Buffer* b = buffer_of(c.arg(2));
        if (b && c.arg(3) + c.arg(4) <= b->length) std::memset(c.mem.host(b->contents + c.arg(3)), int(c.arg(5)), c.arg(4));
    });
    auto buffer_to_texture = [](Cpu& c) {
        Buffer* b;
        {
            std::lock_guard g(gpu);
            b = buffer_of(c.arg(2));
        }
        if (!b) return;
        uint64_t size[3];
        std::memcpy(size, c.mem.host(c.arg(6)), sizeof(size));
        uint64_t origin[3];
        std::memcpy(origin, c.mem.host(stack_arg(c, 2)), sizeof(origin));
        upload(c, c.arg(7), origin[0], origin[1], size[0], size[1], stack_arg(c, 1), stack_arg(c, 0), b->contents + c.arg(3), c.arg(4));
    };
    o.method(B,
             "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:"
             "destinationOrigin:",
             buffer_to_texture);
    o.method(B,
             "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:"
             "destinationOrigin:options:",
             buffer_to_texture);
    o.method(B, "copyFromTexture:toTexture:", [](Cpu& c) {
        std::lock_guard g(gpu);
        Texture *s = texture_of(c.arg(2)), *d = texture_of(c.arg(3));
        if (s && d && s->tex && d->tex && s->width == d->width && s->height == d->height) ctx()->CopyResource(d->tex, s->tex);
    });
    o.method(
        B, "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
        [](Cpu& c) {
            uint64_t so[3], sz[3], dor[3];
            std::memcpy(so, c.mem.host(c.arg(5)), sizeof(so));
            std::memcpy(sz, c.mem.host(c.arg(6)), sizeof(sz));
            std::memcpy(dor, c.mem.host(stack_arg(c, 2)), sizeof(dor));
            std::lock_guard g(gpu);
            Texture *s = texture_of(c.arg(2)), *d = texture_of(c.arg(7));
            if (!s || !d || !s->tex || !d->tex) return;
            D3D11_BOX box{UINT(so[0]), UINT(so[1]), 0, UINT(so[0] + sz[0]), UINT(so[1] + sz[1]), 1};
            ctx()->CopySubresourceRegion(d->tex, D3D11CalcSubresource(UINT(stack_arg(c, 1)), UINT(stack_arg(c, 0)), UINT(d->mips)),
                                         UINT(dor[0]), UINT(dor[1]), 0, s->tex,
                                         D3D11CalcSubresource(UINT(c.arg(4)), UINT(c.arg(3)), UINT(s->mips)), &box);
        });
    o.method(B, "generateMipmapsForTexture:", [](Cpu& c) {
        std::lock_guard g(gpu);
        Texture* t = texture_of(c.arg(2));
        if (t && t->srv && t->mips > 1) ctx()->GenerateMips(t->srv);
    });
}

}
