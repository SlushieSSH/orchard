#include "frameworks/Audio/mixer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace orchard::audio
{
namespace
{
constexpr uint32_t kRate = 48000, kChannels = 2;

template <typename T> void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}
}

Mixer& Mixer::get()
{
    static Mixer m;
    return m;
}

std::shared_ptr<Voice> Mixer::add(std::shared_ptr<Voice> v)
{
    std::lock_guard g(lock_);
    if (std::find(voices_.begin(), voices_.end(), v) == voices_.end()) voices_.push_back(v);
    if (!started_)
    {
        started_ = true;
        start();
    }
    return v;
}

void Mixer::remove(const std::shared_ptr<Voice>& v)
{
    std::lock_guard g(lock_);
    std::erase(voices_, v);
}

void Mixer::mix(float* out, uint32_t frames, std::vector<std::function<void()>>& events)
{
    std::fill(out, out + size_t(frames) * kChannels, 0.0f);
    std::lock_guard g(lock_);
    for (auto it = voices_.begin(); it != voices_.end();)
    {
        Voice& v = **it;
        if (!v.playing)
        {
            ++it;
            continue;
        }
        float left = v.volume * std::min(1.0f, 1.0f - v.pan), right = v.volume * std::min(1.0f, 1.0f + v.pan);
        double step = v.rate / kRate;
        bool finished = false;
        for (uint32_t i = 0; i < frames && !finished; ++i)
        {
            const float* src = nullptr;
            size_t available = 0;
            if (v.streaming)
            {
                while (!v.queue.empty() && size_t(v.position) >= v.queue.front().samples.size() / v.channels)
                {
                    v.position -= double(v.queue.front().samples.size() / v.channels);
                    uint64_t tag = v.queue.front().tag;
                    v.queue.pop_front();
                    if (v.on_chunk_done) events.push_back([cb = v.on_chunk_done, tag] { cb(tag); });
                }
                if (v.queue.empty()) break;
                src = v.queue.front().samples.data();
                available = v.queue.front().samples.size() / v.channels;
            }
            else
            {
                available = v.pcm ? v.pcm->frames() : 0;
                if (size_t(v.position) >= available)
                {
                    if (v.loops != 0 && available)
                    {
                        if (v.loops > 0) --v.loops;
                        v.position = 0;
                    }
                    else
                    {
                        finished = true;
                        break;
                    }
                }
                src = v.pcm->samples.data();
            }
            size_t a = size_t(v.position);
            size_t b = std::min(a + 1, available - 1);
            float t = float(v.position - double(a));
            auto at = [&](size_t frame, uint32_t ch) { return src[frame * v.channels + std::min(ch, v.channels - 1)]; };
            float l = at(a, 0) + (at(b, 0) - at(a, 0)) * t;
            float r = at(a, 1) + (at(b, 1) - at(a, 1)) * t;
            out[i * kChannels] += l * left;
            out[i * kChannels + 1] += r * right;
            v.position += step;
        }
        if (finished)
        {
            v.playing = false;
            v.position = 0;
            if (v.on_finish) events.push_back(v.on_finish);
        }
        ++it;
    }
    for (uint32_t i = 0; i < frames * kChannels; ++i)
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void Mixer::start()
{
    std::thread([this] {
        SetThreadDescription(GetCurrentThread(), L"audio-mixer");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        UINT32 buffer_frames = 0;
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = kChannels;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = kChannels * 4;
        wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
                  SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                  SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
                  SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 600000, 0, &wf,
                                               nullptr)) &&
                  SUCCEEDED(client->GetBufferSize(&buffer_frames)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))) &&
                  SUCCEEDED(client->Start());
        if (!ok) std::printf("[audio] mixer has no output device, running silent\n");
        FILE* dump = nullptr;
        if (const char* path = std::getenv("ORCHARD_MIXER_DUMP")) dump = std::fopen(path, "wb");
        constexpr uint32_t kChunk = 480;
        std::vector<float> buf(kChunk * kChannels);
        std::vector<std::function<void()>> events;
        auto next = std::chrono::steady_clock::now();
        for (;;)
        {
            UINT32 padding = 0;
            if (ok && (FAILED(client->GetCurrentPadding(&padding)) || buffer_frames - std::min(padding, buffer_frames) < kChunk))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            events.clear();
            mix(buf.data(), kChunk, events);
            for (auto& e : events)
                e();
            if (dump)
            {
                std::fwrite(buf.data(), sizeof(float), buf.size(), dump);
                std::fflush(dump);
            }
            if (ok)
            {
                BYTE* data = nullptr;
                if (SUCCEEDED(render->GetBuffer(kChunk, &data)))
                {
                    std::memcpy(data, buf.data(), buf.size() * sizeof(float));
                    render->ReleaseBuffer(kChunk, 0);
                }
            }
            else
            {
                next += std::chrono::microseconds(kChunk * 1000000 / kRate);
                std::this_thread::sleep_until(next);
            }
        }
    }).detach();
}

}
