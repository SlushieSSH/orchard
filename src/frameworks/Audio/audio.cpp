#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard::audio
{
using objc::objc;
using Id = GuestAddr;

namespace
{
constexpr double kSampleRate = 48000;
constexpr uint32_t kFramesPerPull = 480;

constexpr uint32_t kStreamFormat = 8, kSetRenderCallback = 23, kMaximumFramesPerSlice = 14;
constexpr uint32_t kFormatIsFloat = 1, kFormatIsNonInterleaved = 32;
constexpr uint32_t kRenderOutputIsSilence = 16;

struct Format
{
    double rate = kSampleRate;
    uint32_t flags = kFormatIsFloat | 8;
    uint32_t channels = 2;
    uint32_t bits = 32;
    bool is_float() const { return flags & kFormatIsFloat; }
    bool interleaved() const { return !(flags & kFormatIsNonInterleaved); }
    uint32_t sample_bytes() const { return bits / 8; }
    uint32_t frame_bytes() const { return interleaved() ? sample_bytes() * channels : sample_bytes(); }
};

struct Unit
{
    GuestAddr render_proc = 0, render_ref = 0;
    Format format;
    bool running = false;
    bool pulling = false;
};

std::mutex lock;
std::unordered_map<GuestAddr, Unit> units;

void write_format(Cpu& c, GuestAddr asbd, const Format& f)
{
    c.mem.write<double>(asbd, f.rate);
    c.mem.write<uint32_t>(asbd + 8, 0x6c70636d);
    c.mem.write<uint32_t>(asbd + 12, f.flags);
    c.mem.write<uint32_t>(asbd + 16, f.frame_bytes());
    c.mem.write<uint32_t>(asbd + 20, 1);
    c.mem.write<uint32_t>(asbd + 24, f.frame_bytes());
    c.mem.write<uint32_t>(asbd + 28, f.channels);
    c.mem.write<uint32_t>(asbd + 32, f.bits);
    c.mem.write<uint32_t>(asbd + 36, 0);
}

Format read_format(Cpu& c, GuestAddr asbd)
{
    Format f;
    f.rate = c.mem.read<double>(asbd);
    f.flags = c.mem.read<uint32_t>(asbd + 12);
    f.channels = std::max<uint32_t>(c.mem.read<uint32_t>(asbd + 28), 1);
    f.bits = c.mem.read<uint32_t>(asbd + 32);
    if (f.rate <= 0) f.rate = kSampleRate;
    if (f.bits != 16 && f.bits != 32) f.bits = 32;
    return f;
}

float sample_at(const uint8_t* p, const Format& f)
{
    if (f.is_float()) return *reinterpret_cast<const float*>(p);
    if (f.bits == 16) return *reinterpret_cast<const int16_t*>(p) / 32768.0f;
    return float(*reinterpret_cast<const int32_t*>(p) / 2147483648.0);
}

template <typename T> void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

struct Output
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 buffer_frames = 0;
    uint32_t channels = 0;

    bool open(const Format& f)
    {
        channels = std::min<uint32_t>(f.channels, 2);
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = WORD(channels);
        wf.nSamplesPerSec = DWORD(f.rate);
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = WORD(channels * 4);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
               SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
               SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
               SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 800000, 0, &wf,
                                            nullptr)) &&
               SUCCEEDED(client->GetBufferSize(&buffer_frames)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))) &&
               SUCCEEDED(client->Start());
    }

    UINT32 free_frames()
    {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) return 0;
        return buffer_frames - std::min(padding, buffer_frames);
    }

    void write(const std::vector<float>& interleaved, uint32_t frames)
    {
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data))) return;
        std::memcpy(data, interleaved.data(), size_t(frames) * channels * sizeof(float));
        render->ReleaseBuffer(frames, 0);
    }

    ~Output()
    {
        if (client) client->Stop();
        release(render);
        release(client);
        release(device);
        release(enumerator);
    }
};

