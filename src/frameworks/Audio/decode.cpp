#include "frameworks/Audio/decode.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace orchard::audio
{
namespace
{
uint16_t be16(const uint8_t* p)
{
    return uint16_t(p[0] << 8 | p[1]);
}
uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint64_t be64(const uint8_t* p)
{
    return uint64_t(be32(p)) << 32 | be32(p + 4);
}
uint16_t le16(const uint8_t* p)
{
    return uint16_t(p[0] | p[1] << 8);
}
uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

double be_double(const uint8_t* p)
{
    uint64_t v = be64(p);
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}

double extended80(const uint8_t* p)
{
    int exponent = ((p[0] & 0x7f) << 8 | p[1]) - 16383 - 63;
    double v = std::ldexp(double(be64(p + 2)), exponent);
    return (p[0] & 0x80) ? -v : v;
}

constexpr int kImaSteps[89] = {7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
                               31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
                               130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
                               544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
                               2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
                               9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

void decode_ima4(const uint8_t* data, size_t bytes, uint32_t channels, std::vector<float>& out)
{
    constexpr size_t kPacket = 34, kFrames = 64;
    size_t packet_bytes = kPacket * channels;
    std::vector<float> block(kFrames * channels);
    for (size_t off = 0; off + packet_bytes <= bytes; off += packet_bytes)
    {
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            const uint8_t* p = data + off + ch * kPacket;
            uint16_t header = be16(p);
            int predictor = int16_t(header & 0xff80);
            int index = std::clamp(int(header & 0x7f), 0, 88);
            for (size_t i = 0; i < kFrames; ++i)
            {
                int nibble = (p[2 + i / 2] >> ((i & 1) * 4)) & 0xf;
                int step = kImaSteps[index];
                int diff = step >> 3;
                if (nibble & 1) diff += step >> 2;
                if (nibble & 2) diff += step >> 1;
                if (nibble & 4) diff += step;
                predictor = std::clamp(predictor + ((nibble & 8) ? -diff : diff), -32768, 32767);
                index = std::clamp(index + kImaIndex[nibble], 0, 88);
                block[i * channels + ch] = predictor / 32768.0f;
            }
        }
        out.insert(out.end(), block.begin(), block.end());
    }
}

bool decode_caf(const std::vector<uint8_t>& b, Pcm& out, std::string* error)
{
    uint32_t format = 0, flags = 0, bits = 0, channels = 0;
    double rate = 0;
    const uint8_t* data = nullptr;
    size_t data_bytes = 0;
    for (size_t off = 8; off + 12 <= b.size();)
    {
        uint32_t type = be32(&b[off]);
        int64_t size = int64_t(be64(&b[off + 4]));
        size_t body = off + 12;
        if (size < 0 || body + size > b.size()) size = int64_t(b.size() - body);
        if (type == 'desc' && size >= 32)
        {
            rate = be_double(&b[body]);
            format = be32(&b[body + 8]);
            flags = be32(&b[body + 12]);
            channels = be32(&b[body + 24]);
            bits = be32(&b[body + 28]);
        }
        else if (type == 'data' && size >= 4)
        {
            data = &b[body + 4];
            data_bytes = size_t(size - 4);
        }
        off = body + size_t(size);
    }
    if (!data || !channels || rate <= 0)
    {
        if (error) *error = "CAF file without desc or data";
        return false;
    }
    out.rate = rate;
    out.channels = channels;
    if (format == 'ima4')
    {
        decode_ima4(data, data_bytes, channels, out.samples);
        return true;
    }
    if (format == 'lpcm')
    {
        LinearFormat f{rate, channels, bits, bool(flags & 1), !(flags & 2), false};
        append_linear(data, data_bytes, f, out.samples);
        return true;
    }
    if (error) *error = "CAF with unsupported codec";
    return false;
}

bool decode_aiff(const std::vector<uint8_t>& b, Pcm& out, std::string* error)
{
    bool aifc = be32(&b[8]) == 'AIFC';
    uint32_t channels = 0, bits = 16, compression = 'NONE';
    double rate = 0;
    const uint8_t* data = nullptr;
    size_t data_bytes = 0;
    for (size_t off = 12; off + 8 <= b.size();)
    {
        uint32_t type = be32(&b[off]);
        size_t size = std::min<size_t>(be32(&b[off + 4]), b.size() - off - 8);
        const uint8_t* body = &b[off + 8];
        if (type == 'COMM' && size >= 18)
        {
            channels = be16(body);
            bits = be16(body + 6);
            rate = extended80(body + 8);
            if (aifc && size >= 22) compression = be32(body + 18);
        }
        else if (type == 'SSND' && size >= 8)
        {
            uint32_t skip = be32(body);
            data = body + 8 + skip;
            data_bytes = size - 8 - std::min<size_t>(skip, size - 8);
        }
        off += 8 + size + (size & 1);
    }
    if (!data || !channels || rate <= 0)
    {
        if (error) *error = "AIFF file without COMM or SSND";
        return false;
    }
    out.rate = rate;
    out.channels = channels;
    if (compression == 'ima4')
    {
        decode_ima4(data, data_bytes, channels, out.samples);
        return true;
    }
    LinearFormat f{rate, channels, bits, compression == 'fl32', compression != 'sowt', false};
    if (compression != 'NONE' && compression != 'sowt' && compression != 'fl32' && compression != 'twos')
    {
        if (error) *error = "AIFC with unsupported compression";
        return false;
    }
    append_linear(data, data_bytes, f, out.samples);
    return true;
}

bool decode_wav(const std::vector<uint8_t>& b, Pcm& out)
{
    uint32_t tag = 0, channels = 0, bits = 0, rate = 0;
    const uint8_t* data = nullptr;
    size_t data_bytes = 0;
    for (size_t off = 12; off + 8 <= b.size();)
    {
        uint32_t type = be32(&b[off]);
        size_t size = std::min<size_t>(le32(&b[off + 4]), b.size() - off - 8);
        const uint8_t* body = &b[off + 8];
        if (type == 'fmt ' && size >= 16)
        {
            tag = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            if (tag == 0xfffe && size >= 26) tag = le16(body + 24);
        }
        else if (type == 'data')
        {
            data = body;
            data_bytes = size;
        }
        off += 8 + size + (size & 1);
    }
    if (!data || !channels || !rate || (tag != 1 && tag != 3)) return false;
    out.rate = rate;
    out.channels = channels;
    append_linear(data, data_bytes, LinearFormat{double(rate), channels, bits, tag == 3, false, false}, out.samples);
    return true;
}

bool decode_media_foundation(const std::vector<uint8_t>& b, Pcm& out, std::string* error)
{
    static std::once_flag started;
    std::call_once(started, [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
    });
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IStream* stream = SHCreateMemStream(b.data(), UINT(b.size()));
    IMFByteStream* bytes = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* want = nullptr;
    IMFMediaType* got = nullptr;
    bool ok = stream && SUCCEEDED(MFCreateMFByteStreamOnStream(stream, &bytes)) &&
              SUCCEEDED(MFCreateSourceReaderFromByteStream(bytes, nullptr, &reader)) && SUCCEEDED(MFCreateMediaType(&want)) &&
              SUCCEEDED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) &&
              SUCCEEDED(want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) &&
              SUCCEEDED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want)) &&
              SUCCEEDED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got));
    if (ok)
    {
        UINT32 channels = 0, rate = 0;
        got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        out.channels = channels;
        out.rate = rate;
        for (;;)
        {
            DWORD flags = 0;
            IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample))) break;
            if (sample)
            {
                IMFMediaBuffer* buffer = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
                {
                    BYTE* p = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(buffer->Lock(&p, nullptr, &len)))
                    {
                        auto* f = reinterpret_cast<const float*>(p);
                        out.samples.insert(out.samples.end(), f, f + len / sizeof(float));
                        buffer->Unlock();
                    }
                    buffer->Release();
                }
                sample->Release();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        ok = channels && rate && !out.samples.empty();
    }
    for (IUnknown* p : {static_cast<IUnknown*>(got), static_cast<IUnknown*>(want), static_cast<IUnknown*>(reader),
                        static_cast<IUnknown*>(bytes), static_cast<IUnknown*>(stream)})
        if (p) p->Release();
    if (!ok && error) *error = "Media Foundation could not decode the file";
    return ok;
}
}

