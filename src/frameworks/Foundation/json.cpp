#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::objc;

namespace
{
constexpr uint64_t kMutableContainers = 1, kMutableLeaves = 2, kFragmentsAllowed = 4;
constexpr uint64_t kPrettyPrinted = 1, kSortedKeys = 2, kWithoutEscapingSlashes = 8;

class Reader
{
public:
    Reader(Cpu& c, const uint8_t* p, size_t n, uint64_t options) : c_(c), p_(p), end_(p + n), options_(options)
    {
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p_ += 3;
    }

    bool document(Id& out)
    {
        skip();
        if (!value(out, 0)) return false;
        skip();
        if (p_ != end_) return false;
        return true;
    }

private:
    void skip()
    {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
            ++p_;
    }

    bool literal(const char* word)
    {
        size_t n = std::strlen(word);
        if (size_t(end_ - p_) < n || std::memcmp(p_, word, n) != 0) return false;
        p_ += n;
        return true;
    }

    bool value(Id& out, int depth)
    {
        if (depth > 512 || p_ >= end_) return false;
        bool mc = options_ & kMutableContainers;
        switch (*p_)
        {
        case '{': {
            ++p_;
            std::vector<std::pair<Id, Id>> entries;
            skip();
            if (p_ < end_ && *p_ == '}')
            {
                ++p_;
                out = make_dict(c_, entries, mc);
                return true;
            }
            for (;;)
            {
                skip();
                Id key, v;
                if (p_ >= end_ || *p_ != '"' || !string(key)) return false;
                skip();
                if (p_ >= end_ || *p_++ != ':') return false;
                skip();
                if (!value(v, depth + 1)) return false;
                std::u16string ks = to_utf16(c_, key);
                auto dup = std::find_if(entries.begin(), entries.end(), [&](auto& e) { return to_utf16(c_, e.first) == ks; });
                if (dup != entries.end())
                    dup->second = v;
                else
                    entries.emplace_back(key, v);
                skip();
                if (p_ < end_ && *p_ == ',')
                {
                    ++p_;
                    continue;
                }
                if (p_ < end_ && *p_ == '}')
                {
                    ++p_;
                    break;
                }
                return false;
            }
            out = make_dict(c_, entries, mc);
            return true;
        }
        case '[': {
            ++p_;
            std::vector<Id> items;
            skip();
            if (p_ < end_ && *p_ == ']')
            {
                ++p_;
                out = make_array(c_, items, mc);
                return true;
            }
            for (;;)
            {
                skip();
                Id v;
                if (!value(v, depth + 1)) return false;
                items.push_back(v);
                skip();
                if (p_ < end_ && *p_ == ',')
                {
                    ++p_;
                    continue;
                }
                if (p_ < end_ && *p_ == ']')
                {
                    ++p_;
                    break;
                }
                return false;
            }
            out = make_array(c_, items, mc);
            return true;
        }
        case '"': return string(out);
        case 't':
            if (!literal("true")) return false;
            out = boolean(true);
            return true;
        case 'f':
            if (!literal("false")) return false;
            out = boolean(false);
            return true;
        case 'n':
            if (!literal("null")) return false;
            out = null_object(c_);
            return true;
        default: return number(out);
        }
    }

    Id boolean(bool b)
    {
        NumberData n;
        n.type = 'B';
        n.i = b;
        return make_number(c_, n);
    }

