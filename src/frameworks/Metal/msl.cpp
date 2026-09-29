#include "frameworks/Metal/msl.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>

namespace orchard::metal::msl
{
namespace
{
std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

size_t match(const std::string& s, size_t open)
{
    char o = s[open], c = o == '(' ? ')' : o == '{' ? '}' : o == '[' ? ']' : '>';
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == o)
            ++depth;
        else if (s[i] == c && --depth == 0)
            return i + 1;
    }
    return std::string::npos;
}

std::vector<std::string> split_top(const std::string& s, char delim)
{
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char ch : s)
    {
        if (ch == '(' || ch == '[' || ch == '<' || ch == '{') ++depth;
        if (ch == ')' || ch == ']' || ch == '>' || ch == '}') --depth;
        if (ch == delim && depth == 0)
        {
            out.push_back(trim(cur));
            cur.clear();
        }
        else
        {
            cur += ch;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to)
{
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
        s.replace(p, from.size(), to);
    return s;
}

std::string regex_all(const std::string& s, const std::string& re, const std::string& fmt)
{
    return std::regex_replace(s, std::regex(re), fmt);
}

std::string hlsl_type(std::string t)
{
    t = trim(t);
    t = regex_all(t, R"(\bpacked_(\w+))", "$1");
    t = regex_all(t, R"(\bhalf([234]x[234]|[234])?\b)", "float$1");
    t = regex_all(t, R"(\bushort([234])?\b)", "uint$1");
    t = regex_all(t, R"(\bshort([234])?\b)", "int$1");
    t = regex_all(t, R"(\buchar([234])?\b)", "uint$1");
    t = regex_all(t, R"(\bchar([234])?\b)", "int$1");
    t = regex_all(t, R"(\buint32_t\b)", "uint");
    t = regex_all(t, R"(\bint32_t\b)", "int");
    return t;
}

struct TypeInfo
{
    std::string base;
    int cols = 1;
    int rows = 0;
    bool packed = false;
};

TypeInfo type_info(std::string t)
{
    TypeInfo ti;
    t = trim(t);
    if (t.starts_with("packed_"))
    {
        ti.packed = true;
        t = t.substr(7);
    }
    std::smatch m;
    if (std::regex_match(t, m, std::regex(R"((float|half|int|uint|bool|short|ushort|char|uchar)(\d)?(?:x(\d))?)")))
    {
        ti.base = m[1];
        if (m[2].matched) ti.cols = std::stoi(m[2]);
        if (m[3].matched) ti.rows = std::stoi(m[3]);
    }
    else if (t == "uint32_t")
    {
        ti.base = "uint";
    }
    else if (t == "int32_t")
    {
        ti.base = "int";
    }
    return ti;
}

int scalar_size(const std::string& base)
{
    if (base == "half" || base == "short" || base == "ushort") return 2;
    if (base == "bool" || base == "char" || base == "uchar") return 1;
    return 4;
}

void size_align(const TypeInfo& t, uint32_t& size, uint32_t& align)
{
    uint32_t s = uint32_t(scalar_size(t.base));
    auto vec = [&](int n, uint32_t& sz, uint32_t& al) {
        if (t.packed)
        {
            sz = s * uint32_t(n);
            al = s;
        }
        else
        {
            uint32_t k = n == 3 ? 4 : uint32_t(n);
            sz = s * k;
            al = n == 1 ? s : s * k;
        }
    };
    if (t.rows)
    {
        uint32_t csz, cal;
        vec(t.rows, csz, cal);
        size = csz * uint32_t(t.cols);
        align = cal;
    }
    else
    {
        vec(t.cols, size, align);
    }
}

std::string load_expr(const TypeInfo& t, const std::string& buf, const std::string& off)
{
    int n = t.cols;
    std::string suffix = n == 1 ? "" : std::to_string(n);
    if (t.base == "float") return "asfloat(" + buf + ".Load" + suffix + "(" + off + "))";
    if (t.base == "int") return "asint(" + buf + ".Load" + suffix + "(" + off + "))";
    if (t.base == "uint") return buf + ".Load" + suffix + "(" + off + ")";
    if (t.base == "half")
    {
        std::string w0 = buf + ".Load((" + off + ") & ~3u)", sh = "(((" + off + ") & 2u) * 8u)";
        if (n == 1) return "f16tof32(" + w0 + " >> " + sh + ")";
        std::string w1 = buf + ".Load(((" + off + ") & ~3u) + 4u)";
        std::string a = "f16tof32(" + w0 + ")", b = "f16tof32(" + w0 + " >> 16)", c = "f16tof32(" + w1 + ")",
                    d = "f16tof32(" + w1 + " >> 16)";
        if (n == 2) return "float2(" + a + ", " + b + ")";
        if (n == 3) return "float3(" + a + ", " + b + ", " + c + ")";
        return "float4(" + a + ", " + b + ", " + c + ", " + d + ")";
    }
    if (t.base == "bool") return "(((" + buf + ".Load((" + off + ") & ~3u) >> (((" + off + ") & 3u) * 8u)) & 0xFFu) != 0)";
    return buf + ".Load" + suffix + "(" + off + ")";
}

struct MslField
{
    std::string type, name, attr;
    int array = 0;
};

struct MslStruct
{
    std::string name;
    std::vector<MslField> fields;
    size_t begin = 0, end = 0;
};

std::vector<MslField> parse_fields(const std::string& body)
{
    std::vector<MslField> out;
    for (auto& line : split_top(body, ';'))
    {
        std::string l = trim(line);
        if (l.empty()) continue;
        MslField f;
        size_t attr = l.find("[[");
        if (attr != std::string::npos)
        {
            size_t close = l.find("]]", attr);
            f.attr = trim(l.substr(attr + 2, close - attr - 2));
            l = trim(l.substr(0, attr));
        }
        std::smatch m;
        if (!std::regex_match(l, m, std::regex(R"(([\w:<>, ]+?)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?)"))) continue;
        f.type = trim(m[1]);
        f.name = m[2];
        if (m[3].matched) f.array = std::stoi(m[3]);
        out.push_back(f);
    }
    return out;
}

std::string attr_arg(const std::string& attr, const std::string& key)
{
    std::smatch m;
    if (std::regex_search(attr, m, std::regex(key + R"(\s*\(\s*([\w]+)\s*\))"))) return m[1];
    return {};
}

bool has_attr(const std::string& attr, const std::string& key)
{
    return std::regex_search(attr, std::regex("(^|[ ,])" + key + "([ ,(]|$)"));
}

struct TextureDecl
{
    std::string kind;
    bool depth = false;
};

std::string rewrite_texture_calls(const std::string& body, const std::map<std::string, TextureDecl>& textures,
                                  std::set<std::string>& compare_samplers, bool vertex)
{
    std::string out;
    size_t i = 0;
    std::regex call(R"((\w+)\.(sample_compare|sample|read|get_width|get_height|get_depth|get_num_mip_levels|get_array_size|gather)\s*\()");
    std::smatch m;
    std::string rest = body;
    while (std::regex_search(rest, m, call))
    {
        std::string name = m[1], method = m[2];
        size_t open = size_t(m.position(0) + m.length(0) - 1);
        size_t close = match(rest, open);
        auto it = textures.find(name);
        if (it == textures.end() || close == std::string::npos)
        {
            out += rest.substr(0, open + 1);
            rest = rest.substr(open + 1);
            continue;
        }
        const TextureDecl& td = it->second;
        auto args = split_top(rest.substr(open + 1, close - open - 2), ',');
        std::string rep;
        bool array = td.kind.find("array") != std::string::npos;
        auto coord_with_slice = [&](const std::vector<std::string>& a, size_t& next) {
            std::string c = a.size() > 1 ? a[1] : "0";
            next = 2;
            if (array && a.size() > 2)
            {
                std::string dims = td.kind == "cube_array" ? "float4" : "float3";
                c = dims + "(" + c + ", " + a[2] + ")";
                next = 3;
            }
            return c;
        };
        if (method == "sample" || method == "gather")
        {
            size_t next;
            std::string s = args.empty() ? "" : args[0];
            std::string c = coord_with_slice(args, next);
            std::string level, bias, grad, offset;
            for (size_t k = next; k < args.size(); ++k)
            {
                std::string a = trim(args[k]);
                if (a.starts_with("level("))
                    level = a.substr(6, a.size() - 7);
                else if (a.starts_with("bias("))
                    bias = a.substr(5, a.size() - 6);
                else if (a.starts_with("gradient2d(") || a.starts_with("gradient3d(") || a.starts_with("gradientcube("))
                    grad = a.substr(a.find('(') + 1, a.size() - a.find('(') - 2);
                else if (a.starts_with("min_lod_clamp("))
                    continue;
                else if (a.starts_with("component::"))
                    continue;
                else
                    offset = a;
            }
            std::string tail = offset.empty() ? "" : ", " + offset;
            if (method == "gather")
                rep = name + ".Gather(" + s + ", " + c + tail + ")";
            else if (!level.empty() || vertex)
                rep = name + ".SampleLevel(" + s + ", " + c + ", " + (level.empty() ? "0" : level) + tail + ")";
            else if (!bias.empty())
                rep = name + ".SampleBias(" + s + ", " + c + ", " + bias + tail + ")";
            else if (!grad.empty())
                rep = name + ".SampleGrad(" + s + ", " + c + ", " + grad + tail + ")";
            else
                rep = name + ".Sample(" + s + ", " + c + tail + ")";
        }
        else if (method == "sample_compare")
        {
            size_t next;
            std::string s = args.empty() ? "" : args[0];
            compare_samplers.insert(s);
            std::string c = coord_with_slice(args, next);
            std::string ref = next < args.size() ? args[next] : "0";
            bool level0 = vertex;
            for (size_t k = next + 1; k < args.size(); ++k)
                if (trim(args[k]).starts_with("level(")) level0 = true;
            rep = name + (level0 ? ".SampleCmpLevelZero(" : ".SampleCmp(") + s + ", " + c + ", " + ref + ")";
        }
        else if (method == "read")
        {
            std::string c = args.empty() ? "0" : args[0];
            std::string lod = "0";
            if (array)
            {
                std::string slice = args.size() > 1 ? args[1] : "0";
                if (args.size() > 2) lod = args[2];
                rep = name + ".Load(int4(int2(" + c + "), int(" + slice + "), int(" + lod + ")))";
            }
            else if (td.kind == "3d")
            {
                if (args.size() > 1) lod = args[1];
                rep = name + ".Load(int4(int3(" + c + "), int(" + lod + ")))";
            }
            else
            {
                if (args.size() > 1) lod = args[1];
                rep = name + ".Load(int3(int2(" + c + "), int(" + lod + ")))";
            }
        }
        else
        {
            std::string lod = args.empty() ? "0" : args[0];
            std::string what = method == "get_width" ? "x" : method == "get_height" ? "y" : method == "get_depth" ? "z" : "w";
            if (method == "get_num_mip_levels") what = "w";
            rep = "orchard_dims(" + name + ", " + lod + ")." + what;
        }
        out += rest.substr(0, size_t(m.position(0))) + rep;
        rest = rest.substr(close);
    }
    (void)i;
    return out + rest;
}

std::string splat_constructors(const std::string& s)
{
    static const std::regex ctor(R"(\b(float|int|uint|bool)([234])\s*\()");
    std::string out, rest = s;
    std::smatch m;
    while (std::regex_search(rest, m, ctor))
    {
        size_t open = size_t(m.position(0) + m.length(0) - 1);
        size_t close = match(rest, open);
        if (close == std::string::npos) break;
        std::string inner = splat_constructors(rest.substr(open + 1, close - open - 2));
        std::string type = m[1].str() + m[2].str();
        out += rest.substr(0, size_t(m.position(0)));
        out += split_top(inner, ',').size() == 1 ? "((" + type + ")(" + inner + "))" : type + "(" + inner + ")";
        rest = rest.substr(close);
    }
    return out + rest;
}

std::string translate_expressions(std::string s)
{
    s = regex_all(s, R"(\bfma\s*\()", "mad(");
    s = regex_all(s, R"(\bfract\s*\()", "frac(");
    s = regex_all(s, R"(\bmix\s*\()", "lerp(");
    s = regex_all(s, R"(\bdfdx\s*\()", "ddx(");
    s = regex_all(s, R"(\bdfdy\s*\()", "ddy(");
    s = regex_all(s, R"(\brint\s*\()", "round(");
    s = regex_all(s, R"(\bdiscard_fragment\s*\(\s*\))", "discard");
    s = regex_all(s, R"(\b(precise|fast|metal)::)", "");
    s = regex_all(s, R"(\bas_type\s*<\s*(float|half)[234]?\s*>\s*\()", "asfloat(");
    s = regex_all(s, R"(\bas_type\s*<\s*(uint|ushort)[234]?\s*>\s*\()", "asuint(");
    s = regex_all(s, R"(\bas_type\s*<\s*(int|short)[234]?\s*>\s*\()", "asint(");
    s = regex_all(s, R"(\b(\d+\.\d*|\d*\.\d+|\d+)h\b)", "$1");
    s = regex_all(s, R"(\bthread\s+)", "");
    s = regex_all(s, R"(\bdevice\s+)", "");
    s = regex_all(s, R"(\bhalf([234]x[234]|[234])?\b)", "float$1");
    s = regex_all(s, R"(\bushort([234])?\b)", "uint$1");
    s = regex_all(s, R"(\bshort([234])?\b)", "int$1");
    s = regex_all(s, R"(\buint32_t\b)", "uint");
    s = regex_all(s, R"(\bint32_t\b)", "int");
    s = regex_all(s, R"(\bselect\s*\(([^,]+),([^,]+),([^)]+)\))", "(($3) ? ($2) : ($1))");
    return splat_constructors(s);
}

std::string expand_templates(const std::string& src)
{
    std::string out;
    std::regex tmpl(R"(template\s*<\s*typename\s+(\w+)\s*>)");
    std::smatch m;
    std::string rest = src;
    while (std::regex_search(rest, m, tmpl))
    {
        out += rest.substr(0, size_t(m.position(0)));
        std::string param = m[1];
        size_t start = size_t(m.position(0) + m.length(0));
        size_t brace = rest.find('{', start);
        size_t end = brace == std::string::npos ? std::string::npos : match(rest, brace);
        if (end == std::string::npos)
        {
            out += rest.substr(size_t(m.position(0)));
            return out;
        }
        std::string fn = rest.substr(start, end - start);
        for (const char* t : {"uint", "uint2", "uint3", "uint4", "int", "int2", "int3", "int4"})
            out += "\n" + regex_all(fn, "\\b" + param + "\\b", t) + "\n";
        rest = rest.substr(end);
        if (!rest.empty() && rest[0] == ';') rest = rest.substr(1);
    }
    return out + rest;
}

std::string semantic_for(const std::string& attr, bool vertex_input, bool fragment_output, int& target_out,
                         const std::map<std::string, uint64_t>& consts)
{
    if (vertex_input)
    {
        std::string n = attr_arg(attr, "attribute");
        return n.empty() ? "" : "ATTRIB" + n;
    }
    if (has_attr(attr, "position")) return "SV_Position";
    if (fragment_output)
    {
        std::string n = attr_arg(attr, "color");
        if (!n.empty())
        {
            int idx = std::isdigit(uint8_t(n[0])) ? std::stoi(n) : int(consts.count(n) ? consts.at(n) : 0);
            target_out = idx;
            return "SV_Target" + std::to_string(idx);
        }
        if (has_attr(attr, "depth")) return "SV_Depth";
        if (has_attr(attr, "sample_mask")) return "SV_Coverage";
    }
    std::string user = attr_arg(attr, "user");
    if (!user.empty()) return user;
    if (has_attr(attr, "clip_distance")) return "SV_ClipDistance";
    return "";
}

std::string strip_entry_functions(std::string text)
{
    std::regex re(R"(\b(vertex|fragment|kernel)\s+[\w:]+\s+\w+\s*\()");
    std::smatch m;
    std::string out;
    while (std::regex_search(text, m, re))
    {
        size_t start = size_t(m.position(0));
        size_t close = match(text, start + size_t(m.length(0)) - 1);
        size_t brace = close == std::string::npos ? std::string::npos : text.find('{', close);
        size_t end = brace == std::string::npos ? std::string::npos : match(text, brace);
        if (end == std::string::npos) break;
        out += text.substr(0, start);
        text = text.substr(end);
    }
    return out + text;
}

std::string rewrite_compound_returns(std::string body, const std::function<std::vector<std::string>(const std::string&)>& fields_of)
{
    std::regex re(R"(return\s*\(\s*(\w+)\s*\)\s*\{)");
    std::smatch m;
    std::string out;
    while (std::regex_search(body, m, re))
    {
        size_t brace = size_t(m.position(0) + m.length(0)) - 1;
        size_t end = match(body, brace);
        if (end == std::string::npos) break;
        std::string type = m[1];
        std::vector<std::string> names = fields_of(type);
        std::string code = "{ " + type + " orchard_lit; ";
        auto parts = split_top(body.substr(brace + 1, end - brace - 2), ',');
        for (size_t i = 0; i < parts.size(); ++i)
        {
            std::string p = std::regex_replace(parts[i], std::regex(R"(^\s+|\s+$)"), "");
            if (p.empty()) continue;
            if (p[0] == '.' && p.find('=') != std::string::npos)
                code += "orchard_lit." + std::regex_replace(p.substr(1, p.find('=') - 1), std::regex(R"(\s+)"), "") + " = " +
                        p.substr(p.find('=') + 1) + "; ";
            else if (i < names.size())
                code += "orchard_lit." + names[i] + " = " + p + "; ";
        }
        code += "return orchard_lit; }";
        size_t semi = body.find_first_not_of(" \t\r\n", end);
        if (semi != std::string::npos && body[semi] == ';') end = semi + 1;
        out += body.substr(0, size_t(m.position(0))) + code;
        body = body.substr(end);
    }
    return out + body;
}

std::string qualifier_for(const std::string& attr)
{
    if (has_attr(attr, "flat")) return "nointerpolation ";
    if (attr.find("no_perspective") != std::string::npos) return "noperspective ";
    return "";
}

}

