#include "core/plist.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace orchard
{
namespace
{
uint64_t be(std::span<const uint8_t> d, size_t off, size_t n)
{
    if (off + n > d.size()) throw std::runtime_error("plist: truncated");
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v = (v << 8) | d[off + i];
    return v;
}

uint8_t byte_at(std::span<const uint8_t> d, size_t off)
{
    if (off >= d.size()) throw std::runtime_error("plist: truncated");
    return d[off];
}

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80)
    {
        out += char(cp);
    }
    else if (cp < 0x800)
    {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3f));
    }
    else if (cp < 0x10000)
    {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
    else
    {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
}

Plist parse_binary(std::span<const uint8_t> d)
{
    if (d.size() < 40) throw std::runtime_error("plist: too small");
    size_t t = d.size() - 32;
    unsigned off_size = d[t + 6], ref_size = d[t + 7];
    uint64_t count = be(d, t + 8, 8), top = be(d, t + 16, 8), table = be(d, t + 24, 8);

    std::function<Plist(uint64_t, int)> object = [&](uint64_t ref, int depth) -> Plist {
        if (ref >= count || depth > 64) throw std::runtime_error("plist: bad reference");
        size_t at = size_t(be(d, size_t(table + ref * off_size), off_size));
        uint8_t marker = byte_at(d, at);
        uint8_t hi = marker >> 4, lo = marker & 0xf;
        size_t pos = at + 1;
        auto length = [&]() -> uint64_t {
            if (lo != 0xf) return lo;
            uint8_t im = byte_at(d, pos);
            size_t n = size_t(1) << (im & 0xf);
            uint64_t v = be(d, pos + 1, n);
            pos += 1 + n;
            return v;
        };
        Plist p;
        switch (hi)
        {
        case 0x0:
            if (marker == 0x08 || marker == 0x09)
            {
                p.kind = Plist::Kind::Bool;
                p.b = marker == 0x09;
            }
            break;
        case 0x1: {
            size_t n = size_t(1) << lo;
            p.kind = Plist::Kind::Int;
            p.i = int64_t(be(d, pos + (n > 8 ? n - 8 : 0), n > 8 ? 8 : n));
            break;
        }
        case 0x2: {
            p.kind = Plist::Kind::Real;
            if (lo == 2)
            {
                uint32_t bits = uint32_t(be(d, pos, 4));
                float f;
                std::memcpy(&f, &bits, 4);
                p.r = f;
            }
            else
            {
                uint64_t bits = be(d, pos, 8);
                std::memcpy(&p.r, &bits, 8);
            }
            break;
        }
        case 0x3: {
            p.kind = Plist::Kind::Date;
            uint64_t bits = be(d, pos, 8);
            std::memcpy(&p.r, &bits, 8);
            break;
        }
        case 0x4: {
            uint64_t n = length();
            if (pos + n > d.size()) throw std::runtime_error("plist: truncated data");
            p.kind = Plist::Kind::Data;
            p.data.assign(d.begin() + pos, d.begin() + pos + n);
            break;
        }
        case 0x5: {
            uint64_t n = length();
            p.kind = Plist::Kind::String;
            for (uint64_t i = 0; i < n; ++i)
                append_utf8(p.s, byte_at(d, pos + i));
            break;
        }
        case 0x6: {
            uint64_t n = length();
            p.kind = Plist::Kind::String;
            for (uint64_t i = 0; i < n; ++i)
            {
                uint32_t u = uint32_t(be(d, pos + i * 2, 2));
                if (u >= 0xd800 && u < 0xdc00 && i + 1 < n)
                {
                    uint32_t low = uint32_t(be(d, pos + (i + 1) * 2, 2));
                    u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
                    ++i;
                }
                append_utf8(p.s, u);
            }
            break;
        }
        case 0x8:
            p.kind = Plist::Kind::Int;
            p.i = int64_t(be(d, pos, size_t(lo) + 1));
            break;
        case 0xa:
        case 0xc: {
            uint64_t n = length();
            p.kind = Plist::Kind::Array;
            for (uint64_t i = 0; i < n; ++i)
                p.array.push_back(object(be(d, pos + i * ref_size, ref_size), depth + 1));
            break;
        }
        case 0xd: {
            uint64_t n = length();
            p.kind = Plist::Kind::Dict;
            for (uint64_t i = 0; i < n; ++i)
            {
                Plist k = object(be(d, pos + i * ref_size, ref_size), depth + 1);
                Plist v = object(be(d, pos + (n + i) * ref_size, ref_size), depth + 1);
                p.dict.emplace_back(k.s, std::move(v));
            }
            break;
        }
        default: break;
        }
        return p;
    };
    return object(top, 0);
}

class XmlReader
{
public:
    explicit XmlReader(std::string_view text) : t_(text) {}

    Plist parse()
    {
        for (;;)
        {
            std::string tag = next_tag();
            if (tag.empty()) throw std::runtime_error("plist: no root");
            if (tag == "plist") return value(next_tag());
        }
    }

private:
    std::string next_tag()
    {
        for (;;)
        {
            size_t lt = t_.find('<', i_);
            if (lt == std::string_view::npos) return {};
            size_t gt = t_.find('>', lt);
            if (gt == std::string_view::npos) return {};
            i_ = gt + 1;
            std::string_view tag = t_.substr(lt + 1, gt - lt - 1);
            if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
            size_t sp = tag.find_first_of(" \t\r\n/");
            std::string name(tag.substr(0, sp == 0 ? tag.find_first_of(" \t\r\n", 1) : sp));
            if (tag.back() == '/' && name.front() != '/') name += '/';
            return name;
        }
    }

    std::string text_until_close()
    {
        size_t lt = t_.find('<', i_);
        std::string raw(t_.substr(i_, lt - i_));
        i_ = t_.find('>', lt) + 1;
        std::string out;
        for (size_t k = 0; k < raw.size(); ++k)
        {
            if (raw[k] != '&')
            {
                out += raw[k];
                continue;
            }
            size_t semi = raw.find(';', k);
            std::string ent = raw.substr(k + 1, semi - k - 1);
            if (ent == "lt")
                out += '<';
            else if (ent == "gt")
                out += '>';
            else if (ent == "amp")
                out += '&';
            else if (ent == "quot")
                out += '"';
            else if (ent == "apos")
                out += '\'';
            else if (ent.size() > 1 && ent[0] == '#')
                append_utf8(out, uint32_t(std::stoul(ent.substr(ent[1] == 'x' ? 2 : 1), nullptr, ent[1] == 'x' ? 16 : 10)));
            k = semi;
        }
        return out;
    }

    Plist value(const std::string& tag)
    {
        Plist p;
        if (tag == "dict")
        {
            p.kind = Plist::Kind::Dict;
            for (std::string t = next_tag(); t != "/dict" && !t.empty(); t = next_tag())
            {
                if (t != "key") throw std::runtime_error("plist: expected key");
                std::string key = text_until_close();
                p.dict.emplace_back(key, value(next_tag()));
            }
        }
        else if (tag == "array")
        {
            p.kind = Plist::Kind::Array;
            for (std::string t = next_tag(); t != "/array" && !t.empty(); t = next_tag())
                p.array.push_back(value(t));
        }
        else if (tag == "dict/")
        {
            p.kind = Plist::Kind::Dict;
        }
        else if (tag == "array/")
        {
            p.kind = Plist::Kind::Array;
        }
        else if (tag == "string")
        {
            p.kind = Plist::Kind::String;
            p.s = text_until_close();
        }
        else if (tag == "string/")
        {
            p.kind = Plist::Kind::String;
        }
        else if (tag == "integer")
        {
            p.kind = Plist::Kind::Int;
            p.i = std::stoll(text_until_close());
        }
        else if (tag == "real")
        {
            p.kind = Plist::Kind::Real;
            p.r = std::stod(text_until_close());
        }
        else if (tag == "true/" || tag == "false/")
        {
            p.kind = Plist::Kind::Bool;
            p.b = tag == "true/";
        }
        else if (tag == "date")
        {
            p.kind = Plist::Kind::Date;
            text_until_close();
        }
        else if (tag == "data")
        {
            p.kind = Plist::Kind::Data;
            std::string b64 = text_until_close();
            static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            uint32_t acc = 0;
            int bits = 0;
            for (char ch : b64)
            {
                size_t v = chars.find(ch);
                if (v == std::string::npos) continue;
                acc = (acc << 6) | uint32_t(v);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    p.data.push_back(uint8_t(acc >> bits));
                }
            }
        }
        return p;
    }

    std::string_view t_;
    size_t i_ = 0;
};