void start_pulling(Cpu& c, GuestAddr unit)
{
    c.rt.threads->spawn_host("audio-io", [unit](Cpu& cpu) {
        Format f;
        {
            std::lock_guard g(lock);
            f = units[unit].format;
        }
        std::printf("[audio] output %.0f Hz, %u channels, %u-bit %s, %s\n", f.rate, f.channels, f.bits, f.is_float() ? "float" : "int",
                    f.interleaved() ? "interleaved" : "non-interleaved");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Output out;
        bool device = out.open(f);
        if (!device) std::printf("[audio] no output device, running silent\n");

        uint32_t buffers = f.interleaved() ? 1 : f.channels;
        uint32_t buffer_bytes = kFramesPerPull * f.frame_bytes();
        GuestAddr flags = cpu.rt.heap.calloc(8), stamp = cpu.rt.heap.calloc(64), list = cpu.rt.heap.calloc(8 + 16 * buffers);
        std::vector<GuestAddr> data(buffers);
        for (auto& d : data)
            d = cpu.rt.heap.calloc(buffer_bytes);
        std::vector<float> mixed(size_t(kFramesPerPull) * std::max<uint32_t>(out.channels, 1));
        double sample_time = 0;
        auto next = std::chrono::steady_clock::now();
        auto stats_since = std::chrono::steady_clock::now();
        double stats_samples = 0;
        std::chrono::steady_clock::duration callback_time{};
        uint64_t underruns = 0;
        FILE* dump = nullptr;
        if (const char* path = std::getenv("ORCHARD_AUDIO_DUMP")) dump = std::fopen(path, "wb");

        while (!cpu.rt.halted() && !cpu.stopped())
        {
            Unit u;
            {
                std::lock_guard g(lock);
                u = units[unit];
            }
            if (!u.running) break;
            if (device && out.free_frames() < kFramesPerPull)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            cpu.mem.write<uint32_t>(flags, 0);
            cpu.mem.write<double>(stamp, sample_time);
            cpu.mem.write<uint32_t>(stamp + 56, 1);
            cpu.mem.write<uint32_t>(list, buffers);
            for (uint32_t b = 0; b < buffers; ++b)
            {
                cpu.mem.write<uint32_t>(list + 8 + 16 * b, f.interleaved() ? f.channels : 1);
                cpu.mem.write<uint32_t>(list + 12 + 16 * b, buffer_bytes);
                cpu.mem.write<uint64_t>(list + 16 + 16 * b, data[b]);
            }
            if (device && out.free_frames() >= out.buffer_frames) ++underruns;
            auto call_start = std::chrono::steady_clock::now();
            if (u.render_proc) cpu.call(u.render_proc, {u.render_ref, flags, stamp, 0, kFramesPerPull, list});
            callback_time += std::chrono::steady_clock::now() - call_start;
            sample_time += kFramesPerPull;
            if (std::getenv("ORCHARD_AUDIO_STATS") && std::chrono::steady_clock::now() - stats_since > std::chrono::seconds(10))
            {
                double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - stats_since).count();
                std::printf("[audio] last %.0fs: produced %.1fs of audio, callbacks took %.1fs, %llu underruns\n", wall,
                            (sample_time - stats_samples) / f.rate, std::chrono::duration<double>(callback_time).count(),
                            (unsigned long long)underruns);
                stats_since = std::chrono::steady_clock::now();
                stats_samples = sample_time;
                callback_time = {};
                underruns = 0;
            }

            if (device)
            {
                bool silent = !u.render_proc || (cpu.mem.read<uint32_t>(flags) & kRenderOutputIsSilence);
                for (uint32_t i = 0; i < kFramesPerPull; ++i)
                    for (uint32_t ch = 0; ch < out.channels; ++ch)
                    {
                        uint32_t src = std::min(ch, f.channels - 1);
                        const uint8_t* p = f.interleaved()
                                               ? cpu.mem.host(data[0]) + size_t(i) * f.frame_bytes() + size_t(src) * f.sample_bytes()
                                               : cpu.mem.host(data[src]) + size_t(i) * f.sample_bytes();
                        mixed[size_t(i) * out.channels + ch] = silent ? 0.0f : sample_at(p, f);
                    }
                out.write(mixed, kFramesPerPull);
                if (dump) std::fwrite(mixed.data(), sizeof(float), mixed.size(), dump);
            }
            else
            {
                next += std::chrono::microseconds(int64_t(kFramesPerPull * 1e6 / f.rate));
                std::this_thread::sleep_until(next);
            }
        }
        if (dump) std::fclose(dump);
        for (GuestAddr p : {flags, stamp, list})
            cpu.rt.heap.free(p);
        for (GuestAddr d : data)
            cpu.rt.heap.free(d);
        std::lock_guard g(lock);
        units[unit].pulling = false;
    });
}

