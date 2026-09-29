#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace orchard
{
struct Image
{
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

bool load_image(const std::filesystem::path& path, Image& out);

struct PointerEvent
{
    enum class Kind
    {
        Down,
        Move,
        Up
    } kind;
    float x, y;
};

class HostWindow
{
public:
    bool create(const std::wstring& title, int points_w, int points_h);
    ~HostWindow();

    bool pump();

    void show_image(const Image& img);
    void show_texture(ID3D11Texture2D* tex);

    bool save_png(const std::filesystem::path& path);
    bool save_texture_png(ID3D11Texture2D* tex, const std::filesystem::path& path);

    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* context() const { return context_; }

    std::vector<PointerEvent> take_events();

private:
    void draw_srv(void* srv, int src_w, int src_h);
    void resize_swapchain();
    static intptr_t __stdcall wndproc(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
    void on_mouse(PointerEvent::Kind kind, int x, int y);

    void* hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    void* swapchain_ = nullptr;
    void* rtv_ = nullptr;
    void* vs_ = nullptr;
    void* ps_ = nullptr;
    void* sampler_ = nullptr;
    int client_w_ = 0, client_h_ = 0;
    int points_w_ = 0, points_h_ = 0;
    bool closed_ = false;
    bool mouse_down_ = false;
    std::mutex events_lock_;
    std::deque<PointerEvent> events_;
};

}
