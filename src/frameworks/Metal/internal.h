#pragma once

#include <d3d11.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "frameworks/Metal/descriptors.h"

namespace orchard::objc
{
class ObjcRuntime;
}

namespace orchard::metal::detail
{
extern std::recursive_mutex gpu;

struct Texture
{
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint64_t width = 1, height = 1, format = 0, type = 2, mips = 1, array = 1, usage = 1;
    int block_w = 1, block_h = 1;
    bool framebuffer_only = false;
};

struct Buffer
{
    GuestAddr contents = 0;
    uint64_t length = 0;
    bool owned = true;
    GuestAddr deallocator = 0;
};

extern std::unordered_map<Id, Texture> textures;
extern std::unordered_map<Id, Buffer> buffers;
extern std::atomic<uint64_t> draws_done, draws_skipped, frames_presented;

ID3D11Device* d3d();
ID3D11DeviceContext* ctx();
Texture* texture_of(Id obj);
Buffer* buffer_of(Id obj);

void register_render(objc::ObjcRuntime& o);

}
