#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "frameworks/Audio/decode.h"
#include "frameworks/Audio/mixer.h"

using namespace orchard::audio;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: orchard-audiotest <file> [--play]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    auto pcm = std::make_shared<Pcm>();
    std::string error;
    if (!decode_audio(bytes, *pcm, &error))
    {
        std::printf("%s: could not decode (%s)\n", argv[1], error.c_str());
        return 1;
    }
    double sum = 0, peak = 0;
    for (float s : pcm->samples)
    {
        sum += double(s) * s;
        peak = std::max(peak, double(std::fabs(s)));
    }
    double rms = pcm->samples.empty() ? 0 : std::sqrt(sum / double(pcm->samples.size()));
    std::printf("%s: %.0f Hz, %u channels, %.2f s, rms %.4f, peak %.3f\n", argv[1], pcm->rate, pcm->channels, pcm->seconds(), rms, peak);

    if (argc >= 3 && std::strcmp(argv[2], "--play") == 0)
    {
        auto v = std::make_shared<Voice>();
        v->pcm = pcm;
        v->rate = pcm->rate;
        v->channels = pcm->channels;
        v->playing = true;
        bool done = false;
        v->on_finish = [&done] { done = true; };
        Mixer::get().add(v);
        while (!done)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::printf("played\n");
    }
    return 0;
}
