#include "frameworks/Metal/astc.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace orchard::metal::astc
{
namespace
{
struct Bits128
{
    uint64_t lo = 0, hi = 0;

    uint32_t get(int pos, int n) const
    {
        if (n <= 0) return 0;
        uint64_t v;
        if (pos >= 64)
            v = hi >> (pos - 64);
        else if (pos + n <= 64)
            v = lo >> pos;
        else
            v = (lo >> pos) | (hi << (64 - pos));
        return uint32_t(v & ((uint64_t(1) << n) - 1));
    }
};

uint64_t reverse64(uint64_t v)
{
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    v = ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFull) | ((v & 0x0000FFFF0000FFFFull) << 16);
    return (v >> 32) | (v << 32);
}

struct Range
{
    int trits, quints, bits;
};
constexpr Range kRanges[21] = {{0, 0, 1}, {1, 0, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 1}, {0, 0, 3}, {0, 1, 1},
                               {1, 0, 2}, {0, 0, 4}, {0, 1, 2}, {1, 0, 3}, {0, 0, 5}, {0, 1, 3}, {1, 0, 4},
                               {0, 0, 6}, {0, 1, 4}, {1, 0, 5}, {0, 0, 7}, {0, 1, 5}, {1, 0, 6}, {0, 0, 8}};

int ise_bits(int count, int q)
{
    const Range& r = kRanges[q];
    return r.bits * count + (r.trits ? (8 * count + 4) / 5 : 0) + (r.quints ? (7 * count + 2) / 3 : 0);
}

struct TritQuintTables
{
    uint8_t trits[256][5];
    uint8_t quints[128][3];

    TritQuintTables()
    {
        auto bit = [](int v, int i) { return (v >> i) & 1; };
        for (int T = 0; T < 256; ++T)
        {
            int C, t4, t3, t2, t1, t0;
            if (((T >> 2) & 7) == 7)
            {
                C = (((T >> 5) & 7) << 2) | (T & 3);
                t4 = t3 = 2;
            }
            else
            {
                C = T & 0x1F;
                if (((T >> 5) & 3) == 3)
                {
                    t4 = 2;
                    t3 = bit(T, 7);
                }
                else
                {
                    t4 = bit(T, 7);
                    t3 = (T >> 5) & 3;
                }
            }
            if ((C & 3) == 3)
            {
                t2 = 2;
                t1 = bit(C, 4);
                t0 = (bit(C, 3) << 1) | (bit(C, 2) & ~bit(C, 3) & 1);
            }
            else if (((C >> 2) & 3) == 3)
            {
                t2 = 2;
                t1 = 2;
                t0 = C & 3;
            }
            else
            {
                t2 = bit(C, 4);
                t1 = (C >> 2) & 3;
                t0 = (bit(C, 1) << 1) | (bit(C, 0) & ~bit(C, 1) & 1);
            }
            trits[T][0] = uint8_t(t0);
            trits[T][1] = uint8_t(t1);
            trits[T][2] = uint8_t(t2);
            trits[T][3] = uint8_t(t3);
            trits[T][4] = uint8_t(t4);
        }
        for (int Q = 0; Q < 128; ++Q)
        {
            int q2, q1, q0;
            if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0)
            {
                q2 = (bit(Q, 0) << 2) | ((bit(Q, 4) & ~bit(Q, 0) & 1) << 1) | (bit(Q, 3) & ~bit(Q, 0) & 1);
                q1 = q0 = 4;
            }
            else
            {
                int C;
                if (((Q >> 1) & 3) == 3)
                {
                    q2 = 4;
                    C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | bit(Q, 0);
                }
                else
                {
                    q2 = (Q >> 5) & 3;
                    C = Q & 0x1F;
                }
                if ((C & 7) == 5)
                {
                    q1 = 4;
                    q0 = (C >> 3) & 3;
                }
                else
                {
                    q1 = (C >> 3) & 3;
                    q0 = C & 7;
                }
            }
            quints[Q][0] = uint8_t(q0);
            quints[Q][1] = uint8_t(q1);
            quints[Q][2] = uint8_t(q2);
        }
    }
};

