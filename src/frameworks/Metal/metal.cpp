#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/Metal/descriptors.h"
#include "frameworks/Metal/internal.h"
#include "frameworks/Metal/metal.h"
#include "frameworks/UIKit/uikit.h"
#include "frameworks/libSystem/blocks.h"
#include "host/profiler.h"
#include "host/window.h"
#include "objc/runtime.h"

namespace orchard::metal
{
using objc::Class;
using objc::objc;

namespace detail
{
std::recursive_mutex gpu;
std::unordered_map<Id, Texture> textures;
std::unordered_map<Id, Buffer> buffers;
std::atomic<uint64_t> draws_done{0}, draws_skipped{0}, frames_presented{0};

ID3D11Device* d3d()
{
    return uikit::app().window ? uikit::app().window->device() : nullptr;
}
ID3D11DeviceContext* ctx()
{
    return uikit::app().window ? uikit::app().window->context() : nullptr;
}

Texture* texture_of(Id obj)
{
    auto it = textures.find(obj);
    return it == textures.end() ? nullptr : &it->second;
}

Buffer* buffer_of(Id obj)
{
    auto it = buffers.find(obj);
    return it == buffers.end() ? nullptr : &it->second;
}

}

using namespace detail;

namespace
{
struct CommandBuffer
{
    std::vector<Id> present;
    std::vector<GuestAddr> scheduled, completed;
    uint64_t status = 0;
};

std::unordered_map<Id, CommandBuffer> command_buffers;
std::unordered_map<Id, Id> drawable_textures;

Id make(Cpu& c, const char* cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}
Id autoreleased(Cpu& c, const char* cls)
{
    return objc(c).autorelease(c, make(c, cls));
}

struct Formats
{
    DXGI_FORMAT texture = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN, depth_view = DXGI_FORMAT_UNKNOWN;
    int block_w = 1, block_h = 1;
};

Formats plain(DXGI_FORMAT f)
{
    return {f, f, DXGI_FORMAT_UNKNOWN};
}

Formats dxgi_format(uint64_t mtl)
{
    if (mtl >= 186 && mtl <= 218)
    {
        static const int dims[][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {0, 0},   {8, 5},  {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        bool srgb = mtl <= 200;
        uint64_t i = mtl - (srgb ? 186 : 204);
        if (i >= 15 || !dims[i][0]) return {};
        Formats f = plain(srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
        f.block_w = dims[i][0];
        f.block_h = dims[i][1];
        return f;
    }
    switch (mtl)
    {
    case 1: return plain(DXGI_FORMAT_A8_UNORM);
    case 10:
    case 11: return plain(DXGI_FORMAT_R8_UNORM);
    case 12: return plain(DXGI_FORMAT_R8_SNORM);
    case 13: return plain(DXGI_FORMAT_R8_UINT);
    case 14: return plain(DXGI_FORMAT_R8_SINT);
    case 20: return plain(DXGI_FORMAT_R16_UNORM);
    case 22: return plain(DXGI_FORMAT_R16_SNORM);
    case 23: return plain(DXGI_FORMAT_R16_UINT);
    case 24: return plain(DXGI_FORMAT_R16_SINT);
    case 25: return plain(DXGI_FORMAT_R16_FLOAT);
    case 30:
    case 31: return plain(DXGI_FORMAT_R8G8_UNORM);
    case 32: return plain(DXGI_FORMAT_R8G8_SNORM);
    case 33: return plain(DXGI_FORMAT_R8G8_UINT);
    case 34: return plain(DXGI_FORMAT_R8G8_SINT);
    case 40: return plain(DXGI_FORMAT_B5G6R5_UNORM);
    case 53: return plain(DXGI_FORMAT_R32_UINT);
    case 54: return plain(DXGI_FORMAT_R32_SINT);
    case 55: return plain(DXGI_FORMAT_R32_FLOAT);
    case 60: return plain(DXGI_FORMAT_R16G16_UNORM);
    case 62: return plain(DXGI_FORMAT_R16G16_SNORM);
    case 63: return plain(DXGI_FORMAT_R16G16_UINT);
    case 64: return plain(DXGI_FORMAT_R16G16_SINT);
    case 65: return plain(DXGI_FORMAT_R16G16_FLOAT);
    case 70: return plain(DXGI_FORMAT_R8G8B8A8_UNORM);
    case 71: return plain(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    case 72: return plain(DXGI_FORMAT_R8G8B8A8_SNORM);
    case 73: return plain(DXGI_FORMAT_R8G8B8A8_UINT);
    case 74: return plain(DXGI_FORMAT_R8G8B8A8_SINT);
    case 80: return plain(DXGI_FORMAT_B8G8R8A8_UNORM);
    case 81: return plain(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    case 90: return plain(DXGI_FORMAT_R10G10B10A2_UNORM);
    case 91: return plain(DXGI_FORMAT_R10G10B10A2_UINT);
    case 92: return plain(DXGI_FORMAT_R11G11B10_FLOAT);
    case 93: return plain(DXGI_FORMAT_R9G9B9E5_SHAREDEXP);
    case 103: return plain(DXGI_FORMAT_R32G32_FLOAT);
    case 104: return plain(DXGI_FORMAT_R32G32_UINT);
    case 105: return plain(DXGI_FORMAT_R32G32_SINT);
    case 110: return plain(DXGI_FORMAT_R16G16B16A16_UNORM);
    case 112: return plain(DXGI_FORMAT_R16G16B16A16_SNORM);
    case 113: return plain(DXGI_FORMAT_R16G16B16A16_UINT);
    case 114: return plain(DXGI_FORMAT_R16G16B16A16_SINT);
    case 115: return plain(DXGI_FORMAT_R16G16B16A16_FLOAT);
    case 123: return plain(DXGI_FORMAT_R32G32B32A32_UINT);
    case 124: return plain(DXGI_FORMAT_R32G32B32A32_SINT);
    case 125: return plain(DXGI_FORMAT_R32G32B32A32_FLOAT);
    case 130: return plain(DXGI_FORMAT_BC1_UNORM);
    case 131: return plain(DXGI_FORMAT_BC1_UNORM_SRGB);
    case 132: return plain(DXGI_FORMAT_BC2_UNORM);
    case 133: return plain(DXGI_FORMAT_BC2_UNORM_SRGB);
    case 134: return plain(DXGI_FORMAT_BC3_UNORM);
    case 135: return plain(DXGI_FORMAT_BC3_UNORM_SRGB);
    case 140: return plain(DXGI_FORMAT_BC4_UNORM);
    case 141: return plain(DXGI_FORMAT_BC4_SNORM);
    case 142: return plain(DXGI_FORMAT_BC5_UNORM);
    case 143: return plain(DXGI_FORMAT_BC5_SNORM);
    case 150: return plain(DXGI_FORMAT_BC6H_SF16);
    case 151: return plain(DXGI_FORMAT_BC6H_UF16);
    case 152: return plain(DXGI_FORMAT_BC7_UNORM);
    case 153: return plain(DXGI_FORMAT_BC7_UNORM_SRGB);
    case 250: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM};
    case 252: return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT};
    case 253:
    case 262: return {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT};
    case 255: return {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT};
    case 260:
    case 261: return {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    default: return {};
    }
}

void create_gpu_texture(Texture& t)
{
    Formats f = dxgi_format(t.format);
    if (!d3d()) return;
    if (f.texture == DXGI_FORMAT_UNKNOWN)
    {
        static std::mutex m;
        static std::set<uint64_t> seen;
        std::lock_guard g(m);
        if (seen.insert(t.format).second) std::printf("[Metal] unsupported pixel format %llu\n", (unsigned long long)t.format);
        return;
    }
    t.block_w = f.block_w;
    t.block_h = f.block_h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(std::max<uint64_t>(t.width, 1));
    td.Height = UINT(std::max<uint64_t>(t.height, 1));
    td.MipLevels = UINT(t.mips);
    td.ArraySize = UINT(t.type == 5 ? 6 : t.type == 6 ? 6 * std::max<uint64_t>(t.array, 1) : std::max<uint64_t>(t.array, 1));
    td.Format = f.texture;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    bool depth = f.depth_view != DXGI_FORMAT_UNKNOWN;
    bool compressed = f.texture >= DXGI_FORMAT_BC1_TYPELESS && f.texture <= DXGI_FORMAT_BC5_SNORM ||
                      f.texture >= DXGI_FORMAT_BC6H_TYPELESS && f.texture <= DXGI_FORMAT_BC7_UNORM_SRGB;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (compressed ? 0 : depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET);
    if (t.type == 5 || t.type == 6) td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (t.mips > 1 && (td.BindFlags & D3D11_BIND_RENDER_TARGET)) td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(d3d()->CreateTexture2D(&td, nullptr, &t.tex)) && (td.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS))
    {
        td.MiscFlags &= ~UINT(D3D11_RESOURCE_MISC_GENERATE_MIPS);
        d3d()->CreateTexture2D(&td, nullptr, &t.tex);
    }
    if (!t.tex)
    {
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d3d()->CreateTexture2D(&td, nullptr, &t.tex))) return;
    }
    if (depth)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = f.depth_view;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        d3d()->CreateDepthStencilView(t.tex, &dv, &t.dsv);
    }
    else if (td.BindFlags & D3D11_BIND_RENDER_TARGET)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = f.view;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        d3d()->CreateRenderTargetView(t.tex, &rv, &t.rtv);
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = f.view;
    if (t.type == 5)
    {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = UINT(t.mips);
    }
    else if (t.type == 6)
    {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        sv.TextureCubeArray.MipLevels = UINT(t.mips);
        sv.TextureCubeArray.NumCubes = td.ArraySize / 6;
    }
    else if (t.type == 3)
    {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MipLevels = UINT(t.mips);
        sv.Texture2DArray.ArraySize = td.ArraySize;
    }
    else
    {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = UINT(t.mips);
    }
    d3d()->CreateShaderResourceView(t.tex, &sv, &t.srv);
}

Id new_texture(Cpu& c, Texture t)
{
    Id obj = make(c, "OrchardMTLTexture");
    std::lock_guard g(gpu);
    create_gpu_texture(t);
    textures[obj] = t;
    return obj;
}

void present(Cpu& c, Id drawable)
{
    std::lock_guard g(gpu);
    auto it = drawable_textures.find(drawable);
    if (it == drawable_textures.end()) return;
    Texture* t = texture_of(it->second);
    if (!t || !t->tex || !uikit::app().window) return;
    uikit::app().window->show_texture(t->tex);
    uint64_t n = ++frames_presented;
    note_frame_presented();
    static auto started = std::chrono::steady_clock::now();
    if (n == 1 || n % 300 == 0)
    {
        double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("[Metal] presented frame %llu at +%.1fs (%llu draws rendered, %llu skipped)\n", (unsigned long long)n, at,
                    (unsigned long long)draws_done.load(), (unsigned long long)draws_skipped.load());
        std::fflush(stdout);
    }
}

void register_device(objc::ObjcRuntime& o)
{
    o.rt.hle.fn("_MTLCreateSystemDefaultDevice", [](Cpu& c) {
        static Id device = [&] {
            std::printf("[Metal] device created (Direct3D 11 backend)\n");
            return make(c, "OrchardMTLDevice");
        }();
        c.ret(device);
    });
    o.rt.hle.fn("_MTLCopyAllDevices", [](Cpu& c) {
        Id dev = c.call(c.rt.hle.resolve("_MTLCreateSystemDefaultDevice", "Metal"));
        c.ret(objc(c).retain(foundation::make_array(c, {dev})));
    });

    const char* D = "OrchardMTLDevice";
    o.method(D, "name", [](Cpu& c) { c.ret(foundation::string_autoreleased(c, "Apple A11 GPU")); });
    o.method(D, "registryID", [](Cpu& c) { c.ret(1); });
    o.method(D, "supportsFamily:", [](Cpu& c) {
        uint64_t f = c.arg(2);
        c.ret((f >= 1001 && f <= 1004) || f == 3001 || f == 3002);
    });
    o.method(D, "supportsFeatureSet:", [](Cpu& c) { c.ret(c.arg(2) < 10000); });
    o.method(D, "maxThreadsPerThreadgroup", [](Cpu& c) {
        GuestAddr out = c.x(8);
        c.mem.write<uint64_t>(out, 1024);
        c.mem.write<uint64_t>(out + 8, 1024);
        c.mem.write<uint64_t>(out + 16, 1024);
    });
    o.method(D, "recommendedMaxWorkingSetSize", [](Cpu& c) { c.ret(2ull << 30); });
    o.method(D, "maxBufferLength", [](Cpu& c) { c.ret(1ull << 30); });
    o.method(D, "maxThreadgroupMemoryLength", [](Cpu& c) { c.ret(32768); });
    o.method(D, "maxArgumentBufferSamplerCount", [](Cpu& c) { c.ret(16); });
    o.method(D, "currentAllocatedSize", [](Cpu& c) { c.ret(0); });
    o.method(D, "hasUnifiedMemory", [](Cpu& c) { c.ret(1); });
    o.method(D, "readWriteTextureSupport", [](Cpu& c) { c.ret(1); });
    o.method(D, "argumentBuffersSupport", [](Cpu& c) { c.ret(0); });
    o.method(D, "minimumLinearTextureAlignmentForPixelFormat:", [](Cpu& c) { c.ret(16); });
    o.method(D, "minimumTextureBufferAlignmentForPixelFormat:", [](Cpu& c) { c.ret(16); });
    o.method(D, "supportsTextureSampleCount:", [](Cpu& c) { c.ret(c.arg(2) == 1 || c.arg(2) == 2 || c.arg(2) == 4); });
    o.method(D, "sparseTileSizeInBytes", [](Cpu& c) { c.ret(16384); });
    o.method(D, "supports32BitFloatFiltering", [](Cpu& c) { c.ret(1); });
    o.method(D, "supportsQueryTextureLOD", [](Cpu& c) { c.ret(1); });
    for (const char* sel : {"isLowPower", "isHeadless", "isRemovable", "location", "supportsBCTextureCompression", "supportsRaytracing",
                            "supportsDynamicLibraries", "supportsFunctionPointers", "supportsShaderBarycentricCoordinates",
                            "areProgrammableSamplePositionsSupported", "areRasterOrderGroupsSupported", "supportsPrimitiveMotionBlur",
                            "supportsRenderDynamicLibraries", "supportsPullModelInterpolation", "areBarycentricCoordsSupported",
                            "supports32BitMSAA", "depth24Stencil8PixelFormatSupported"})
        o.method(D, sel, [](Cpu& c) { c.ret(0); });
    o.method(D, "supportsCounterSampling:", [](Cpu& c) { c.ret(0); });
    o.method(D, "supportsVertexAmplificationCount:", [](Cpu& c) { c.ret(c.arg(2) == 1); });
    o.method(D, "supportsRasterizationRateMapWithLayerCount:", [](Cpu& c) { c.ret(0); });
    o.method(D, "counterSets", [](Cpu& c) { c.ret(0); });

    o.method(D, "newCommandQueue", [](Cpu& c) { c.ret(make(c, "OrchardMTLCommandQueue")); });
    o.method(D, "newCommandQueueWithMaxCommandBufferCount:", [](Cpu& c) { c.ret(make(c, "OrchardMTLCommandQueue")); });

    auto new_buffer = [](Cpu& c, uint64_t length, GuestAddr init) {
        Id obj = make(c, "OrchardMTLBuffer");
        Buffer b;
        b.length = length;
        b.contents = c.rt.heap.alloc(std::max<uint64_t>(length, 16), 256);
        if (init)
            std::memcpy(c.mem.host(b.contents), c.mem.host(init), length);
        else
            std::memset(c.mem.host(b.contents), 0, length);
        std::lock_guard g(gpu);
        buffers[obj] = b;
        return obj;
    };
    static decltype(new_buffer) s_new_buffer = new_buffer;
    o.method(D, "newBufferWithLength:options:", [](Cpu& c) { c.ret(s_new_buffer(c, c.arg(2), 0)); });
    o.method(D, "newBufferWithBytes:length:options:", [](Cpu& c) { c.ret(s_new_buffer(c, c.arg(3), c.arg(2))); });
    o.method(D, "newBufferWithBytesNoCopy:length:options:deallocator:", [](Cpu& c) {
        Id obj = make(c, "OrchardMTLBuffer");
        GuestAddr dealloc = c.arg(5) ? block_copy(c, c.arg(5)) : 0;
        std::lock_guard g(gpu);
        buffers[obj] = {c.arg(2), c.arg(3), false, dealloc};
        c.ret(obj);
    });
    o.method(D, "newTextureWithDescriptor:", [](Cpu& c) {
        Id d = c.arg(2);
        Texture t;
        t.width = prop_x(c, d, "width");
        t.height = prop_x(c, d, "height");
        t.format = prop_x(c, d, "pixelFormat");
        t.type = prop_x(c, d, "textureType");
        t.mips = std::max<uint64_t>(prop_x(c, d, "mipmapLevelCount"), 1);
        t.array = std::max<uint64_t>(prop_x(c, d, "arrayLength"), 1);
        t.usage = prop_x(c, d, "usage");
        c.ret(new_texture(c, t));
    });
    o.method(D, "newFence", [](Cpu& c) { c.ret(make(c, "OrchardMTLFence")); });
    o.method(D, "newEvent", [](Cpu& c) { c.ret(make(c, "OrchardMTLEvent")); });
    o.method(D, "newSharedEvent", [](Cpu& c) { c.ret(make(c, "OrchardMTLEvent")); });
    o.method(D, "newHeapWithDescriptor:", [](Cpu& c) { c.ret(make(c, "OrchardMTLHeap")); });
    o.method(D, "heapBufferSizeAndAlignWithLength:options:", [](Cpu& c) {
        c.set_x(1, 256);
        c.ret((c.arg(2) + 255) & ~uint64_t(255));
    });
    o.method(D, "heapTextureSizeAndAlignWithDescriptor:", [](Cpu& c) {
        uint64_t size = prop_x(c, c.arg(2), "width") * prop_x(c, c.arg(2), "height") * 8;
        c.set_x(1, 16384);
        c.ret((size + 16383) & ~uint64_t(16383));
    });

    o.method(D, "newLibraryWithSource:options:error:", [](Cpu& c) {
        Id lib = make(c, "OrchardMTLLibrary");
        if (const char* dir = std::getenv("ORCHARD_DUMP_SHADERS"))
        {
            static std::atomic<int> n{0};
            std::string path = std::string(dir) + "/shader_" + std::to_string(n++) + ".metal";
            if (FILE* f = std::fopen(path.c_str(), "wb"))
            {
                std::string src = foundation::to_utf8(c, c.arg(2));
                std::fwrite(src.data(), 1, src.size(), f);
                std::fclose(f);
            }
        }
        objc(c).set_associated(c, lib, 1, c.arg(2), 1);
        if (c.arg(4)) c.mem.write<uint64_t>(c.arg(4), 0);
        c.ret(lib);
    });
    o.method(D, "newLibraryWithData:error:", [](Cpu& c) {
        if (c.arg(3)) c.mem.write<uint64_t>(c.arg(3), 0);
        c.ret(make(c, "OrchardMTLLibrary"));
    });
    o.method(D, "newDefaultLibrary", [](Cpu& c) { c.ret(make(c, "OrchardMTLLibrary")); });
    o.method(D, "newComputePipelineStateWithFunction:error:", [](Cpu& c) {
        if (c.arg(3)) c.mem.write<uint64_t>(c.arg(3), 0);
        c.ret(make(c, "OrchardMTLComputePipelineState"));
    });
    o.method(D, "newComputePipelineStateWithDescriptor:options:reflection:error:", [](Cpu& c) {
        if (c.arg(4)) c.mem.write<uint64_t>(c.arg(4), 0);
        if (c.arg(5)) c.mem.write<uint64_t>(c.arg(5), 0);
        c.ret(make(c, "OrchardMTLComputePipelineState"));
    });
}

void register_resources(objc::ObjcRuntime& o)
{
    const char* L = "OrchardMTLLibrary";
    o.method(L, "newFunctionWithName:", [](Cpu& c) {
        Id f = make(c, "OrchardMTLFunction");
        objc(c).set_associated(c, f, 1, objc(c).send(c, c.arg(2), "copy"), 1);
        objc(c).set_associated(c, f, 2, c.arg(0), 1);
        c.ret(f);
    });
    o.method(L, "newFunctionWithName:constantValues:error:", [](Cpu& c) {
        if (c.arg(4)) c.mem.write<uint64_t>(c.arg(4), 0);
        Id f = make(c, "OrchardMTLFunction");
        objc(c).set_associated(c, f, 1, objc(c).send(c, c.arg(2), "copy"), 1);
        objc(c).set_associated(c, f, 2, c.arg(0), 1);
        objc(c).set_associated(c, f, 3, c.arg(3), 1);
        c.ret(f);
    });
    o.method(L, "functionNames", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });
    const char* F = "OrchardMTLFunction";
    o.method(F, "name", [](Cpu& c) { c.ret(objc(c).get_associated(c.arg(0), 1)); });
    o.method(F, "functionType", [](Cpu& c) { c.ret(1); });
    o.method(F, "patchType", [](Cpu& c) { c.ret(0); });
    o.method(F, "patchControlPointCount", [](Cpu& c) { c.ret(uint64_t(-1)); });
    o.method(F, "options", [](Cpu& c) { c.ret(0); });
    o.method(F, "vertexAttributes", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });
    o.method(F, "stageInputAttributes", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });
    o.method(F, "functionConstantsDictionary", [](Cpu& c) { c.ret(foundation::make_dict(c, {})); });
    o.method("OrchardMTLComputePipelineState", "maxTotalThreadsPerThreadgroup", [](Cpu& c) { c.ret(1024); });
    o.method("OrchardMTLComputePipelineState", "threadExecutionWidth", [](Cpu& c) { c.ret(32); });
    o.method("OrchardMTLComputePipelineState", "staticThreadgroupMemoryLength", [](Cpu& c) { c.ret(0); });

    const char* B = "OrchardMTLBuffer";
    o.method(B, "contents", [](Cpu& c) {
        std::lock_guard g(gpu);
        c.ret(buffers[c.arg(0)].contents);
    });
    o.method(B, "length", [](Cpu& c) {
        std::lock_guard g(gpu);
        c.ret(buffers[c.arg(0)].length);
    });
    o.method(B, "allocatedSize", [](Cpu& c) {
        std::lock_guard g(gpu);
        c.ret(buffers[c.arg(0)].length);
    });
    o.method(B, "gpuAddress", [](Cpu& c) {
        std::lock_guard g(gpu);
        c.ret(buffers[c.arg(0)].contents);
    });
    o.method(B, "storageMode", [](Cpu& c) { c.ret(0); });
    o.method(B, "didModifyRange:", [](Cpu& c) {});
    o.method(B, "setPurgeableState:", [](Cpu& c) { c.ret(1); });
    o.method(B, "dealloc", [](Cpu& c) {
        Buffer b;
        {
            std::lock_guard g(gpu);
            auto it = buffers.find(c.arg(0));
            if (it != buffers.end())
            {
                b = it->second;
                buffers.erase(it);
            }
        }
        if (b.owned && b.contents) c.rt.heap.free(b.contents);
        if (b.deallocator)
        {
            call_block(c, b.deallocator, {b.contents, b.length});
            block_release(c, b.deallocator);
        }
        objc(c).dispose(c.arg(0));
    });

    const char* T = "OrchardMTLTexture";
    auto field = [](Cpu& c, uint64_t Texture::* f) {
        std::lock_guard g(gpu);
        Texture* t = texture_of(c.arg(0));
        c.ret(t ? t->*f : 0);
    };
    static decltype(field) s_field = field;
    o.method(T, "width", [](Cpu& c) { s_field(c, &Texture::width); });
    o.method(T, "height", [](Cpu& c) { s_field(c, &Texture::height); });
    o.method(T, "pixelFormat", [](Cpu& c) { s_field(c, &Texture::format); });
    o.method(T, "textureType", [](Cpu& c) { s_field(c, &Texture::type); });
    o.method(T, "mipmapLevelCount", [](Cpu& c) { s_field(c, &Texture::mips); });
    o.method(T, "arrayLength", [](Cpu& c) { s_field(c, &Texture::array); });
    o.method(T, "usage", [](Cpu& c) { s_field(c, &Texture::usage); });
    o.method(T, "depth", [](Cpu& c) { c.ret(1); });
    o.method(T, "sampleCount", [](Cpu& c) { c.ret(1); });
    o.method(T, "storageMode", [](Cpu& c) { c.ret(0); });
    o.method(T, "isFramebufferOnly", [](Cpu& c) { c.ret(0); });
    o.method(T, "parentTexture", [](Cpu& c) { c.ret(0); });
    o.method(T, "iosurface", [](Cpu& c) { c.ret(0); });
    o.method(T, "buffer", [](Cpu& c) { c.ret(0); });
    o.method(T, "allocatedSize", [](Cpu& c) {
        std::lock_guard g(gpu);
        Texture* t = texture_of(c.arg(0));
        c.ret(t ? t->width * t->height * 4 : 0);
    });
    o.method(T, "setPurgeableState:", [](Cpu& c) { c.ret(1); });
    o.method(T, "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(gpu);
            auto it = textures.find(c.arg(0));
            if (it != textures.end())
            {
                Texture& t = it->second;
                for (IUnknown* p : {static_cast<IUnknown*>(t.srv), static_cast<IUnknown*>(t.rtv), static_cast<IUnknown*>(t.dsv),
                                    static_cast<IUnknown*>(t.tex)})
                    if (p) p->Release();
                textures.erase(it);
            }
        }
        objc(c).dispose(c.arg(0));
    });
    for (const char* k : {"OrchardMTLTexture", "OrchardMTLBuffer"})
    {
        for (const char* sel :
             {"heap", "heapOffset", "isAliasable", "cpuCacheMode", "hazardTrackingMode", "resourceOptions", "compressionType", "isSparse",
              "firstMipmapInTail", "tailSizeInBytes", "isShareable", "remoteStorageTexture", "remoteStorageBuffer", "rootResource",
              "parentRelativeLevel", "parentRelativeSlice", "bufferOffset", "bufferBytesPerRow"})
            o.method(k, sel, [](Cpu& c) { c.ret(0); });
        o.method(k, "allowGPUOptimizedContents", [](Cpu& c) { c.ret(1); });
        o.method(k, "gpuResourceID", [](Cpu& c) { c.ret(c.arg(0)); });
        o.method(k, "makeAliasable", [](Cpu& c) {});
    }
    o.method(T, "swizzle", [](Cpu& c) { c.ret(0x05040302); });
    o.method(T, "newTextureViewWithPixelFormat:", [](Cpu& c) { c.ret(objc(c).retain(c.arg(0))); });
    o.method(T, "newTextureViewWithPixelFormat:textureType:levels:slices:", [](Cpu& c) { c.ret(objc(c).retain(c.arg(0))); });

    for (const char* k :
         {"OrchardMTLBuffer", "OrchardMTLTexture", "OrchardMTLLibrary", "OrchardMTLFunction", "OrchardMTLCommandQueue",
          "OrchardMTLCommandBuffer", "OrchardMTLRenderPipelineState", "OrchardMTLComputePipelineState", "OrchardMTLSamplerState",
          "OrchardMTLDepthStencilState", "OrchardMTLHeap", "OrchardMTLFence", "OrchardMTLEvent", "OrchardMTLRenderCommandEncoder",
          "OrchardMTLBlitCommandEncoder", "OrchardMTLComputeCommandEncoder", "OrchardMTLDevice"})
    {
        o.method(k, "label", [](Cpu& c) { c.ret(objc(c).get_associated(c.arg(0), 99)); });
        o.method(k, "setLabel:", [](Cpu& c) { objc(c).set_associated(c, c.arg(0), 99, c.arg(2), 3); });
        o.method(k, "device", [](Cpu& c) { c.ret(c.call(c.rt.hle.resolve("_MTLCreateSystemDefaultDevice", "Metal"))); });
    }
    o.method("OrchardMTLHeap", "newBufferWithLength:options:", [](Cpu& c) {
        Id dev = c.call(c.rt.hle.resolve("_MTLCreateSystemDefaultDevice", "Metal"));
        c.ret(objc(c).send(c, dev, "newBufferWithLength:options:", {c.arg(2), c.arg(3)}));
    });
    o.method("OrchardMTLHeap", "newTextureWithDescriptor:", [](Cpu& c) {
        Id dev = c.call(c.rt.hle.resolve("_MTLCreateSystemDefaultDevice", "Metal"));
        c.ret(objc(c).send(c, dev, "newTextureWithDescriptor:", {c.arg(2)}));
    });
    o.method("OrchardMTLHeap", "usedSize", [](Cpu& c) { c.ret(0); });
    o.method("OrchardMTLHeap", "maxAvailableSizeWithAlignment:", [](Cpu& c) { c.ret(256ull << 20); });
    o.method("OrchardMTLEvent", "signaledValue", [](Cpu& c) { c.ret(objc(c).get_associated(c.arg(0), 5)); });
    o.method("OrchardMTLEvent", "setSignaledValue:", [](Cpu& c) { objc(c).set_associated(c, c.arg(0), 5, c.arg(2), 0); });
}

