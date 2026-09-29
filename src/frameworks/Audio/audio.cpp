#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

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

struct Unit
{
    GuestAddr render_proc = 0, render_ref = 0;
    bool running = false;
    bool pulling = false;
};

std::mutex lock;
std::unordered_map<GuestAddr, Unit> units;

void write_format(Cpu& c, GuestAddr asbd)
{
    c.mem.write<double>(asbd, kSampleRate);
    c.mem.write<uint32_t>(asbd + 8, 0x6c70636d);
    c.mem.write<uint32_t>(asbd + 12, 1 | 8);
    c.mem.write<uint32_t>(asbd + 16, 8);
    c.mem.write<uint32_t>(asbd + 20, 1);
    c.mem.write<uint32_t>(asbd + 24, 8);
    c.mem.write<uint32_t>(asbd + 28, 2);
    c.mem.write<uint32_t>(asbd + 32, 32);
    c.mem.write<uint32_t>(asbd + 36, 0);
}

void start_pulling(Cpu& c, GuestAddr unit)
{
    c.rt.threads->spawn_host("audio-io", [unit](Cpu& cpu) {
        GuestAddr flags = cpu.rt.heap.calloc(8), stamp = cpu.rt.heap.calloc(64), list = cpu.rt.heap.calloc(32);
        GuestAddr samples = cpu.rt.heap.calloc(kFramesPerPull * 8);
        double sample_time = 0;
        auto next = std::chrono::steady_clock::now();
        while (!cpu.rt.halted() && !cpu.stopped())
        {
            Unit u;
            {
                std::lock_guard g(lock);
                u = units[unit];
            }
            if (!u.running) break;
            cpu.mem.write<double>(stamp, sample_time);
            cpu.mem.write<uint32_t>(stamp + 56, 1);
            cpu.mem.write<uint32_t>(list, 1);
            cpu.mem.write<uint32_t>(list + 8, 2);
            cpu.mem.write<uint32_t>(list + 12, kFramesPerPull * 8);
            cpu.mem.write<uint64_t>(list + 16, samples);
            if (u.render_proc) cpu.call(u.render_proc, {u.render_ref, flags, stamp, 0, kFramesPerPull, list});
            sample_time += kFramesPerPull;
            next += std::chrono::microseconds(int64_t(kFramesPerPull * 1e6 / kSampleRate));
            std::this_thread::sleep_until(next);
        }
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
        units[c.arg(0)].running = false;
        c.ret(0);
    });
    h.fn("_AudioUnitSetProperty", [](Cpu& c) {
        if (uint32_t(c.arg(1)) == kSetRenderCallback && c.arg(4))
        {
            std::lock_guard g(lock);
            units[c.arg(0)].render_proc = c.mem.read<uint64_t>(c.arg(4));
            units[c.arg(0)].render_ref = c.mem.read<uint64_t>(c.arg(4) + 8);
        }
        c.ret(0);
    });
    h.fn("_AudioUnitGetProperty", [](Cpu& c) {
        uint32_t id = uint32_t(c.arg(1));
        if (id == kStreamFormat && c.arg(4))
            write_format(c, c.arg(4));
        else if (id == kMaximumFramesPerSlice && c.arg(4))
            c.mem.write<uint32_t>(c.arg(4), 4096);
        else if (c.arg(4) && c.arg(5))
            std::memset(c.mem.host(c.arg(4)), 0, c.mem.read<uint32_t>(c.arg(5)));
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
        units[c.arg(0)].running = false;
        c.ret(0);
    });
}

void register_queues_and_files(Hle& h)
{
    constexpr uint64_t kUnsupported = 0x7479703f;
    for (const char* n : {"_AudioQueueNewOutput", "_AudioQueueNewInput", "_AudioFileOpenWithCallbacks"})
        h.fn(n, [](Cpu& c) { c.ret(kUnsupported); });
    for (const char* n : {"_AudioQueueAllocateBuffer", "_AudioQueueAllocateBufferWithPacketDescriptions", "_AudioQueueDispose",
                          "_AudioQueueEnqueueBuffer", "_AudioQueueFlush", "_AudioQueueOfflineRender", "_AudioQueuePause",
                          "_AudioQueueSetOfflineRenderFormat", "_AudioQueueSetProperty", "_AudioQueueStart", "_AudioQueueStop",
                          "_AudioFileClose", "_AudioFileGetProperty", "_AudioFileGetPropertyInfo", "_AudioFileReadPacketData"})
        h.fn(n, [](Cpu& c) { c.ret(kUnsupported); });
    h.fn("_AudioServicesCreateSystemSoundID", [](Cpu& c) {
        c.mem.write<uint32_t>(c.arg(1), 4096);
        c.ret(0);
    });
    for (const char* n : {"_AudioServicesDisposeSystemSoundID", "_AudioServicesPlaySystemSound", "_AudioServicesAddSystemSoundCompletion",
                          "_AudioServicesRemoveSystemSoundCompletion", "_AudioServicesSetProperty"})
        h.fn(n, [](Cpu& c) { c.ret(0); });
    h.fn("_AudioServicesPlaySystemSoundWithCompletion", [](Cpu& c) {});
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

void register_audio(objc::ObjcRuntime& o)
{
    register_audio_units(o.rt.hle);
    register_queues_and_files(o.rt.hle);
    register_session(o);
}

}