const TritQuintTables& tq()
{
    static const TritQuintTables t;
    return t;
}

struct IseValue
{
    int m;
    int tq;
};

void ise_decode(const Bits128& b, int start, int count, int q, IseValue* out)
{
    const Range& r = kRanges[q];
    int n = r.bits, pos = start;
    auto take = [&](int k) {
        uint32_t v = b.get(pos, k);
        pos += k;
        return int(v);
    };
    if (r.trits)
    {
        for (int i = 0; i < count; i += 5)
        {
            int m[5] = {}, T = 0;
            m[0] = take(n);
            T |= take(2);
            m[1] = take(n);
            T |= take(2) << 2;
            m[2] = take(n);
            T |= take(1) << 4;
            m[3] = take(n);
            T |= take(2) << 5;
            m[4] = take(n);
            T |= take(1) << 7;
            for (int k = 0; k < 5 && i + k < count; ++k)
                out[i + k] = {m[k], tq().trits[T][k]};
        }
    }
    else if (r.quints)
    {
        for (int i = 0; i < count; i += 3)
        {
            int m[3] = {}, Q = 0;
            m[0] = take(n);
            Q |= take(3);
            m[1] = take(n);
            Q |= take(2) << 3;
            m[2] = take(n);
            Q |= take(2) << 5;
            for (int k = 0; k < 3 && i + k < count; ++k)
                out[i + k] = {m[k], tq().quints[Q][k]};
        }
    }
    else
    {
        for (int i = 0; i < count; ++i)
            out[i] = {take(n), 0};
    }
}

int pattern(const char* p, int m)
{
    int v = 0;
    for (; *p; ++p)
        v = (v << 1) | (*p == '0' ? 0 : (m >> (*p - 'a')) & 1);
    return v;
}

int replicate(int v, int from, int to)
{
    int out = 0, have = 0;
    while (have < to)
    {
        int take = std::min(from, to - have);
        out = (out << take) | (v >> (from - take));
        have += take;
    }
    return out;
}

int unquantize_color(const IseValue& v, int q)
{
    const Range& r = kRanges[q];
    if (!r.trits && !r.quints) return replicate(v.m, r.bits, 8);
    int A = (v.m & 1) ? 0x1FF : 0, B = 0, C = 0;
    if (r.trits)
    {
        switch (r.bits)
        {
        case 1: C = 204; break;
        case 2:
            B = pattern("b000b0bb0", v.m);
            C = 93;
            break;
        case 3:
            B = pattern("cb000cbcb", v.m);
            C = 44;
            break;
        case 4:
            B = pattern("dcb000dcb", v.m);
            C = 22;
            break;
        case 5:
            B = pattern("edcb000ed", v.m);
            C = 11;
            break;
        case 6:
            B = pattern("fedcb000f", v.m);
            C = 5;
            break;
        }
    }
    else
    {
        switch (r.bits)
        {
        case 1: C = 113; break;
        case 2:
            B = pattern("b0000bb00", v.m);
            C = 54;
            break;
        case 3:
            B = pattern("cb0000cbc", v.m);
            C = 26;
            break;
        case 4:
            B = pattern("dcb0000dc", v.m);
            C = 13;
            break;
        case 5:
            B = pattern("edcb0000e", v.m);
            C = 6;
            break;
        }
    }
    int T = v.tq * C + B;
    T ^= A;
    return (A & 0x80) | (T >> 2);
}