void register_commands(objc::ObjcRuntime& o)
{
    auto new_command_buffer = [](Cpu& c) {
        Id cb = objc(c).autorelease(c, make(c, "OrchardMTLCommandBuffer"));
        std::lock_guard g(gpu);
        command_buffers[cb] = {};
        return cb;
    };
    static decltype(new_command_buffer) s_new_cb = new_command_buffer;
    const char* Q = "OrchardMTLCommandQueue";
    o.method(Q, "commandBuffer", [](Cpu& c) { c.ret(s_new_cb(c)); });
    o.method(Q, "commandBufferWithUnretainedReferences", [](Cpu& c) { c.ret(s_new_cb(c)); });
    o.method(Q, "commandBufferWithDescriptor:", [](Cpu& c) { c.ret(s_new_cb(c)); });
    o.method(Q, "insertDebugCaptureBoundary", [](Cpu& c) {});

    const char* CB = "OrchardMTLCommandBuffer";
    o.method(CB, "computeCommandEncoder", [](Cpu& c) { c.ret(autoreleased(c, "OrchardMTLComputeCommandEncoder")); });
    o.method(CB, "computeCommandEncoderWithDispatchType:", [](Cpu& c) { c.ret(autoreleased(c, "OrchardMTLComputeCommandEncoder")); });
    o.method(CB, "presentDrawable:", [](Cpu& c) {
        std::lock_guard g(gpu);
        command_buffers[c.arg(0)].present.push_back(objc(c).retain(c.arg(2)));
    });
    o.method(CB, "presentDrawable:afterMinimumDuration:", [](Cpu& c) {
        std::lock_guard g(gpu);
        command_buffers[c.arg(0)].present.push_back(objc(c).retain(c.arg(2)));
    });
    o.method(CB, "presentDrawable:atTime:", [](Cpu& c) {
        std::lock_guard g(gpu);
        command_buffers[c.arg(0)].present.push_back(objc(c).retain(c.arg(2)));
    });
    o.method(CB, "addCompletedHandler:", [](Cpu& c) {
        GuestAddr b = block_copy(c, c.arg(2));
        std::lock_guard g(gpu);
        command_buffers[c.arg(0)].completed.push_back(b);
    });
    o.method(CB, "addScheduledHandler:", [](Cpu& c) {
        GuestAddr b = block_copy(c, c.arg(2));
        std::lock_guard g(gpu);
        command_buffers[c.arg(0)].scheduled.push_back(b);
    });
    o.method(CB, "enqueue", [](Cpu& c) {});
    o.method(CB, "commit", [](Cpu& c) {
        CommandBuffer cb;
        {
            std::lock_guard g(gpu);
            cb = command_buffers[c.arg(0)];
            command_buffers[c.arg(0)].status = 4;
        }
        for (Id d : cb.present)
        {
            present(c, d);
            objc(c).release(c, d);
        }
        for (GuestAddr b : cb.scheduled)
            call_block(c, b, {c.arg(0)});
        for (GuestAddr b : cb.completed)
            call_block(c, b, {c.arg(0)});
    });
    o.method(CB, "waitUntilCompleted", [](Cpu& c) {});
    o.method(CB, "waitUntilScheduled", [](Cpu& c) {});
    o.method(CB, "status", [](Cpu& c) {
        std::lock_guard g(gpu);
        c.ret(command_buffers[c.arg(0)].status);
    });
    o.method(CB, "error", [](Cpu& c) { c.ret(0); });
    o.method(CB, "retainedReferences", [](Cpu& c) { c.ret(1); });
    for (const char* sel : {"GPUStartTime", "GPUEndTime", "kernelStartTime", "kernelEndTime"})
        o.method(CB, sel, [](Cpu& c) { c.set_d(0, 0); });
    for (const char* sel : {"encodeSignalEvent:value:", "encodeWaitForEvent:value:", "pushDebugGroup:", "popDebugGroup"})
        o.method(CB, sel, [](Cpu& c) {});
    o.method(CB, "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(gpu);
            command_buffers.erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });

    o.add_fallback([](Cpu& c, Class* cls, objc::SEL s) -> GuestAddr {
        if (cls->is_meta || cls->name.rfind("OrchardMTL", 0) != 0 || cls->name.find("Encoder") == std::string::npos) return 0;
        std::string sel = objc(c).sel_name(s);
        if (sel.starts_with("draw") || sel.starts_with("dispatch"))
        {
            static GuestAddr skip = c.rt.hle.make_stub("Metal draw (unsupported form)", [](Cpu&) { ++draws_skipped; });
            return skip;
        }
        static GuestAddr nop = c.rt.hle.make_stub("Metal encoder state (ignored)", [](Cpu&) {});
        return nop;
    });
}

