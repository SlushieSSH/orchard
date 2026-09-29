#include "host/window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstring>
#include <fstream>

namespace orchard
{
namespace
{
template <typename T> void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

constexpr const char* kShader = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_main(VSOut i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1); }
)";

}

bool load_image(const std::filesystem::path& path, Image& out)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok =
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    if (ok)
    {
        UINT w, h;
        conv->GetSize(&w, &h);
        out.width = int(w);
        out.height = int(h);
        out.bgra.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data()));
    }
    release(conv);
    release(frame);
    release(decoder);
    release(factory);
    char head[16] = {};
    if (ok && std::ifstream(path, std::ios::binary).read(head, sizeof(head)) && std::memcmp(head + 12, "CgBI", 4) == 0)
    {
        for (size_t i = 0; i + 3 < out.bgra.size(); i += 4)
        {
            uint8_t* p = &out.bgra[i];
            std::swap(p[0], p[2]);
            if (p[3] && p[3] != 255)
                for (int k = 0; k < 3; ++k)
                    p[k] = uint8_t(std::min(255, p[k] * 255 / p[3]));
        }
    }
    return ok;
}

intptr_t __stdcall HostWindow::wndproc(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp)
{
    auto* self = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(HWND(hwnd), GWLP_USERDATA));
    switch (msg)
    {
    case WM_CLOSE:
        if (self) self->closed_ = true;
        return 0;
    case WM_SIZE:
        if (self && wp != SIZE_MINIMIZED)
        {
            self->client_w_ = LOWORD(lp);
            self->client_h_ = HIWORD(lp);
            self->resize_swapchain();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (self)
        {
            SetCapture(HWND(hwnd));
            self->mouse_down_ = true;
            self->on_mouse(PointerEvent::Kind::Down, short(LOWORD(lp)), short(HIWORD(lp)));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (self && self->mouse_down_) self->on_mouse(PointerEvent::Kind::Move, short(LOWORD(lp)), short(HIWORD(lp)));
        return 0;
    case WM_LBUTTONUP:
        if (self && self->mouse_down_)
        {
            ReleaseCapture();
            self->mouse_down_ = false;
            self->on_mouse(PointerEvent::Kind::Up, short(LOWORD(lp)), short(HIWORD(lp)));
        }
        return 0;
    }
    return DefWindowProcW(HWND(hwnd), msg, wp, lp);
}

bool HostWindow::create(const std::wstring& title, int points_w, int points_h)
{
    points_w_ = points_w;
    points_h_ = points_h;
    WNDCLASSW wc{};
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&HostWindow::wndproc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"OrchardWindow";
    RegisterClassW(&wc);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int h = int((work.bottom - work.top) * 0.9);
    int w = h * points_w / points_h;
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;
    hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(this));
    client_w_ = w;
    client_h_ = h;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    IDXGISwapChain* swap = nullptr;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &swap, &device_, &level, &context_)))
        return false;
    swapchain_ = swap;
    resize_swapchain();

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    D3DCompile(kShader, std::strlen(kShader), "blit", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &err);
    D3DCompile(kShader, std::strlen(kShader), "blit", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &err);
    if (!vsb || !psb) return false;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    vs_ = vs;
    ps_ = ps;
    release(vsb);
    release(psb);
    release(err);
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* sampler;
    device_->CreateSamplerState(&smp, &sampler);
    sampler_ = sampler;

    ShowWindow(hwnd, SW_SHOW);
    return true;
}

HostWindow::~HostWindow()
{
    auto rtv = static_cast<ID3D11RenderTargetView*>(rtv_);
    auto swap = static_cast<IDXGISwapChain*>(swapchain_);
    auto vs = static_cast<ID3D11VertexShader*>(vs_);
    auto ps = static_cast<ID3D11PixelShader*>(ps_);
    auto sampler = static_cast<ID3D11SamplerState*>(sampler_);
    release(rtv);
    release(swap);
    release(vs);
    release(ps);
    release(sampler);
    release(context_);
    release(device_);
    if (hwnd_) DestroyWindow(HWND(hwnd_));
}

void HostWindow::resize_swapchain()
{
    auto swap = static_cast<IDXGISwapChain*>(swapchain_);
    if (!swap || client_w_ <= 0 || client_h_ <= 0) return;
    auto rtv = static_cast<ID3D11RenderTargetView*>(rtv_);
    if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    release(rtv);
    swap->ResizeBuffers(0, UINT(client_w_), UINT(client_h_), DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back = nullptr;
    swap->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back, nullptr, &rtv);
    release(back);
    rtv_ = rtv;
}

bool HostWindow::pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

void HostWindow::on_mouse(PointerEvent::Kind kind, int x, int y)
{
    float scale = std::min(float(client_w_) / points_w_, float(client_h_) / points_h_);
    float ox = (client_w_ - points_w_ * scale) / 2, oy = (client_h_ - points_h_ * scale) / 2;
    PointerEvent e{kind, (x - ox) / scale, (y - oy) / scale};
    std::lock_guard g(events_lock_);
    events_.push_back(e);
}

std::vector<PointerEvent> HostWindow::take_events()
{
    std::lock_guard g(events_lock_);
    std::vector<PointerEvent> out(events_.begin(), events_.end());
    events_.clear();
    return out;
}