int unquantize_weight(const IseValue& v, int q)
{
    const Range& r = kRanges[q];
    int w;
    if (!r.trits && !r.quints)
    {
        w = replicate(v.m, r.bits, 6);
    }
    else if (r.bits == 0)
    {
        static const int k3[] = {0, 32, 63}, k5[] = {0, 16, 32, 47, 63};
        w = r.trits ? k3[v.tq] : k5[v.tq];
    }
    else
    {
        int A = (v.m & 1) ? 0x7F : 0, B = 0, C = 0;
        if (r.trits)
        {
            switch (r.bits)
            {
            case 1: C = 50; break;
            case 2:
                B = pattern("b000b0b", v.m);
                C = 23;
                break;
            case 3:
                B = pattern("cb000cb", v.m);
                C = 11;
                break;
            }
        }
        else
        {
            switch (r.bits)
            {
            case 1: C = 28; break;
            case 2:
                B = pattern("b0000b0", v.m);
                C = 13;
                break;
            }
        }
        int T = v.tq * C + B;
        T ^= A;
        w = (A & 0x20) | (T >> 2);
    }
    return w > 32 ? w + 1 : w;
}

struct BlockMode
{
    int grid_w = 0, grid_h = 0, quant = 0;
    bool dual = false;
    int weight_bits = 0;
};

bool decode_block_mode(uint32_t mode, BlockMode& bm)
{
    uint32_t base_quant = (mode >> 4) & 1, H = (mode >> 9) & 1, D = (mode >> 10) & 1, A = (mode >> 5) & 3;
    int x = 0, y = 0;
    if (mode & 3)
    {
        base_quant |= (mode & 3) << 1;
        uint32_t B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3)
        {
        case 0:
            x = int(B) + 4;
            y = int(A) + 2;
            break;
        case 1:
            x = int(B) + 8;
            y = int(A) + 2;
            break;
        case 2:
            x = int(A) + 2;
            y = int(B) + 8;
            break;
        case 3:
            B &= 1;
            if (mode & 0x100)
            {
                x = int(B) + 2;
                y = int(A) + 2;
            }
            else
            {
                x = int(A) + 2;
                y = int(B) + 6;
            }
            break;
        }
    }
    else
    {
        base_quant |= ((mode >> 2) & 3) << 1;
        if (((mode >> 2) & 3) == 0) return false;
        uint32_t B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3)
        {
        case 0:
            x = 12;
            y = int(A) + 2;
            break;
        case 1:
            x = int(A) + 2;
            y = 12;
            break;
        case 2:
            x = int(A) + 6;
            y = int(B) + 6;
            D = 0;
            H = 0;
            break;
        case 3:
            if (((mode >> 5) & 3) == 0)
            {
                x = 6;
                y = 10;
            }
            else if (((mode >> 5) & 3) == 1)
            {
                x = 10;
                y = 6;
            }
            else
            {
                return false;
            }
            break;
        }
    }
    bm.grid_w = x;
    bm.grid_h = y;
    bm.dual = D != 0;
    bm.quant = int(base_quant) - 2 + 6 * int(H);
    int count = x * y * (bm.dual ? 2 : 1);
    if (bm.quant < 0 || bm.quant > 11 || count > 64) return false;
    bm.weight_bits = ise_bits(count, bm.quant);
    return bm.weight_bits >= 24 && bm.weight_bits <= 96;
}

uint32_t hash52(uint32_t p)
{
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}

int select_partition(int seed, int x, int y, int z, int count, bool small_block)
{
    if (small_block)
    {
        x <<= 1;
        y <<= 1;
        z <<= 1;
    }
    seed += (count - 1) * 1024;
    uint32_t r = hash52(uint32_t(seed));
    int s[13];
    s[1] = r & 0xF;
    s[2] = (r >> 4) & 0xF;
    s[3] = (r >> 8) & 0xF;
    s[4] = (r >> 12) & 0xF;
    s[5] = (r >> 16) & 0xF;
    s[6] = (r >> 20) & 0xF;
    s[7] = (r >> 24) & 0xF;
    s[8] = (r >> 28) & 0xF;
    s[9] = (r >> 18) & 0xF;
    s[10] = (r >> 22) & 0xF;
    s[11] = (r >> 26) & 0xF;
    s[12] = ((r >> 30) | (r << 2)) & 0xF;
    for (int i = 1; i <= 12; ++i)
        s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1)
    {
        sh1 = (seed & 2) ? 4 : 5;
        sh2 = count == 3 ? 6 : 5;
    }
    else
    {
        sh1 = count == 3 ? 6 : 5;
        sh2 = (seed & 2) ? 4 : 5;
    }
    int sh3 = (seed & 0x10) ? sh1 : sh2;
    for (int i = 1; i <= 8; ++i)
        s[i] >>= (i & 1) ? sh1 : sh2;
    for (int i = 9; i <= 12; ++i)
        s[i] >>= sh3;
    int a = (s[1] * x + s[2] * y + s[11] * z + int(r >> 14)) & 0x3F;
    int b = (s[3] * x + s[4] * y + s[12] * z + int(r >> 10)) & 0x3F;
    int c = (s[5] * x + s[6] * y + s[9] * z + int(r >> 6)) & 0x3F;
    int d = (s[7] * x + s[8] * y + s[10] * z + int(r >> 2)) & 0x3F;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