void register_drawables(objc::ObjcRuntime& o)
{
    o.define("CAMetalDrawable", "NSObject");
    o.method("CAMetalLayer", "nextDrawable", [](Cpu& c) {
        Id layer = c.arg(0);
        auto& l = uikit::layers().get(layer);
        uint64_t w = uint64_t(l.drawable_w ? l.drawable_w : l.bounds.w * l.contents_scale);
        uint64_t h = uint64_t(l.drawable_h ? l.drawable_h : l.bounds.h * l.contents_scale);
        if (!w || !h) return c.ret(0);
        static std::mutex m;
        static std::unordered_map<Id, std::vector<Id>> rings;
        static std::unordered_map<Id, size_t> next;
        std::lock_guard g(m);
        auto& ring = rings[layer];
        if (!ring.empty())
        {
            std::lock_guard gg(gpu);
            Texture* t = texture_of(ring[0]);
            if (!t || t->width != w || t->height != h || t->format != l.pixel_format)
            {
                for (Id old : ring)
                    objc(c).release(c, old);
                ring.clear();
            }
        }
        if (ring.empty())
        {
            for (int i = 0; i < 3; ++i)
            {
                Texture t;
                t.width = w;
                t.height = h;
                t.format = l.pixel_format ? l.pixel_format : 80;
                t.usage = 4;
                ring.push_back(new_texture(c, t));
            }
            std::printf("[Metal] drawables %llux%llu for layer 0x%llx\n", (unsigned long long)w, (unsigned long long)h,
                        (unsigned long long)layer);
        }
        Id tex = ring[next[layer]++ % ring.size()];
        Id drawable = objc(c).autorelease(c, make(c, "CAMetalDrawable"));
        std::lock_guard gg(gpu);
        drawable_textures[drawable] = tex;
        objc(c).set_associated(c, drawable, 1, layer, 0);
        c.ret(drawable);
    });
    o.method("CAMetalDrawable", "texture", [](Cpu& c) {
        std::lock_guard g(gpu);
        c.ret(drawable_textures[c.arg(0)]);
    });
    o.method("CAMetalDrawable", "layer", [](Cpu& c) { c.ret(objc(c).get_associated(c.arg(0), 1)); });
    o.method("CAMetalDrawable", "present", [](Cpu& c) { present(c, c.arg(0)); });
    o.method("CAMetalDrawable", "drawableID", [](Cpu& c) { c.ret(c.arg(0)); });
    o.method("CAMetalDrawable", "presentedTime", [](Cpu& c) { c.set_d(0, 0); });
    o.method("CAMetalDrawable", "addPresentedHandler:", [](Cpu& c) {});
    o.method("CAMetalDrawable", "dealloc", [](Cpu& c) {
        {
            std::lock_guard g(gpu);
            drawable_textures.erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });
}

}

void register_metal(objc::ObjcRuntime& o)
{
    register_descriptors(o);
    for (const char* k : {"OrchardMTLDevice", "OrchardMTLCommandQueue", "OrchardMTLCommandBuffer", "OrchardMTLBuffer", "OrchardMTLTexture",
                          "OrchardMTLLibrary", "OrchardMTLFunction", "OrchardMTLRenderPipelineState", "OrchardMTLComputePipelineState",
                          "OrchardMTLSamplerState", "OrchardMTLDepthStencilState", "OrchardMTLHeap", "OrchardMTLFence", "OrchardMTLEvent",
                          "OrchardMTLRenderCommandEncoder", "OrchardMTLBlitCommandEncoder", "OrchardMTLComputeCommandEncoder"})
        o.define(k, "NSObject");
    register_device(o);
    register_resources(o);
    register_commands(o);
    register_drawables(o);
    register_render(o);
}

}