void register_audio_units(Hle& h)
{
    h.fn("_AudioComponentFindNext", [](Cpu& c) {
        static GuestAddr component = c.mem.alloc_system(16, 16);
        c.ret(component);
    });
    h.fn("_AudioComponentInstanceNew", [](Cpu& c) {
        GuestAddr unit = c.rt.heap.calloc(16);
        {
            std::lock_guard g(lock);
            units[unit];
        }
        c.mem.write<uint64_t>(c.arg(1), unit);
        c.ret(0);
    });
    h.fn("_AudioComponentInstanceDispose", [](Cpu& c) {
        std::lock_guard g(lock);
        auto it = units.find(c.arg(0));
        if (it != units.end()) it->second.running = false;
        c.ret(0);
    });
    h.fn("_AudioUnitSetProperty", [](Cpu& c) {
        uint32_t id = uint32_t(c.arg(1));
        if (id == kSetRenderCallback && c.arg(4))
        {
            std::lock_guard g(lock);
            units[c.arg(0)].render_proc = c.mem.read<uint64_t>(c.arg(4));
            units[c.arg(0)].render_ref = c.mem.read<uint64_t>(c.arg(4) + 8);
        }
        else if (id == kStreamFormat && c.arg(4))
        {
            Format f = read_format(c, c.arg(4));
            std::lock_guard g(lock);
            units[c.arg(0)].format = f;
        }
        c.ret(0);
    });
    h.fn("_AudioUnitGetProperty", [](Cpu& c) {
        uint32_t id = uint32_t(c.arg(1));
        if (id == kStreamFormat && c.arg(4))
        {
            Format f;
            {
                std::lock_guard g(lock);
                auto it = units.find(c.arg(0));
                if (it != units.end()) f = it->second.format;
            }
            write_format(c, c.arg(4), f);
        }
        else if (id == kMaximumFramesPerSlice && c.arg(4))
        {
            c.mem.write<uint32_t>(c.arg(4), 4096);
        }
        else if (c.arg(4) && c.arg(5))
        {
            std::memset(c.mem.host(c.arg(4)), 0, c.mem.read<uint32_t>(c.arg(5)));
        }
        c.ret(0);
    });
    h.fn("_AudioUnitGetPropertyInfo", [](Cpu& c) {
        if (c.arg(4)) c.mem.write<uint32_t>(c.arg(4), 40);
        if (c.arg(5)) c.mem.write<uint8_t>(c.arg(5), 1);
        c.ret(0);
    });
    h.fn("_AudioUnitInitialize", [](Cpu& c) { c.ret(0); });
    h.fn("_AudioUnitUninitialize", [](Cpu& c) { c.ret(0); });
    h.fn("_AudioUnitRender", [](Cpu& c) { c.ret(0); });
    h.fn("_AudioUnitAddPropertyListener", [](Cpu& c) { c.ret(0); });
    h.fn("_AudioOutputUnitStart", [](Cpu& c) {
        bool start = false;
        {
            std::lock_guard g(lock);
            Unit& u = units[c.arg(0)];
            u.running = true;
            if (!u.pulling) start = u.pulling = true;
        }
        if (start) start_pulling(c, c.arg(0));
        c.ret(0);
    });
    h.fn("_AudioOutputUnitStop", [](Cpu& c) {
        std::lock_guard g(lock);
        auto it = units.find(c.arg(0));
        if (it != units.end()) it->second.running = false;
        c.ret(0);
    });
}

void register_queues_and_files(Hle& h)
{
    constexpr uint64_t kUnsupported = 0x7479703f;
    for (const char* n :
         {"_AudioFileOpenWithCallbacks", "_AudioFileOpenURL", "_AudioQueueOfflineRender", "_AudioQueueSetOfflineRenderFormat",
          "_AudioFileClose", "_AudioFileGetProperty", "_AudioFileGetPropertyInfo", "_AudioFileReadPacketData"})
        h.fn(n, [](Cpu& c) { c.ret(kUnsupported); });
    auto cmtime = [](Cpu& c, int64_t value, int32_t timescale) {
        GuestAddr out = c.x(8);
        c.mem.write<int64_t>(out, value);
        c.mem.write<int32_t>(out + 8, timescale);
        c.mem.write<uint32_t>(out + 12, timescale > 0 ? 1 : 0);
        c.mem.write<int64_t>(out + 16, 0);
    };
    static decltype(cmtime) s_cmtime = cmtime;
    h.fn("_CMTimeMake", [](Cpu& c) { s_cmtime(c, int64_t(c.arg(0)), int32_t(c.arg(1))); });
    h.fn("_CMTimeMakeWithSeconds", [](Cpu& c) {
        int32_t scale = int32_t(c.arg(0)) > 0 ? int32_t(c.arg(0)) : 600;
        s_cmtime(c, int64_t(c.d(0) * scale), scale);
    });
    h.fn("_CMTimeGetSeconds", [](Cpu& c) {
        int64_t value = c.mem.read<int64_t>(c.arg(0));
        int32_t scale = c.mem.read<int32_t>(c.arg(0) + 8);
        c.set_d(0, scale > 0 ? double(value) / scale : 0.0);
    });
}