Result translate(const std::string& source, const std::string& entry, const Options& opt)
{
    Result r;
    std::string src = source;
    src = regex_all(src, R"(#include\s*<[^>]*>)", "");
    src = regex_all(src, R"(using\s+namespace\s+metal\s*;)", "");

    std::map<std::string, uint64_t> consts = opt.named_constants;
    {
        std::regex fc(R"(constant\s+(\w+)\s+(\w+)\s*\[\[\s*function_constant\s*\(\s*(\w+)\s*\)\s*\]\]\s*;)");
        std::smatch m;
        std::string out, rest = src;
        while (std::regex_search(rest, m, fc))
        {
            std::string type = hlsl_type(m[1]), name = m[2], idx = m[3];
            uint64_t v = 0;
            if (std::isdigit(uint8_t(idx[0])))
            {
                auto it = opt.constants.find(std::stoi(idx));
                if (it != opt.constants.end()) v = it->second;
            }
            else if (opt.named_constants.count(name))
            {
                v = opt.named_constants.at(name);
            }
            consts[name] = v;
            std::string value = type == "bool" ? (v ? "true" : "false") : std::to_string(v);
            out += rest.substr(0, size_t(m.position(0))) + "static const " + type + " " + name + " = " + value + ";";
            rest = m.suffix();
        }
        src = out + rest;
        std::regex derived(R"(constant\s+const\s+(\w+)\s+(\w+)\s*=\s*\(\s*(\w+)\s*>>\s*(\d+)\s*\)\s*&\s*0x([0-9A-Fa-f]+)\s*;)");
        out.clear();
        rest = src;
        while (std::regex_search(rest, m, derived))
        {
            uint64_t v = (consts[m[3]] >> std::stoi(m[4])) & std::stoull(m[5].str(), nullptr, 16);
            consts[m[2]] = v;
            out += rest.substr(0, size_t(m.position(0))) + "static const uint " + m[2].str() + " = " + std::to_string(v) + ";";
            rest = m.suffix();
        }
        src = out + rest;
    }

    src = expand_templates(src);

    std::smatch em;
    std::regex entry_re("(vertex|fragment|kernel)\\s+(\\w+)\\s+" + entry + "\\s*\\(");
    if (!std::regex_search(src, em, entry_re))
    {
        r.error = "entry point " + entry + " not found";
        return r;
    }
    r.vertex = em[1] == "vertex";
    if (em[1] == "kernel")
    {
        r.error = "compute kernels are not supported yet";
        return r;
    }
    std::string ret_type = em[2];
    size_t sig_begin = size_t(em.position(0));
    size_t params_open = size_t(em.position(0) + em.length(0) - 1);
    size_t params_close = match(src, params_open);
    size_t body_open = src.find('{', params_close);
    size_t body_close = match(src, body_open);
    std::string params_text = src.substr(params_open + 1, params_close - params_open - 2);
    std::string body = src.substr(body_open + 1, body_close - body_open - 2);
    std::string before = src.substr(0, sig_begin), after = src.substr(body_close);
    before = regex_all(before, R"(\bconstant\s+const\b)", "static const");
    before = regex_all(before, R"((^|\n)\s*constant\s+(\w))", "$1static const $2");

    std::vector<MslStruct> structs;
    std::map<std::string, size_t> struct_index;
    {
        std::regex sre(R"(struct\s+(\w+)\s*\{([^{}]*)\}\s*;)");
        for (auto it = std::sregex_iterator(before.begin(), before.end(), sre); it != std::sregex_iterator(); ++it)
        {
            MslStruct st;
            st.name = (*it)[1];
            st.fields = parse_fields((*it)[2]);
            struct_index[st.name] = structs.size();
            structs.push_back(st);
        }
        before = std::regex_replace(before, sre, "");
    }
    before = strip_entry_functions(before);
    after = strip_entry_functions(after);
    body = rewrite_compound_returns(body, [&](const std::string& type) {
        std::vector<std::string> names;
        if (auto s = struct_index.find(type); s != struct_index.end())
            for (auto& f : structs[s->second].fields)
                names.push_back(f.name);
        return names;
    });
    struct Layout
    {
        uint32_t size = 0, align = 4;
        std::vector<uint32_t> offsets;
    };
    std::map<std::string, Layout> layouts;
    std::function<Layout(const std::string&)> layout_of = [&](const std::string& type) -> Layout {
        if (auto l = layouts.find(type); l != layouts.end()) return l->second;
        Layout out;
        if (auto s = struct_index.find(type); s != struct_index.end())
        {
            uint32_t off = 0;
            for (auto& f : structs[s->second].fields)
            {
                Layout fl = layout_of(f.type);
                off = (off + fl.align - 1) / fl.align * fl.align;
                out.offsets.push_back(off);
                off += fl.size * uint32_t(std::max(f.array, 1));
                out.align = std::max(out.align, fl.align);
            }
            out.size = (off + out.align - 1) / out.align * out.align;
        }
        else
        {
            size_align(type_info(type), out.size, out.align);
        }
        layouts[type] = out;
        return out;
    };
    std::set<std::string> needed_loaders;
    std::function<std::string(const std::string&, const std::string&, const std::string&)> load_value =
        [&](const std::string& type, const std::string& buf, const std::string& off) -> std::string {
        if (struct_index.count(type))
        {
            needed_loaders.insert(type);
            return "orchard_load_" + type + "(" + buf + ", " + off + ")";
        }
        TypeInfo ti = type_info(type);
        if (!ti.rows) return load_expr(ti, buf, off);
        TypeInfo col = ti;
        col.cols = ti.rows;
        col.rows = 0;
        uint32_t csz, cal;
        size_align(col, csz, cal);
        std::string cols;
        for (int k = 0; k < ti.cols; ++k)
            cols += (k ? ", " : "") + load_expr(col, buf, off + " + " + std::to_string(uint32_t(k) * csz) + "u");
        return hlsl_type(type) + "(" + cols + ")";
    };

    std::string decls, prologue, signature;
    std::string stage_in_type;
    std::map<std::string, TextureDecl> textures;
    std::map<std::string, int> sampler_names;
    std::vector<std::pair<std::string, std::string>> sv_params;
    struct PointerBuffer
    {
        std::string type, buf, off;
        uint32_t stride;
    };
    std::map<std::string, PointerBuffer> pointer_buffers;
    for (auto& p : split_top(params_text, ','))
    {
        std::string attr;
        std::string decl = p;
        size_t a = p.find("[[");
        if (a != std::string::npos)
        {
            attr = trim(p.substr(a + 2, p.find("]]", a) - a - 2));
            decl = trim(p.substr(0, a));
        }
        std::smatch pm;
        std::regex_match(decl, pm, std::regex(R"(([\s\S]*?)\s*(\w+)\s*)"));
        std::string type = trim(pm[1]), name = pm[2];
        std::string fc = attr_arg(attr, "function_constant");
        bool enabled = fc.empty() || consts[fc] != 0;
        if (type.find("texture") != std::string::npos || type.find("depth") == 0)
        {
            int slot = std::stoi(attr_arg(attr, "texture"));
            TextureDecl td;
            td.depth = type.starts_with("depth");
            std::smatch km;
            std::regex_search(type, km, std::regex(R"((?:texture|depth)(2d_array|2d_ms|2d|3d|cube_array|cube|1d))"));
            td.kind = km[1];
            textures[name] = td;
            std::string elem = td.depth ? "float" : "float4";
            if (type.find("<int") != std::string::npos) elem = "int4";
            if (type.find("<uint") != std::string::npos) elem = "uint4";
            std::string hk = td.kind == "2d_array"     ? "Texture2DArray"
                             : td.kind == "3d"         ? "Texture3D"
                             : td.kind == "cube"       ? "TextureCube"
                             : td.kind == "cube_array" ? "TextureCubeArray"
                             : td.kind == "2d_ms"      ? "Texture2DMS"
                                                       : "Texture2D";
            decls += hk + "<" + elem + "> " + name + " : register(t" + std::to_string(slot) + ");\n";
            r.textures.push_back(slot);
        }
        else if (type == "sampler")
        {
            int slot = std::stoi(attr_arg(attr, "sampler"));
            sampler_names[name] = slot;
            decls += "SAMPLER_" + name + " " + name + " : register(s" + std::to_string(slot) + ");\n";
            r.samplers.push_back(slot);
        }
        else if (!attr_arg(attr, "buffer").empty())
        {
            int slot = std::stoi(attr_arg(attr, "buffer"));
            bool pointer = type.find('*') != std::string::npos;
            std::string st = trim(regex_all(type, R"(\b(constant|const|device)\b|&|\*)", ""));
            std::string buf = "orchard_buf" + std::to_string(slot);
            std::string off = "orchard_offsets[" + std::to_string(slot / 4) + "][" + std::to_string(slot % 4) + "]";
            decls += "ByteAddressBuffer " + buf + " : register(t" + std::to_string(kBufferRegisterBase + slot) + ");\n";
            Layout l = layout_of(st);
            if (pointer)
            {
                pointer_buffers[name] = {st, buf, off, l.size};
                r.buffers[slot] = 0;
            }
            else
            {
                prologue += "    " + hlsl_type(st) + " " + name + " = " + load_value(st, buf, off) + ";\n";
                r.buffers[slot] = l.size;
            }
        }
        else if (has_attr(attr, "stage_in"))
        {
            stage_in_type = type;
            signature += (signature.empty() ? "" : ", ") + type + " " + name;
        }
        else if (has_attr(attr, "vertex_id"))
        {
            sv_params.push_back({"uint " + name + " : SV_VertexID", name});
        }
        else if (has_attr(attr, "instance_id"))
        {
            sv_params.push_back({"uint " + name + " : SV_InstanceID", name});
            prologue += "    " + name + " += orchard_base_instance;\n";
        }
        else if (has_attr(attr, "base_vertex"))
        {
            prologue += "    uint " + name + " = " + (enabled ? "orchard_base_vertex" : "0") + ";\n";
        }
        else if (has_attr(attr, "base_instance"))
        {
            prologue += "    uint " + name + " = " + (enabled ? "orchard_base_instance" : "0") + ";\n";
        }
        else if (has_attr(attr, "front_facing"))
        {
            sv_params.push_back({"bool " + name + " : SV_IsFrontFace", name});
        }
        else if (has_attr(attr, "position"))
        {
            sv_params.push_back({"float4 " + name + " : SV_Position", name});
        }
        else if (has_attr(attr, "sample_id"))
        {
            sv_params.push_back({"uint " + name + " : SV_SampleIndex", name});
        }
        else
        {
            r.error = "unsupported parameter: " + p;
            return r;
        }
    }

    for (auto& [name, pb] : pointer_buffers)
    {
        std::string out, rest = body;
        std::regex use("\\b" + name + "\\s*\\[");
        std::smatch m;
        while (std::regex_search(rest, m, use))
        {
            size_t open = size_t(m.position(0) + m.length(0) - 1);
            size_t close = match(rest, open);
            std::string index = rest.substr(open + 1, close - open - 2);
            out += rest.substr(0, size_t(m.position(0))) + "(" +
                   load_value(pb.type, pb.buf, pb.off + " + uint(" + index + ") * " + std::to_string(pb.stride) + "u") + ")";
            rest = rest.substr(close);
        }
        body = out + rest;
    }

    std::string struct_text, loaders;
    for (auto& st : structs)
    {
        const std::string& sname = st.name;
        bool is_input = sname == stage_in_type, is_output = sname == ret_type;
        std::string text = "struct " + sname + "\n{\n";
        if (is_input && !r.vertex && opt.vertex_outputs)
        {
            std::set<std::string> mine;
            int unused_target = 0;
            auto field_sem = [&](const MslField& f) {
                std::string s = semantic_for(f.attr, false, false, unused_target, consts);
                return s.empty() ? "ORCHARD_" + f.name : s;
            };
            for (auto& f : st.fields)
                mine.insert(field_sem(f));
            int k = 0;
            for (auto& vf : *opt.vertex_outputs)
            {
                std::string name = vf.name;
                for (auto& f : st.fields)
                    if (field_sem(f) == vf.semantic) name = f.name;
                if (!mine.count(vf.semantic)) name = "orchard_unused" + std::to_string(k++);
                text += "    " + vf.qualifier + vf.type + " " + name + (vf.array ? "[" + std::to_string(vf.array) + "]" : "") + " : " +
                        vf.semantic + ";\n";
            }
        }
        else
        {
            for (auto& f : st.fields)
            {
                int target = -1;
                std::string sem =
                    (is_input || is_output) ? semantic_for(f.attr, is_input && r.vertex, is_output && !r.vertex, target, consts) : "";
                if ((is_input || is_output) && has_attr(f.attr, "point_size")) continue;
                if (sem.empty() && ((is_output && r.vertex) || (is_input && !r.vertex))) sem = "ORCHARD_" + f.name;
                std::string ht = hlsl_type(f.type);
                std::string q = (is_input || is_output) ? qualifier_for(f.attr) : "";
                text += "    " + q + ht + " " + f.name + (f.array ? "[" + std::to_string(f.array) + "]" : "") +
                        (sem.empty() ? "" : " : " + sem) + ";\n";
                if (is_output && r.vertex) r.outputs.push_back({ht, f.name, f.array, sem, q});
                if (is_input && r.vertex)
                {
                    std::string n = attr_arg(f.attr, "attribute");
                    if (!n.empty()) r.attributes.push_back(std::stoi(n));
                }
            }
        }
        text += "};\n";
        struct_text += text;
    }
    std::set<std::string> emitted;
    std::function<void(const std::string&)> emit_loader = [&](const std::string& sname) {
        if (!emitted.insert(sname).second) return;
        const MslStruct& st = structs[struct_index[sname]];
        Layout l = layout_of(sname);
        std::string fn = sname + " orchard_load_" + sname + "(ByteAddressBuffer b, uint base)\n{\n    " + sname + " v;\n";
        for (size_t i = 0; i < st.fields.size(); ++i)
        {
            const MslField& f = st.fields[i];
            Layout fl = layout_of(f.type);
            int count = std::max(f.array, 1);
            for (int e = 0; e < count; ++e)
            {
                uint32_t at = l.offsets[i] + uint32_t(e) * fl.size;
                std::string lhs = "v." + f.name + (f.array ? "[" + std::to_string(e) + "]" : "");
                fn += "    " + lhs + " = " + load_value(f.type, "b", "base + " + std::to_string(at) + "u") + ";\n";
            }
        }
        fn += "    return v;\n}\n";
        // HLSL needs nested loaders defined first
        for (auto& f : st.fields)
            if (struct_index.count(f.type)) emit_loader(f.type);
        loaders += fn;
    };
    for (std::string n : std::vector<std::string>(needed_loaders.begin(), needed_loaders.end()))
        emit_loader(n);

    std::set<std::string> compare_samplers;
    body = rewrite_texture_calls(body, textures, compare_samplers, r.vertex);
    before = rewrite_texture_calls(before, textures, compare_samplers, r.vertex);
    for (std::string* text : {&before, &body})
    {
        std::regex cs(R"(constexpr\s+sampler\s+(\w+)\s*\(([^)]*)\)\s*;)");
        std::smatch m;
        std::string out, rest = *text;
        while (std::regex_search(rest, m, cs))
        {
            ConstSampler s;
            s.slot = kConstSamplerTop - int(r.const_samplers.size());
            for (auto& tok : split_top(m[2].str(), ','))
            {
                auto sep = tok.find("::");
                if (sep == std::string::npos) continue;
                std::string key = trim(tok.substr(0, sep)), val = trim(tok.substr(sep + 2));
                bool lin = val == "linear";
                if (key == "filter")
                    s.linear_min = s.linear_mag = lin;
                else if (key == "min_filter")
                    s.linear_min = lin;
                else if (key == "mag_filter")
                    s.linear_mag = lin;
                else if (key == "mip_filter")
                {
                    s.has_mip = val != "none";
                    s.linear_mip = lin;
                }
                else if (key.ends_with("address"))
                {
                    s.address = val == "repeat"                                        ? 1
                                : val == "mirrored_repeat"                             ? 2
                                : (val == "clamp_to_zero" || val == "clamp_to_border") ? 3
                                                                                       : 0;
                }
                else if (key == "compare_func")
                {
                    static const std::vector<std::string> kFuncs = {"never",   "less",      "equal",         "less_equal",
                                                                    "greater", "not_equal", "greater_equal", "always"};
                    auto fit = std::find(kFuncs.begin(), kFuncs.end(), val);
                    s.compare = fit == kFuncs.end() ? 0 : int(fit - kFuncs.begin()) + 1;
                }
            }
            std::string name = m[1];
            decls += std::string(s.compare ? "SamplerComparisonState " : "SamplerState ") + name + " : register(s" +
                     std::to_string(s.slot) + ");\n";
            r.const_samplers.push_back(s);
            out += rest.substr(0, size_t(m.position(0)));
            rest = m.suffix();
        }
        *text = out + rest;
    }
    for (auto& [n, slot] : sampler_names)
    {
        decls = replace_all(decls, "SAMPLER_" + n + " ", compare_samplers.count(n) ? "SamplerComparisonState " : "SamplerState ");
        if (compare_samplers.count(n)) r.compare_samplers.push_back(slot);
    }

    body = regex_all(body, R"(\n[^\n;]*\.mtl_PointSize\s*=[^;]*;)", "\n");

    std::ostringstream h;
    h << "cbuffer OrchardDraw : register(b0)\n{\n    uint4 orchard_offsets[8];\n    uint orchard_base_vertex;\n    uint "
         "orchard_base_instance;\n    uint2 orchard_pad;\n};\n";
    h << "uint4 orchard_dims(Texture2D<float4> t, uint lod) { uint w, hh, l; t.GetDimensions(lod, w, hh, l); return uint4(w, hh, 1, l); "
         "}\n";
    h << "uint4 orchard_dims(Texture2D<float> t, uint lod) { uint w, hh, l; t.GetDimensions(lod, w, hh, l); return uint4(w, hh, 1, l); }\n";
    h << "uint4 orchard_dims(TextureCube<float4> t, uint lod) { uint w, hh, l; t.GetDimensions(lod, w, hh, l); return uint4(w, hh, 1, l); "
         "}\n";
    h << "uint4 orchard_dims(Texture2DArray<float4> t, uint lod) { uint w, hh, e, l; t.GetDimensions(lod, w, hh, e, l); return uint4(w, "
         "hh, e, l); }\n";
    h << "uint4 orchard_dims(Texture3D<float4> t, uint lod) { uint w, hh, d, l; t.GetDimensions(lod, w, hh, d, l); return uint4(w, hh, d, "
         "l); }\n";
    h << translate_expressions(struct_text) << "\n" << decls << "\n" << loaders << "\n";
    h << translate_expressions(before) << "\n";
    std::string sig = ret_type + " main(" + signature;
    for (auto& [d, n] : sv_params)
        sig += (sig.back() == '(' ? "" : ", ") + d;
    std::string ret_semantic;
    if (ret_type != "void" && !struct_index.count(ret_type)) ret_semantic = r.vertex ? " : SV_Position" : " : SV_Target0";
    h << sig << ")" << ret_semantic << "\n{\n" << prologue << translate_expressions(body) << "\n}\n" << translate_expressions(after);
    r.hlsl = h.str();
    r.ok = true;
    return r;
}

}