void append_linear(const uint8_t* data, size_t bytes, const LinearFormat& f, std::vector<float>& out)
{
    uint32_t sample_bytes = std::max<uint32_t>(f.bits / 8, 1);
    size_t frame_bytes = size_t(sample_bytes) * f.channels;
    size_t frames = f.non_interleaved ? bytes / sample_bytes / f.channels : bytes / frame_bytes;
    size_t base = out.size();
    out.resize(base + frames * f.channels);
    for (size_t i = 0; i < frames; ++i)
        for (uint32_t ch = 0; ch < f.channels; ++ch)
        {
            const uint8_t* p =
                f.non_interleaved ? data + (size_t(ch) * frames + i) * sample_bytes : data + i * frame_bytes + ch * sample_bytes;
            uint8_t s[8] = {};
            for (uint32_t k = 0; k < sample_bytes && k < 8; ++k)
                s[k] = f.big_endian ? p[sample_bytes - 1 - k] : p[k];
            float v;
            if (f.is_float && sample_bytes == 4)
                std::memcpy(&v, s, 4);
            else if (f.is_float && sample_bytes == 8)
            {
                double d;
                std::memcpy(&d, s, 8);
                v = float(d);
            }
            else if (sample_bytes == 1)
                v = (int(s[0]) - 128) / 128.0f;
            else if (sample_bytes == 2)
                v = int16_t(s[0] | s[1] << 8) / 32768.0f;
            else if (sample_bytes == 3)
                v = float(int32_t(uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24) / 2147483648.0);
            else
                v = float(int32_t(le32(s)) / 2147483648.0);
            out[base + i * f.channels + ch] = v;
        }
}

bool decode_audio(const std::vector<uint8_t>& bytes, Pcm& out, std::string* error)
{
    out = {};
    if (bytes.size() >= 12)
    {
        uint32_t magic = be32(bytes.data());
        if (magic == 'caff') return decode_caf(bytes, out, error);
        if (magic == 'FORM' && (be32(&bytes[8]) == 'AIFF' || be32(&bytes[8]) == 'AIFC')) return decode_aiff(bytes, out, error);
        if (magic == 'RIFF' && be32(&bytes[8]) == 'WAVE' && decode_wav(bytes, out)) return true;
    }
    out = {};
    return decode_media_foundation(bytes, out, error);
}

}