using Color = std::array<int, 4>;

int clamp255(int v)
{
    return std::clamp(v, 0, 255);
}

void bit_transfer_signed(int& a, int& b)
{
    b >>= 1;
    b |= a & 0x80;
    a >>= 1;
    a &= 0x3F;
    if (a & 0x20) a -= 0x40;
}

Color blue_contract(int r, int g, int b, int a)
{
    return {(r + b) >> 1, (g + b) >> 1, b, a};
}

bool endpoints(int cem, const int* v, Color& e0, Color& e1)
{
    switch (cem)
    {
    case 0:
        e0 = {v[0], v[0], v[0], 255};
        e1 = {v[1], v[1], v[1], 255};
        return true;
    case 1: {
        int l0 = (v[0] >> 2) | (v[1] & 0xC0);
        int l1 = std::min(l0 + (v[1] & 0x3F), 255);
        e0 = {l0, l0, l0, 255};
        e1 = {l1, l1, l1, 255};
        return true;
    }
    case 4:
        e0 = {v[0], v[0], v[0], v[2]};
        e1 = {v[1], v[1], v[1], v[3]};
        return true;
    case 5: {
        int a0 = v[0], b0 = v[1], a2 = v[2], b2 = v[3];
        bit_transfer_signed(b0, a0);
        bit_transfer_signed(b2, a2);
        e0 = {a0, a0, a0, a2};
        e1 = {clamp255(a0 + b0), clamp255(a0 + b0), clamp255(a0 + b0), clamp255(a2 + b2)};
        return true;
    }
    case 6:
        e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255};
        e1 = {v[0], v[1], v[2], 255};
        return true;
    case 8: {
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        if (s1 >= s0)
        {
            e0 = {v[0], v[2], v[4], 255};
            e1 = {v[1], v[3], v[5], 255};
        }
        else
        {
            e0 = blue_contract(v[1], v[3], v[5], 255);
            e1 = blue_contract(v[0], v[2], v[4], 255);
        }
        return true;
    }
    case 9: {
        int x[6];
        std::memcpy(x, v, sizeof(x));
        bit_transfer_signed(x[1], x[0]);
        bit_transfer_signed(x[3], x[2]);
        bit_transfer_signed(x[5], x[4]);
        if (x[1] + x[3] + x[5] >= 0)
        {
            e0 = {x[0], x[2], x[4], 255};
            e1 = {clamp255(x[0] + x[1]), clamp255(x[2] + x[3]), clamp255(x[4] + x[5]), 255};
        }
        else
        {
            e0 = blue_contract(x[0] + x[1], x[2] + x[3], x[4] + x[5], 255);
            e1 = blue_contract(x[0], x[2], x[4], 255);
        }
        for (int i = 0; i < 4; ++i)
        {
            e0[i] = clamp255(e0[i]);
            e1[i] = clamp255(e1[i]);
        }
        return true;
    }
    case 10:
        e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]};
        e1 = {v[0], v[1], v[2], v[5]};
        return true;
    case 12: {
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        if (s1 >= s0)
        {
            e0 = {v[0], v[2], v[4], v[6]};
            e1 = {v[1], v[3], v[5], v[7]};
        }
        else
        {
            e0 = blue_contract(v[1], v[3], v[5], v[7]);
            e1 = blue_contract(v[0], v[2], v[4], v[6]);
        }
        return true;
    }
    case 13: {
        int x[8];
        std::memcpy(x, v, sizeof(x));
        bit_transfer_signed(x[1], x[0]);
        bit_transfer_signed(x[3], x[2]);
        bit_transfer_signed(x[5], x[4]);
        bit_transfer_signed(x[7], x[6]);
        if (x[1] + x[3] + x[5] >= 0)
        {
            e0 = {x[0], x[2], x[4], x[6]};
            e1 = {x[0] + x[1], x[2] + x[3], x[4] + x[5], x[6] + x[7]};
        }
        else
        {
            e0 = blue_contract(x[0] + x[1], x[2] + x[3], x[4] + x[5], x[6] + x[7]);
            e1 = blue_contract(x[0], x[2], x[4], x[6]);
        }
        for (int i = 0; i < 4; ++i)
        {
            e0[i] = clamp255(e0[i]);
            e1[i] = clamp255(e1[i]);
        }
        return true;
    }
    default: return false;
    }
}