std::string xml_escape(const std::string& s)
{
    std::string o;
    for (char ch : s)
    {
        if (ch == '<')
            o += "&lt;";
        else if (ch == '>')
            o += "&gt;";
        else if (ch == '&')
            o += "&amp;";
        else
            o += ch;
    }
    return o;
}

void write_value(std::string& out, const Plist& p, int indent)
{
    std::string pad(indent, '\t');
    char buf[64];
    switch (p.kind)
    {
    case Plist::Kind::Bool: out += pad + (p.b ? "<true/>\n" : "<false/>\n"); break;
    case Plist::Kind::Int: out += pad + "<integer>" + std::to_string(p.i) + "</integer>\n"; break;
    case Plist::Kind::Real:
    case Plist::Kind::Date:
        std::snprintf(buf, sizeof buf, "%.17g", p.r);
        out += pad + "<real>" + buf + "</real>\n";
        break;
    case Plist::Kind::String: out += pad + "<string>" + xml_escape(p.s) + "</string>\n"; break;
    case Plist::Kind::Data: {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (size_t i = 0; i < p.data.size(); i += 3)
        {
            uint32_t v = uint32_t(p.data[i]) << 16;
            if (i + 1 < p.data.size()) v |= uint32_t(p.data[i + 1]) << 8;
            if (i + 2 < p.data.size()) v |= p.data[i + 2];
            b64 += chars[(v >> 18) & 63];
            b64 += chars[(v >> 12) & 63];
            b64 += i + 1 < p.data.size() ? chars[(v >> 6) & 63] : '=';
            b64 += i + 2 < p.data.size() ? chars[v & 63] : '=';
        }
        out += pad + "<data>" + b64 + "</data>\n";
        break;
    }
    case Plist::Kind::Array:
        out += pad + "<array>\n";
        for (auto& v : p.array)
            write_value(out, v, indent + 1);
        out += pad + "</array>\n";
        break;
    case Plist::Kind::Dict:
        out += pad + "<dict>\n";
        for (auto& [k, v] : p.dict)
        {
            out += pad + "\t<key>" + xml_escape(k) + "</key>\n";
            write_value(out, v, indent + 1);
        }
        out += pad + "</dict>\n";
        break;
    default: break;
    }
}

}

std::optional<Plist> parse_plist(std::span<const uint8_t> bytes)
{
    try
    {
        if (bytes.size() >= 8 && std::memcmp(bytes.data(), "bplist00", 8) == 0) return parse_binary(bytes);
        return XmlReader(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).parse();
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::string write_xml_plist(const Plist& root)
{
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n";
    write_value(out, root, 0);
    return out + "</plist>\n";
}

}