void HostWindow::draw_srv(void* srv_ptr, int src_w, int src_h)
{
    auto rtv = static_cast<ID3D11RenderTargetView*>(rtv_);
    auto srv = static_cast<ID3D11ShaderResourceView*>(srv_ptr);
    const float black[4] = {0, 0, 0, 1};
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, black);
    context_->RSSetState(nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
    float scale = std::min(float(client_w_) / src_w, float(client_h_) / src_h);
    D3D11_VIEWPORT vp{(client_w_ - src_w * scale) / 2, (client_h_ - src_h * scale) / 2, src_w * scale, src_h * scale, 0, 1};
    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(static_cast<ID3D11VertexShader*>(vs_), nullptr, 0);
    context_->PSSetShader(static_cast<ID3D11PixelShader*>(ps_), nullptr, 0);
    context_->PSSetShaderResources(0, 1, &srv);
    auto sampler = static_cast<ID3D11SamplerState*>(sampler_);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->Draw(3, 0);
    static_cast<IDXGISwapChain*>(swapchain_)->Present(1, 0);
}

namespace
{
bool encode_png(const std::filesystem::path& path, UINT w, UINT h, UINT pitch, const uint8_t* bgra)
{
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
              SUCCEEDED(frame->WritePixels(h, pitch, pitch * h, const_cast<BYTE*>(bgra))) && SUCCEEDED(frame->Commit()) &&
              SUCCEEDED(enc->Commit());
    release(frame);
    release(enc);
    release(stream);
    release(factory);
    return ok;
}

float half_to_float(uint16_t h)
{
    int e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * std::ldexp(1.0f, e - 15);
    return (h & 0x8000) ? -v : v;
}

float small_float(uint32_t bits, int mant)
{
    int e = int(bits >> mant), m = int(bits & ((1u << mant) - 1));
    return e == 0 ? m / float(1 << mant) * std::ldexp(1.0f, -14) : (1.0f + m / float(1 << mant)) * std::ldexp(1.0f, e - 15);
}

uint8_t to8(float v)
{
    return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool convert_row(DXGI_FORMAT fmt, const uint8_t* src, uint8_t* dst, UINT w)
{
    for (UINT x = 0; x < w; ++x)
    {
        uint8_t* o = dst + x * 4;
        switch (fmt)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: std::memcpy(o, src + x * 4, 4); break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            o[0] = src[x * 4 + 2];
            o[1] = src[x * 4 + 1];
            o[2] = src[x * 4 + 0];
            o[3] = src[x * 4 + 3];
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            auto* p = reinterpret_cast<const uint16_t*>(src + x * 8);
            o[0] = to8(half_to_float(p[2]));
            o[1] = to8(half_to_float(p[1]));
            o[2] = to8(half_to_float(p[0]));
            o[3] = to8(half_to_float(p[3]));
            break;
        }
        case DXGI_FORMAT_R11G11B10_FLOAT: {
            uint32_t v;
            std::memcpy(&v, src + x * 4, 4);
            o[2] = to8(small_float(v & 0x7ff, 6));
            o[1] = to8(small_float((v >> 11) & 0x7ff, 6));
            o[0] = to8(small_float(v >> 22, 5));
            o[3] = 255;
            break;
        }
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            uint32_t v;
            std::memcpy(&v, src + x * 4, 4);
            o[2] = uint8_t((v & 0x3ff) >> 2);
            o[1] = uint8_t(((v >> 10) & 0x3ff) >> 2);
            o[0] = uint8_t(((v >> 20) & 0x3ff) >> 2);
            o[3] = uint8_t(((v >> 30) & 3) * 85);
            break;
        }
        case DXGI_FORMAT_R8_UNORM: o[0] = o[1] = o[2] = src[x], o[3] = 255; break;
        case DXGI_FORMAT_A8_UNORM: o[0] = o[1] = o[2] = src[x], o[3] = 255; break;
        default: return false;
        }
    }
    return true;
}

}

bool HostWindow::save_texture_png(ID3D11Texture2D* tex, const std::filesystem::path& path)
{
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    UINT w = td.Width, h = td.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) return false;
    context_->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};
    bool ok = SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &map));
    if (ok)
    {
        std::vector<uint8_t> bgra(size_t(w) * h * 4);
        for (UINT y = 0; y < h && ok; ++y)
            ok = convert_row(td.Format, static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, bgra.data() + size_t(y) * w * 4,
                             w);
        context_->Unmap(staging, 0);
        ok = ok && encode_png(path, w, h, w * 4, bgra.data());
    }
    release(staging);
    return ok;
}

bool HostWindow::save_png(const std::filesystem::path& path)
{
    auto swap = static_cast<IDXGISwapChain*>(swapchain_);
    ID3D11Texture2D* back = nullptr;
    if (!swap || FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    bool ok = save_texture_png(back, path);
    release(back);
    return ok;
}

void HostWindow::show_image(const Image& img)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(img.width);
    td.Height = UINT(img.height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{img.bgra.data(), UINT(img.width * 4), 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device_->CreateTexture2D(&td, &init, &tex))) return;
    show_texture(tex);
    release(tex);
}

void HostWindow::show_texture(ID3D11Texture2D* tex)
{
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device_->CreateShaderResourceView(tex, nullptr, &srv))) return;
    draw_srv(srv, int(td.Width), int(td.Height));
    release(srv);
}

}