void error_block(uint8_t* out, int bw, int bh)
{
    for (int i = 0; i < bw * bh; ++i)
    {
        out[i * 4 + 0] = 255;
        out[i * 4 + 1] = 0;
        out[i * 4 + 2] = 255;
        out[i * 4 + 3] = 255;
    }
}

void decode_block(const uint8_t* src, int bw, int bh, uint8_t* out)
{
    Bits128 b;
    std::memcpy(&b.lo, src, 8);
    std::memcpy(&b.hi, src + 8, 8);
    uint32_t mode = b.get(0, 11);

    if ((mode & 0x1FF) == 0x1FC)
    {
        uint8_t c[4];
        for (int i = 0; i < 4; ++i)
        {
            uint32_t v = b.get(64 + 16 * i, 16);
            c[i] = uint8_t(v >> 8);
        }
        if (mode & 0x200)
        {
            for (int i = 0; i < 4; ++i)
            {
                uint32_t h = b.get(64 + 16 * i, 16);
                int e = int((h >> 10) & 0x1F), m = int(h & 0x3FF);
                float f = e == 0 ? m / 16777216.0f : (1.0f + m / 1024.0f) * float(1 << e) / 32768.0f;
                if (h & 0x8000) f = 0;
                c[i] = uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
        for (int i = 0; i < bw * bh; ++i)
            std::memcpy(out + i * 4, c, 4);
        return;
    }

    BlockMode bm;
    if (!decode_block_mode(mode, bm)) return error_block(out, bw, bh);
    if (bm.grid_w > bw || bm.grid_h > bh) return error_block(out, bw, bh);
    int partitions = int(b.get(11, 2)) + 1;
    if (partitions == 4 && bm.dual) return error_block(out, bw, bh);

    int below_weights = 128 - bm.weight_bits;
    int cems[4] = {};
    int config_start;
    int seed = 0;
    if (partitions == 1)
    {
        cems[0] = int(b.get(13, 4));
        config_start = 17;
    }
    else
    {
        seed = int(b.get(13, 10));
        uint32_t enc = b.get(23, 6);
        config_start = 29;
        if ((enc & 3) == 0)
        {
            for (int i = 0; i < partitions; ++i)
                cems[i] = int(enc >> 2);
        }
        else
        {
            int extra = 3 * partitions - 4;
            below_weights -= extra;
            uint32_t full = (enc >> 2) | (b.get(below_weights, extra) << 4);
            int base = int(enc & 3) - 1;
            for (int i = 0; i < partitions; ++i)
                cems[i] = base + int((full >> i) & 1);
            for (int i = 0; i < partitions; ++i)
                cems[i] = (cems[i] << 2) | int((full >> (partitions + 2 * i)) & 3);
        }
    }
    int plane2_component = -1;
    if (bm.dual)
    {
        below_weights -= 2;
        plane2_component = int(b.get(below_weights, 2));
    }

    int value_count = 0;
    for (int i = 0; i < partitions; ++i)
        value_count += ((cems[i] >> 2) + 1) * 2;
    if (value_count > 18) return error_block(out, bw, bh);
    int color_bits = below_weights - config_start;
    int cq = -1;
    for (int q = 20; q >= 4; --q)
        if (ise_bits(value_count, q) <= color_bits)
        {
            cq = q;
            break;
        }
    if (cq < 0) return error_block(out, bw, bh);
    IseValue raw[18];
    ise_decode(b, config_start, value_count, cq, raw);
    int values[18];
    for (int i = 0; i < value_count; ++i)
        values[i] = unquantize_color(raw[i], cq);
    Color e0[4], e1[4];
    for (int i = 0, at = 0; i < partitions; ++i)
    {
        if (!endpoints(cems[i], values + at, e0[i], e1[i])) return error_block(out, bw, bh);
        at += ((cems[i] >> 2) + 1) * 2;
    }

    Bits128 rev;
    rev.lo = reverse64(b.hi);
    rev.hi = reverse64(b.lo);
    int planes = bm.dual ? 2 : 1;
    int wcount = bm.grid_w * bm.grid_h * planes;
    IseValue wraw[64];
    ise_decode(rev, 0, wcount, bm.quant, wraw);
    int grid[64];
    for (int i = 0; i < wcount; ++i)
        grid[i] = unquantize_weight(wraw[i], bm.quant);

    int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    bool small_block = bw * bh < 31;
    for (int t = 0; t < bh; ++t)
    {
        for (int s = 0; s < bw; ++s)
        {
            int gs = (Ds * s * (bm.grid_w - 1) + 32) >> 6, gt = (Dt * t * (bm.grid_h - 1) + 32) >> 6;
            int js = gs >> 4, fs = gs & 0xF, jt = gt >> 4, ft = gt & 0xF;
            int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int w[2] = {};
            for (int p = 0; p < planes; ++p)
            {
                auto at = [&](int x, int y) {
                    x = std::min(x, bm.grid_w - 1);
                    y = std::min(y, bm.grid_h - 1);
                    return grid[(y * bm.grid_w + x) * planes + p];
                };
                w[p] = (at(js, jt) * w00 + at(js + 1, jt) * w01 + at(js, jt + 1) * w10 + at(js + 1, jt + 1) * w11 + 8) >> 4;
            }
            int part = partitions > 1 ? select_partition(seed, s, t, 0, partitions, small_block) : 0;
            uint8_t* px = out + (t * bw + s) * 4;
            for (int c = 0; c < 4; ++c)
            {
                int wt = c == plane2_component ? w[1] : w[0];
                int c0 = e0[part][c] * 257, c1 = e1[part][c] * 257;
                int v = (c0 * (64 - wt) + c1 * wt + 32) >> 6;
                px[c] = uint8_t(v >> 8);
            }
        }
    }
}

}

void decode(const uint8_t* src, uint64_t bpr, int width, int height, int block_w, int block_h, uint8_t* rgba)
{
    int bx = (width + block_w - 1) / block_w, by = (height + block_h - 1) / block_h;
    if (!bpr) bpr = uint64_t(bx) * 16;
    uint8_t texels[12 * 12 * 4];
    for (int y = 0; y < by; ++y)
    {
        for (int x = 0; x < bx; ++x)
        {
            decode_block(src + uint64_t(y) * bpr + uint64_t(x) * 16, block_w, block_h, texels);
            for (int ty = 0; ty < block_h; ++ty)
            {
                int py = y * block_h + ty;
                if (py >= height) break;
                int n = std::min(block_w, width - x * block_w);
                std::memcpy(rgba + (uint64_t(py) * width + uint64_t(x) * block_w) * 4, texels + ty * block_w * 4, size_t(n) * 4);
            }
        }
    }
}

}
