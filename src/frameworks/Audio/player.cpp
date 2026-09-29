#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "frameworks/Audio/decode.h"
#include "frameworks/Audio/mixer.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "hle/hle.h"
#include "objc/runtime.h"

namespace orchard::audio
{
using objc::objc;
using Id = GuestAddr;

namespace
{
constexpr uint32_t kLinearPcm = 0x6c70636d, kUnsupportedFormat = 0x666d743f, kIsRunning = 0x6171726e;

Id send(Cpu& c, Id o, const char* sel, std::initializer_list<uint64_t> args = {})
{
    return objc(c).send(c, o, sel, args);
}

bool responds(Cpu& c, Id obj, const char* sel)
{
    return obj && objc(c).responds(objc(c).class_of(obj), objc(c).sel(sel));
}

std::vector<uint8_t> url_bytes(Cpu& c, Id url)
{
    std::vector<uint8_t> out;
    if (!url) return out;
    auto host = c.rt.vfs.to_host(foundation::url_string_path(c, url));
    if (!host) return out;
    std::ifstream f(*host, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(f), {});
    return out;
}

std::vector<uint8_t> data_bytes(Cpu& c, Id data)
{
    std::vector<uint8_t> out;
    if (!data) return out;
    uint64_t n = send(c, data, "length");
    GuestAddr p = send(c, data, "bytes");
    out.resize(n);
    if (n && p) c.mem.read_bytes(p, out.data(), n);
    return out;
}

std::shared_ptr<const Pcm> decode(const std::vector<uint8_t>& bytes)
{
    auto pcm = std::make_shared<Pcm>();
    std::string why;
    if (bytes.empty() || !decode_audio(bytes, *pcm, &why))
    {
        std::printf("[audio] could not decode a sound: %s\n", why.empty() ? "empty file" : why.c_str());
        return nullptr;
    }
    return pcm;
}

std::shared_ptr<Voice> voice_for(const std::shared_ptr<const Pcm>& pcm)
{
    auto v = std::make_shared<Voice>();
    v->pcm = pcm;
    v->rate = pcm->rate;
    v->channels = pcm->channels;
    return v;
}

struct Player
{
    std::shared_ptr<const Pcm> pcm;
    std::shared_ptr<Voice> voice;
    Id url = 0, data = 0, delegate = 0;
    float rate = 1;
    bool retained = false;
    bool metering = false;
};

std::mutex players_lock;
std::unordered_map<Id, std::shared_ptr<Player>> players;

std::shared_ptr<Player> player(Id obj)
{
    std::lock_guard g(players_lock);
    auto it = players.find(obj);
    return it == players.end() ? nullptr : it->second;
}

GuestAddr finish_stub(Cpu& c)
{
    static GuestAddr stub = c.rt.hle.make_stub("AVAudioPlayer finished", [](Cpu& k) {
        Id obj = k.arg(0);
        auto p = player(obj);
        if (!p) return;
        if (responds(k, p->delegate, "audioPlayerDidFinishPlaying:successfully:"))
            send(k, p->delegate, "audioPlayerDidFinishPlaying:successfully:", {obj, 1});
        if (p->retained)
        {
            p->retained = false;
            objc(k).release(k, obj);
        }
    });
    return stub;
}

Id init_player(Cpu& c, Id self, const std::vector<uint8_t>& bytes, Id url, Id data, GuestAddr error_out)
{
    auto pcm = decode(bytes);
    if (!pcm)
    {
        if (error_out)
        {
            Id err = send(c, objc(c).host_class("NSError")->addr,
                          "errorWithDomain:code:userInfo:", {foundation::string_autoreleased(c, "NSOSStatusErrorDomain"), 1954115647, 0});
            c.mem.write<uint64_t>(error_out, err);
        }
        objc(c).dispose(self);
        return 0;
    }
    auto p = std::make_shared<Player>();
    p->pcm = pcm;
    p->voice = voice_for(pcm);
    p->url = url ? objc(c).retain(url) : 0;
    p->data = data ? objc(c).retain(data) : 0;
    GuestAddr main_queue = main_queue_object(c);
    GuestAddr stub = finish_stub(c);
    p->voice->on_finish = [main_queue, stub, self] { dispatch_function_async(main_queue, stub, self); };
    std::lock_guard g(players_lock);
    players[self] = p;
    return self;
}

template <typename F> auto with_voice(Cpu& c, F&& f)
{
    auto p = player(c.arg(0));
    std::lock_guard g(Mixer::get().lock());
    return f(p.get(), p ? p->voice.get() : nullptr);
}

void register_player(objc::ObjcRuntime& o)
{
    o.define("AVAudioPlayer", "NSObject");
    const char* P = "AVAudioPlayer";
    o.method(
        P, "initWithContentsOfURL:error:", [](Cpu& c) { c.ret(init_player(c, c.arg(0), url_bytes(c, c.arg(2)), c.arg(2), 0, c.arg(3))); });
    o.method(P, "initWithContentsOfURL:fileTypeHint:error:", [](Cpu& c) {
        c.ret(init_player(c, c.arg(0), url_bytes(c, c.arg(2)), c.arg(2), 0, c.arg(4)));
    });
    o.method(P, "initWithData:error:", [](Cpu& c) { c.ret(init_player(c, c.arg(0), data_bytes(c, c.arg(2)), 0, c.arg(2), c.arg(3))); });
    o.method(P, "initWithData:fileTypeHint:error:", [](Cpu& c) {
        c.ret(init_player(c, c.arg(0), data_bytes(c, c.arg(2)), 0, c.arg(2), c.arg(4)));
    });
    o.method(P, "dealloc", [](Cpu& c) {
        auto p = player(c.arg(0));
        if (p)
        {
            Mixer::get().remove(p->voice);
            if (p->url) objc(c).release(c, p->url);
            if (p->data) objc(c).release(c, p->data);
            std::lock_guard g(players_lock);
            players.erase(c.arg(0));
        }
        objc(c).dispose(c.arg(0));
    });
    o.method(P, "prepareToPlay", [](Cpu& c) { c.ret(player(c.arg(0)) != nullptr); });
    auto play = [](Cpu& c) {
        auto p = player(c.arg(0));
        if (!p) return c.ret(0);
        {
            std::lock_guard g(Mixer::get().lock());
            p->voice->playing = true;
        }
        Mixer::get().add(p->voice);
        if (!p->retained)
        {
            p->retained = true;
            objc(c).retain(c.arg(0));
        }
        c.ret(1);
    };
    static decltype(play) s_play = play;
    o.method(P, "play", [](Cpu& c) { s_play(c); });
    o.method(P, "playAtTime:", [](Cpu& c) { s_play(c); });
    o.method(P, "pause", [](Cpu& c) {
        with_voice(c, [](Player*, Voice* v) {
            if (v) v->playing = false;
        });
    });
    o.method(P, "stop", [](Cpu& c) {
        with_voice(c, [](Player*, Voice* v) {
            if (v) v->playing = false;
        });
        auto p = player(c.arg(0));
        if (p && p->retained)
        {
            p->retained = false;
            objc(c).release(c, c.arg(0));
        }
    });
    o.method(P, "isPlaying", [](Cpu& c) { c.ret(with_voice(c, [](Player*, Voice* v) { return v && v->playing; })); });
    o.method(P, "volume", [](Cpu& c) { c.set_s(0, with_voice(c, [](Player*, Voice* v) { return v ? v->volume : 0.0f; })); });
    auto set_volume = [](Cpu& c) {
        float value = std::clamp(c.s(0), 0.0f, 1.0f);
        with_voice(c, [value](Player*, Voice* v) {
            if (v) v->volume = value;
        });
    };
    static decltype(set_volume) s_set_volume = set_volume;
    o.method(P, "setVolume:", [](Cpu& c) { s_set_volume(c); });
    o.method(P, "setVolume:fadeDuration:", [](Cpu& c) { s_set_volume(c); });
    o.method(P, "pan", [](Cpu& c) { c.set_s(0, with_voice(c, [](Player*, Voice* v) { return v ? v->pan : 0.0f; })); });
    o.method(P, "setPan:", [](Cpu& c) {
        float value = std::clamp(c.s(0), -1.0f, 1.0f);
        with_voice(c, [value](Player*, Voice* v) {
            if (v) v->pan = value;
        });
    });
    o.method(P, "numberOfLoops",
             [](Cpu& c) { c.ret(uint64_t(int64_t(with_voice(c, [](Player*, Voice* v) { return v ? v->loops : 0; })))); });
    o.method(P, "setNumberOfLoops:", [](Cpu& c) {
        int loops = int(int64_t(c.arg(2)));
        with_voice(c, [loops](Player*, Voice* v) {
            if (v) v->loops = loops;
        });
    });
    o.method(P, "currentTime", [](Cpu& c) {
        c.set_d(0, with_voice(c, [](Player* p, Voice* v) { return v && p->pcm->rate > 0 ? v->position / p->pcm->rate : 0.0; }));
    });
    o.method(P, "setCurrentTime:", [](Cpu& c) {
        double t = std::max(0.0, c.d(0));
        with_voice(c, [t](Player* p, Voice* v) {
            if (v) v->position = t * p->pcm->rate;
        });
    });
    o.method(P, "duration", [](Cpu& c) {
        auto p = player(c.arg(0));
        c.set_d(0, p ? p->pcm->seconds() : 0.0);
    });
    o.method(P, "deviceCurrentTime",
             [](Cpu& c) { c.set_d(0, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()); });
    o.method(P, "numberOfChannels", [](Cpu& c) {
        auto p = player(c.arg(0));
        c.ret(p ? p->pcm->channels : 0);
    });
    o.method(P, "url", [](Cpu& c) {
        auto p = player(c.arg(0));
        c.ret(p ? p->url : 0);
    });
    o.method(P, "data", [](Cpu& c) {
        auto p = player(c.arg(0));
        c.ret(p ? p->data : 0);
    });
    o.method(P, "delegate", [](Cpu& c) {
        auto p = player(c.arg(0));
        c.ret(p ? p->delegate : 0);
    });
    o.method(P, "setDelegate:", [](Cpu& c) {
        if (auto p = player(c.arg(0))) p->delegate = c.arg(2);
    });
    o.method(P, "rate", [](Cpu& c) {
        auto p = player(c.arg(0));
        c.set_s(0, p ? p->rate : 1.0f);
    });
    o.method(P, "setRate:", [](Cpu& c) {
        float r = std::clamp(c.s(0), 0.5f, 2.0f);
        with_voice(c, [r](Player* p, Voice* v) {
            if (!v) return;
            p->rate = r;
            v->rate = p->pcm->rate * r;
        });
    });
    o.method(P, "enableRate", [](Cpu& c) { c.ret(1); });
    o.method(P, "setEnableRate:", [](Cpu& c) {});
    o.method(P, "isMeteringEnabled", [](Cpu& c) {
        auto p = player(c.arg(0));
        c.ret(p && p->metering);
    });
    o.method(P, "setMeteringEnabled:", [](Cpu& c) {
        if (auto p = player(c.arg(0))) p->metering = c.arg(2) & 1;
    });
    o.method(P, "updateMeters", [](Cpu& c) {});
    o.method(P, "averagePowerForChannel:", [](Cpu& c) { c.set_s(0, -160.0f); });
    o.method(P, "peakPowerForChannel:", [](Cpu& c) { c.set_s(0, -160.0f); });
}

struct SystemSound
{
    std::shared_ptr<const Pcm> pcm;
    GuestAddr completion = 0, client_data = 0;
};

std::mutex sounds_lock;
std::unordered_map<uint32_t, SystemSound> sounds;
std::atomic<uint32_t> next_sound{4097};

GuestAddr sound_done_stub(Cpu& c)
{
    static GuestAddr stub = c.rt.hle.make_stub("system sound finished", [](Cpu& k) {
        GuestAddr ctx = k.arg(0);
        uint32_t id = k.mem.read<uint32_t>(ctx);
        GuestAddr block = k.mem.read<uint64_t>(ctx + 8);
        SystemSound s;
        {
            std::lock_guard g(sounds_lock);
            if (auto it = sounds.find(id); it != sounds.end()) s = it->second;
        }
        if (block)
        {
            call_block(k, block);
            block_release(k, block);
        }
        else if (s.completion)
        {
            k.call(s.completion, {id, s.client_data});
        }
        k.rt.heap.free(ctx);
    });
    return stub;
}

void play_sound(Cpu& c, uint32_t id, GuestAddr block)
{
    SystemSound s;
    {
        std::lock_guard g(sounds_lock);
        auto it = sounds.find(id);
        if (it == sounds.end()) return;
        s = it->second;
    }
    auto v = voice_for(s.pcm);
    if (block || s.completion)
    {
        GuestAddr ctx = c.rt.heap.calloc(16);
        c.mem.write<uint32_t>(ctx, id);
        c.mem.write<uint64_t>(ctx + 8, block ? block_copy(c, block) : 0);
        GuestAddr main_queue = main_queue_object(c), stub = sound_done_stub(c);
        v->on_finish = [main_queue, stub, ctx] { dispatch_function_async(main_queue, stub, ctx); };
    }
    v->playing = true;
    Mixer::get().add(v);
}

void register_system_sounds(Hle& h)
{
    h.fn("_AudioServicesCreateSystemSoundID", [](Cpu& c) {
        auto pcm = decode(url_bytes(c, c.arg(0)));
        if (!pcm) return c.ret(uint64_t(-1500));
        uint32_t id = next_sound++;
        {
            std::lock_guard g(sounds_lock);
            sounds[id].pcm = pcm;
        }
        c.mem.write<uint32_t>(c.arg(1), id);
        c.ret(0);
    });
    h.fn("_AudioServicesDisposeSystemSoundID", [](Cpu& c) {
        std::lock_guard g(sounds_lock);
        sounds.erase(uint32_t(c.arg(0)));
        c.ret(0);
    });
    h.fn("_AudioServicesPlaySystemSound", [](Cpu& c) { play_sound(c, uint32_t(c.arg(0)), 0); });
    h.fn("_AudioServicesPlayAlertSound", [](Cpu& c) { play_sound(c, uint32_t(c.arg(0)), 0); });
    h.fn("_AudioServicesPlaySystemSoundWithCompletion", [](Cpu& c) { play_sound(c, uint32_t(c.arg(0)), c.arg(1)); });
    h.fn("_AudioServicesPlayAlertSoundWithCompletion", [](Cpu& c) { play_sound(c, uint32_t(c.arg(0)), c.arg(1)); });
    h.fn("_AudioServicesAddSystemSoundCompletion", [](Cpu& c) {
        std::lock_guard g(sounds_lock);
        if (auto it = sounds.find(uint32_t(c.arg(0))); it != sounds.end())
        {
            it->second.completion = c.arg(3);
            it->second.client_data = c.arg(4);
        }
        c.ret(0);
    });
    h.fn("_AudioServicesRemoveSystemSoundCompletion", [](Cpu& c) {
        std::lock_guard g(sounds_lock);
        if (auto it = sounds.find(uint32_t(c.arg(0))); it != sounds.end()) it->second.completion = 0;
    });
    for (const char* n : {"_AudioServicesSetProperty", "_AudioServicesGetProperty", "_AudioServicesGetPropertyInfo"})
        h.fn(n, [](Cpu& c) { c.ret(0); });
}

struct Queue
{
    LinearFormat format;
    GuestAddr callback = 0, user_data = 0;
    std::shared_ptr<Voice> voice;
    std::mutex m;
    std::condition_variable cv;
    std::deque<GuestAddr> done;
    bool disposed = false;
    double played_frames = 0;
};

std::mutex queues_lock;
std::unordered_map<GuestAddr, std::shared_ptr<Queue>> queues;

std::shared_ptr<Queue> queue(GuestAddr aq)
{
    std::lock_guard g(queues_lock);
    auto it = queues.find(aq);
    return it == queues.end() ? nullptr : it->second;
}

constexpr uint64_t kBufferHeader = 64;

GuestAddr allocate_buffer(Cpu& c, uint32_t bytes, uint32_t descriptions)
{
    GuestAddr b = c.rt.heap.calloc(kBufferHeader + bytes + uint64_t(descriptions) * 16);
    c.mem.write<uint32_t>(b, bytes);
    c.mem.write<uint64_t>(b + 8, b + kBufferHeader);
    c.mem.write<uint32_t>(b + 32, descriptions);
    c.mem.write<uint64_t>(b + 40, descriptions ? b + kBufferHeader + bytes : 0);
    return b;
}

void register_queues(Hle& h)
{
    h.fn("_AudioQueueNewOutput", [](Cpu& c) {
        GuestAddr asbd = c.arg(0);
        if (c.mem.read<uint32_t>(asbd + 8) != kLinearPcm) return c.ret(kUnsupportedFormat);
        uint32_t flags = c.mem.read<uint32_t>(asbd + 12);
        auto q = std::make_shared<Queue>();
        q->format.rate = c.mem.read<double>(asbd);
        q->format.channels = std::max<uint32_t>(c.mem.read<uint32_t>(asbd + 28), 1);
        q->format.bits = c.mem.read<uint32_t>(asbd + 32);
        q->format.is_float = flags & 1;
        q->format.big_endian = flags & 2;
        q->format.non_interleaved = flags & 32;
        q->callback = c.arg(1);
        q->user_data = c.arg(2);
        q->voice = std::make_shared<Voice>();
        q->voice->streaming = true;
        q->voice->rate = q->format.rate;
        q->voice->channels = q->format.channels;
        GuestAddr aq = c.rt.heap.calloc(16);
        std::weak_ptr<Queue> weak = q;
        q->voice->on_chunk_done = [weak](uint64_t buffer) {
            if (auto q = weak.lock())
            {
                std::lock_guard g(q->m);
                q->done.push_back(buffer);
                q->cv.notify_one();
            }
        };
        {
            std::lock_guard g(queues_lock);
            queues[aq] = q;
        }
        c.rt.threads->spawn_host("audio-queue", [q, aq](Cpu& cpu) {
            for (;;)
            {
                GuestAddr buffer;
                {
                    std::unique_lock l(q->m);
                    q->cv.wait(l, [&] { return q->disposed || !q->done.empty(); });
                    if (q->disposed) return;
                    buffer = q->done.front();
                    q->done.pop_front();
                }
                if (cpu.rt.halted()) return;
                cpu.call(q->callback, {q->user_data, aq, buffer});
            }
        });
        c.mem.write<uint64_t>(c.arg(6), aq);
        c.ret(0);
    });
    h.fn("_AudioQueueNewInput", [](Cpu& c) { c.ret(kUnsupportedFormat); });
    h.fn("_AudioQueueNewOutputWithDispatchQueue", [](Cpu& c) { c.ret(kUnsupportedFormat); });
    h.fn("_AudioQueueAllocateBuffer", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(2), allocate_buffer(c, uint32_t(c.arg(1)), 0));
        c.ret(0);
    });
    h.fn("_AudioQueueAllocateBufferWithPacketDescriptions", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(3), allocate_buffer(c, uint32_t(c.arg(1)), uint32_t(c.arg(2))));
        c.ret(0);
    });
    h.fn("_AudioQueueFreeBuffer", [](Cpu& c) {
        c.rt.heap.free(c.arg(1));
        c.ret(0);
    });
    auto enqueue = [](Cpu& c) {
        auto q = queue(c.arg(0));
        if (!q) return c.ret(uint64_t(-66687));
        GuestAddr b = c.arg(1);
        uint32_t bytes = c.mem.read<uint32_t>(b + 16);
        Chunk chunk;
        chunk.tag = b;
        append_linear(c.mem.host(c.mem.read<uint64_t>(b + 8)), bytes, q->format, chunk.samples);
        {
            std::lock_guard g(Mixer::get().lock());
            q->voice->queue.push_back(std::move(chunk));
        }
        c.ret(0);
    };
    static decltype(enqueue) s_enqueue = enqueue;
    h.fn("_AudioQueueEnqueueBuffer", [](Cpu& c) { s_enqueue(c); });
    h.fn("_AudioQueueEnqueueBufferWithParameters", [](Cpu& c) { s_enqueue(c); });
    h.fn("_AudioQueueStart", [](Cpu& c) {
        auto q = queue(c.arg(0));
        if (!q) return c.ret(uint64_t(-66687));
        {
            std::lock_guard g(Mixer::get().lock());
            q->voice->playing = true;
        }
        Mixer::get().add(q->voice);
        c.ret(0);
    });
    auto halt = [](Cpu& c, bool clear) {
        auto q = queue(c.arg(0));
        if (!q) return c.ret(uint64_t(-66687));
        std::lock_guard g(Mixer::get().lock());
        q->voice->playing = false;
        if (clear)
        {
            q->voice->queue.clear();
            q->voice->position = 0;
        }
        c.ret(0);
    };
    static decltype(halt) s_halt = halt;
    h.fn("_AudioQueuePause", [](Cpu& c) { s_halt(c, false); });
    h.fn("_AudioQueueStop", [](Cpu& c) { s_halt(c, true); });
    h.fn("_AudioQueueReset", [](Cpu& c) { s_halt(c, true); });
    h.fn("_AudioQueueFlush", [](Cpu& c) { c.ret(0); });
    h.fn("_AudioQueuePrime", [](Cpu& c) {
        if (c.arg(2)) c.mem.write<uint32_t>(c.arg(2), 0);
        c.ret(0);
    });
    h.fn("_AudioQueueDispose", [](Cpu& c) {
        auto q = queue(c.arg(0));
        if (!q) return c.ret(0);
        Mixer::get().remove(q->voice);
        {
            std::lock_guard g(q->m);
            q->disposed = true;
            q->cv.notify_all();
        }
        std::lock_guard g(queues_lock);
        queues.erase(c.arg(0));
        c.ret(0);
    });
    h.fn("_AudioQueueSetParameter", [](Cpu& c) {
        auto q = queue(c.arg(0));
        if (q && uint32_t(c.arg(1)) == 1)
        {
            std::lock_guard g(Mixer::get().lock());
            q->voice->volume = std::clamp(c.s(0), 0.0f, 1.0f);
        }
        c.ret(0);
    });
    h.fn("_AudioQueueGetParameter", [](Cpu& c) {
        auto q = queue(c.arg(0));
        float v = 1;
        if (q && uint32_t(c.arg(1)) == 1)
        {
            std::lock_guard g(Mixer::get().lock());
            v = q->voice->volume;
        }
        if (c.arg(2)) c.mem.write<float>(c.arg(2), v);
        c.ret(0);
    });
    h.fn("_AudioQueueGetProperty", [](Cpu& c) {
        auto q = queue(c.arg(0));
        if (uint32_t(c.arg(1)) == kIsRunning && c.arg(2))
        {
            bool running = false;
            if (q)
            {
                std::lock_guard g(Mixer::get().lock());
                running = q->voice->playing;
            }
            c.mem.write<uint32_t>(c.arg(2), running);
        }
        c.ret(0);
    });
    for (const char* n :
         {"_AudioQueueSetProperty", "_AudioQueueAddPropertyListener", "_AudioQueueRemovePropertyListener", "_AudioQueueGetPropertySize"})
        h.fn(n, [](Cpu& c) { c.ret(0); });
    h.fn("_AudioQueueGetCurrentTime", [](Cpu& c) {
        if (c.arg(2))
        {
            c.mem.write<double>(c.arg(2), 0);
            c.mem.write<uint32_t>(c.arg(2) + 56, 1);
        }
        if (c.arg(3)) c.mem.write<uint8_t>(c.arg(3), 0);
        c.ret(0);
    });
}
}

void register_players(objc::ObjcRuntime& o)
{
    register_player(o);
    register_system_sounds(o.rt.hle);
    register_queues(o.rt.hle);
}

}
