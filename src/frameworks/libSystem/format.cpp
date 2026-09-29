#include "frameworks/libSystem/format.h"

#include <cstdio>
#include <cstring>

#include "cpu/cpu.h"

namespace orchard
{
namespace
{
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

}

std::string guest_format(Cpu& cpu, const std::string& fmt, VarArgs args, const std::function<std::string(uint64_t)>& object)
{
    std::string out;
    size_t i = 0;
    while (i < fmt.size())
    {
        char ch = fmt[i];
        if (ch != '%')
        {
            out += ch;
            ++i;
            continue;
        }
        size_t start = i++;
        if (i < fmt.size() && fmt[i] == '%')
        {
            out += '%';
            ++i;
            continue;
        }
        size_t digits = i;
        while (digits < fmt.size() && std::isdigit(uint8_t(fmt[digits])))
            ++digits;
        if (digits > i && digits < fmt.size() && fmt[digits] == '$') i = digits + 1;

        std::string spec = "%";
        while (i < fmt.size() && std::strchr("-+ #0'", fmt[i]))
            spec += fmt[i++];
        auto number = [&] {
            if (i < fmt.size() && fmt[i] == '*')
            {
                spec += std::to_string(int32_t(args.u64()));
                ++i;
            }
            else
            {
                while (i < fmt.size() && std::isdigit(uint8_t(fmt[i])))
                    spec += fmt[i++];
            }
        };
        number();
        if (i < fmt.size() && fmt[i] == '.')
        {
            spec += fmt[i++];
            number();
        }
        std::string length;
        while (i < fmt.size() && std::strchr("hlLqjztv", fmt[i]))
            length += fmt[i++];
        if (i >= fmt.size()) break;
        char conv = fmt[i++];

        char buf[512];
        switch (conv)
        {
        case 'd':
        case 'i': {
            int64_t v = int64_t(args.u64());
            if (length.empty())
                v = int32_t(v);
            else if (length == "h")
                v = int16_t(v);
            else if (length == "hh")
                v = int8_t(v);
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)v);
            out += buf;
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v = args.u64();
            if (length.empty())
                v = uint32_t(v);
            else if (length == "h")
                v = uint16_t(v);
            else if (length == "hh")
                v = uint8_t(v);
            std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)v);
            out += buf;
            break;
        }
        case 'D':
        case 'U':
        case 'O': {
            std::snprintf(buf, sizeof buf, (spec + "ll" + char(std::tolower(conv))).c_str(), (long long)args.u64());
            out += buf;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), args.f64());
            out += buf;
            break;
        }
        case 'c': out += char(args.u64()); break;
        case 'C': append_utf8(out, uint32_t(args.u64() & 0xffff)); break;
        case 's': {
            GuestAddr p = args.u64();
            std::string s = p ? cpu.mem.read_cstr(p) : "(null)";
            std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += s.size() >= sizeof buf - 1 ? s : std::string(buf);
            break;
        }
        case 'S': {
            GuestAddr p = args.u64();
            if (!p)
            {
                out += "(null)";
                break;
            }
            for (uint16_t u; (u = cpu.mem.read<uint16_t>(p)); p += 2)
                append_utf8(out, u);
            break;
        }
        case 'p': {
            std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)args.u64());
            out += buf;
            break;
        }
        case '@': {
            uint64_t obj = args.u64();
            if (object)
                out += object(obj);
            else
            {
                std::snprintf(buf, sizeof buf, "<0x%llx>", (unsigned long long)obj);
                out += buf;
            }
            break;
        }
        case 'n': args.u64(); break;
        default: out += fmt.substr(start, i - start); break;
        }
    }
    return out;
}

}