    bool number(Id& out)
    {
        const uint8_t* start = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        if (p_ >= end_ || !std::isdigit(*p_)) return false;
        bool real = false;
        while (p_ < end_ && (std::isdigit(*p_) || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-'))
        {
            if (*p_ == '.' || *p_ == 'e' || *p_ == 'E') real = true;
            ++p_;
        }
        std::string text(start, p_);
        NumberData n;
        if (!real)
        {
            errno = 0;
            long long v = std::strtoll(text.c_str(), nullptr, 10);
            if (errno == 0)
            {
                n.type = 'q';
                n.i = v;
                out = make_number(c_, n);
                return true;
            }
            errno = 0;
            unsigned long long u = std::strtoull(text.c_str(), nullptr, 10);
            if (errno == 0 && text[0] != '-')
            {
                n.type = 'Q';
                n.u = u;
                out = make_number(c_, n);
                return true;
            }
        }
        n.type = 'd';
        n.d = std::strtod(text.c_str(), nullptr);
        out = make_number(c_, n);
        return true;
    }

    static int hex(uint8_t ch)
    {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    bool string(Id& out)
    {
        ++p_;
        std::string utf8;
        std::u16string pending;
        auto flush = [&] {
            if (!pending.empty())
            {
                utf8 += utf16_to_8(pending);
                pending.clear();
            }
        };
        while (p_ < end_ && *p_ != '"')
        {
            uint8_t ch = *p_++;
            if (ch < 0x20) return false;
            if (ch != '\\')
            {
                flush();
                utf8 += char(ch);
                continue;
            }
            if (p_ >= end_) return false;
            uint8_t e = *p_++;
            if (e == 'u')
            {
                if (end_ - p_ < 4) return false;
                int v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    int h = hex(p_[i]);
                    if (h < 0) return false;
                    v = v * 16 + h;
                }
                p_ += 4;
                pending += char16_t(v);
                continue;
            }
            flush();
            switch (e)
            {
            case '"': utf8 += '"'; break;
            case '\\': utf8 += '\\'; break;
            case '/': utf8 += '/'; break;
            case 'b': utf8 += '\b'; break;
            case 'f': utf8 += '\f'; break;
            case 'n': utf8 += '\n'; break;
            case 'r': utf8 += '\r'; break;
            case 't': utf8 += '\t'; break;
            default: return false;
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        flush();
        out = string_autoreleased(c_, utf8);
        if (options_ & kMutableLeaves) out = objc(c_).autorelease(c_, objc(c_).send(c_, out, "mutableCopy"));
        return true;
    }

    Cpu& c_;
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t options_;
};

class Writer
{
public:
    Writer(Cpu& c, uint64_t options) : c_(c), options_(options) {}

    bool value(Id obj, int depth)
    {
        if (depth > 512 || !obj) return false;
        if (obj == null_object(c_))
        {
            out += "null";
            return true;
        }
        if (is_string(c_, obj))
        {
            string(to_utf8(c_, obj));
            return true;
        }
        NumberData n;
        if (number_of(c_, obj, n))
        {
            if (n.type == 'B')
            {
                out += n.i ? "true" : "false";
            }
            else if (n.is_float())
            {
                if (!std::isfinite(n.d)) return false;
                out += shortest(n.d);
            }
            else if (n.is_unsigned())
            {
                out += std::to_string(n.u);
            }
            else
            {
                out += std::to_string(n.i);
            }
            return true;
        }
        objc::Class* k = objc(c_).class_of(obj);
        if (objc(c_).is_subclass(k, objc(c_).class_named("NSDictionary")))
        {
            auto entries = dict_entries(c_, obj);
            std::vector<std::pair<std::string, Id>> kv;
            for (auto& [key, v] : entries)
            {
                if (!is_string(c_, key)) return false;
                kv.emplace_back(to_utf8(c_, key), v);
            }
            if (options_ & kSortedKeys) std::sort(kv.begin(), kv.end(), [](auto& a, auto& b) { return a.first < b.first; });
            out += '{';
            for (size_t i = 0; i < kv.size(); ++i)
            {
                if (i) out += ',';
                newline(depth + 1);
                string(kv[i].first);
                out += (options_ & kPrettyPrinted) ? " : " : ":";
                if (!value(kv[i].second, depth + 1)) return false;
            }
            if (!kv.empty()) newline(depth);
            out += '}';
            return true;
        }
        if (objc(c_).is_subclass(k, objc(c_).class_named("NSArray")))
        {
            auto items = array_items(c_, obj);
            out += '[';
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (i) out += ',';
                newline(depth + 1);
                if (!value(items[i], depth + 1)) return false;
            }
            if (!items.empty()) newline(depth);
            out += ']';
            return true;
        }
        return false;
    }

    std::string out;

private:
    void newline(int depth)
    {
        if (!(options_ & kPrettyPrinted)) return;
        out += '\n';
        out.append(size_t(depth) * 2, ' ');
    }

    static std::string shortest(double d)
    {
        if (d == std::floor(d) && std::fabs(d) < 1e15) return std::to_string(int64_t(d));
        char buf[40];
        for (int prec = 1; prec <= 17; ++prec)
        {
            std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
            if (std::strtod(buf, nullptr) == d) break;
        }
        return buf;
    }

    void string(const std::string& s)
    {
        out += '"';
        for (unsigned char ch : s)
        {
            switch (ch)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '/': out += (options_ & kWithoutEscapingSlashes) ? "/" : "\\/"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (ch < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                }
                else
                {
                    out += char(ch);
                }
            }
        }
        out += '"';
    }

    Cpu& c_;
    uint64_t options_;
};

bool bytes_of(Cpu& c, Id data, std::vector<uint8_t>& out)
{
    if (!data) return false;
    uint64_t n = objc(c).send(c, data, "length");
    GuestAddr p = objc(c).send(c, data, "bytes");
    out.resize(n);
    if (n) c.mem.read_bytes(p, out.data(), n);
    return true;
}

void clear_error(Cpu& c, GuestAddr error_out)
{
    if (error_out) c.mem.write<uint64_t>(error_out, 0);
}

}

void register_json(objc::ObjcRuntime& o)
{
    o.define("NSJSONSerialization", "NSObject");
    o.class_method("NSJSONSerialization", "JSONObjectWithData:options:error:", [](Cpu& c) {
        clear_error(c, c.arg(4));
        std::vector<uint8_t> bytes;
        if (!bytes_of(c, c.arg(2), bytes)) return c.ret(0);
        Reader r(c, bytes.data(), bytes.size(), c.arg(3));
        Id out = 0;
        if (!r.document(out)) return c.ret(0);
        objc::Class* k = objc(c).class_of(out);
        bool container =
            objc(c).is_subclass(k, objc(c).class_named("NSDictionary")) || objc(c).is_subclass(k, objc(c).class_named("NSArray"));
        if (!container && !(c.arg(3) & kFragmentsAllowed)) return c.ret(0);
        c.ret(out);
    });
    o.class_method("NSJSONSerialization", "dataWithJSONObject:options:error:", [](Cpu& c) {
        clear_error(c, c.arg(4));
        Writer w(c, c.arg(3));
        if (!w.value(c.arg(2), 0)) return c.ret(0);
        c.ret(make_data(c, std::vector<uint8_t>(w.out.begin(), w.out.end())));
    });
    o.class_method("NSJSONSerialization", "isValidJSONObject:", [](Cpu& c) {
        objc::Class* k = objc(c).class_of(c.arg(2));
        bool container =
            objc(c).is_subclass(k, objc(c).class_named("NSDictionary")) || objc(c).is_subclass(k, objc(c).class_named("NSArray"));
        Writer w(c, 0);
        c.ret(container && w.value(c.arg(2), 0));
    });
}

}
