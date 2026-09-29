#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "frameworks/Audio/decode.h"

namespace orchard::audio
{
struct Chunk
{
    std::vector<float> samples;
    uint64_t tag = 0;
};

struct Voice
{
    std::shared_ptr<const Pcm> pcm;
    double rate = 44100;
    uint32_t channels = 2;
    double position = 0;
    int loops = 0;
    float volume = 1, pan = 0;
    bool playing = false;
    bool streaming = false;
    std::deque<Chunk> queue;
    std::function<void()> on_finish;
    std::function<void(uint64_t tag)> on_chunk_done;
};

class Mixer
{
public:
    static Mixer& get();

    std::shared_ptr<Voice> add(std::shared_ptr<Voice> v);
    void remove(const std::shared_ptr<Voice>& v);
    std::mutex& lock() { return lock_; }

private:
    Mixer() = default;
    void start();
    void mix(float* out, uint32_t frames, std::vector<std::function<void()>>& events);

    std::mutex lock_;
    std::vector<std::shared_ptr<Voice>> voices_;
    bool started_ = false;
};

}