void register_session(objc::ObjcRuntime& o)
{
    o.define("AVAudioSession", "NSObject");
    o.define("AVAudioSessionRouteDescription", "NSObject");
    o.class_method("AVAudioSession", "sharedInstance", [](Cpu& c) {
        static Id s = objc(c).alloc_instance(objc(c).host_class("AVAudioSession"));
        c.ret(s);
    });
    for (const char* sel : {"setCategory:error:", "setCategory:withOptions:error:", "setCategory:mode:options:error:", "setMode:error:",
                            "setActive:error:", "setActive:withOptions:error:", "setPreferredSampleRate:error:",
                            "setPreferredIOBufferDuration:error:", "setPreferredOutputNumberOfChannels:error:",
                            "overrideOutputAudioPort:error:", "setAllowHapticsAndSystemSoundsDuringRecording:error:"})
        o.method("AVAudioSession", sel, [](Cpu& c) { c.ret(1); });
    o.method("AVAudioSession", "category", [](Cpu& c) { c.ret(foundation::string_autoreleased(c, "AVAudioSessionCategorySoloAmbient")); });
    o.method("AVAudioSession", "mode", [](Cpu& c) { c.ret(foundation::string_autoreleased(c, "AVAudioSessionModeDefault")); });
    o.method("AVAudioSession", "categoryOptions", [](Cpu& c) { c.ret(0); });
    o.method("AVAudioSession", "sampleRate", [](Cpu& c) { c.set_d(0, kSampleRate); });
    o.method("AVAudioSession", "preferredSampleRate", [](Cpu& c) { c.set_d(0, kSampleRate); });
    o.method("AVAudioSession", "IOBufferDuration", [](Cpu& c) { c.set_d(0, kFramesPerPull / kSampleRate); });
    o.method("AVAudioSession", "outputLatency", [](Cpu& c) { c.set_d(0, 0.01); });
    o.method("AVAudioSession", "inputLatency", [](Cpu& c) { c.set_d(0, 0.01); });
    o.method("AVAudioSession", "outputVolume", [](Cpu& c) { c.set_s(0, 1.0f); });
    o.method("AVAudioSession", "outputNumberOfChannels", [](Cpu& c) { c.ret(2); });
    o.method("AVAudioSession", "maximumOutputNumberOfChannels", [](Cpu& c) { c.ret(2); });
    o.method("AVAudioSession", "inputNumberOfChannels", [](Cpu& c) { c.ret(0); });
    o.method("AVAudioSession", "isOtherAudioPlaying", [](Cpu& c) { c.ret(0); });
    o.method("AVAudioSession", "secondaryAudioShouldBeSilencedHint", [](Cpu& c) { c.ret(0); });
    o.method("AVAudioSession", "isInputAvailable", [](Cpu& c) { c.ret(0); });
    o.method("AVAudioSession", "recordPermission", [](Cpu& c) { c.ret(0x64656e79); });
    o.method("AVAudioSession", "currentRoute", [](Cpu& c) {
        static Id r = objc(c).alloc_instance(objc(c).host_class("AVAudioSessionRouteDescription"));
        c.ret(r);
    });
    o.method("AVAudioSessionRouteDescription", "outputs", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });
    o.method("AVAudioSessionRouteDescription", "inputs", [](Cpu& c) { c.ret(foundation::make_array(c, {})); });
}

}

void register_players(objc::ObjcRuntime& o);

void register_audio(objc::ObjcRuntime& o)
{
    register_audio_units(o.rt.hle);
    register_queues_and_files(o.rt.hle);
    register_session(o);
    register_players(o);
}

}
