#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/runtime.h"
#include "cpu/cpu.h"
#include "frameworks/Foundation/foundation.h"
#include "frameworks/Foundation/objects.h"
#include "frameworks/libSystem/blocks.h"
#include "frameworks/libSystem/dispatch.h"
#include "hle/hle.h"
#include "host/permission.h"
#include "objc/runtime.h"

namespace orchard::foundation
{
using objc::objc;

namespace
{
using Headers = std::vector<std::pair<std::string, std::string>>;

template <typename T> class Table
{
public:
    std::shared_ptr<T> get(Id obj)
    {
        std::lock_guard g(lock_);
        auto& p = items_[obj];
        if (!p) p = std::make_shared<T>();
        return p;
    }
    std::shared_ptr<T> find(Id obj)
    {
        std::lock_guard g(lock_);
        auto it = items_.find(obj);
        return it == items_.end() ? nullptr : it->second;
    }
    void erase(Id obj)
    {
        std::lock_guard g(lock_);
        items_.erase(obj);
    }

private:
    std::mutex lock_;
    std::unordered_map<Id, std::shared_ptr<T>> items_;
};

struct Request
{
    std::string url, method = "GET";
    Headers headers;
    std::vector<uint8_t> body;
    double timeout = 60;
    uint64_t cache_policy = 0;
};

struct Response
{
    std::string url, mime;
    int64_t status = 200;
    Headers headers;
    int64_t expected = -1;
};

struct Config
{
    Headers headers;
    double timeout = 60;
};

struct Session
{
    Id delegate = 0;
    Id queue = 0;
    Config config;
};

enum class Kind
{
    Data,
    Upload,
    Download
};

struct Task
{
    Id session = 0, delegate = 0, handler = 0;
    Id original_request = 0, response = 0, error = 0;
    Kind kind = Kind::Data;
    Request request;
    uint64_t ident = 0;
    std::atomic<uint64_t> state{1};
    std::atomic<bool> cancelled{false};
    std::string description;
    int64_t status = 0, error_code = 0;
    std::string final_url, error_text;
    Headers response_headers;
    std::vector<uint8_t> data;
};

Table<Request>& requests()
{
    static Table<Request> t;
    return t;
}
Table<Response>& responses()
{
    static Table<Response> t;
    return t;
}
Table<Config>& configs()
{
    static Table<Config> t;
    return t;
}
Table<Session>& sessions()
{
    static Table<Session> t;
    return t;
}
Table<Task>& tasks()
{
    static Table<Task> t;
    return t;
}

std::atomic<uint64_t> g_next_ident{1};
std::atomic<uint64_t> g_next_download{1};

Id send(Cpu& c, Id o, const char* sel, std::initializer_list<uint64_t> args = {})
{
    return objc(c).send(c, o, sel, args);
}

bool responds(Cpu& c, Id obj, const char* sel)
{
    return obj && objc(c).responds(objc(c).class_of(obj), objc(c).sel(sel));
}

Id make(Cpu& c, const char* cls)
{
    return objc(c).alloc_instance(objc(c).host_class(cls));
}

std::string string_arg(Cpu& c, Id s)
{
    return s ? to_utf8(c, s) : std::string();
}

std::string url_text(Cpu& c, Id url)
{
    if (!url) return {};
    if (is_string(c, url)) return to_utf8(c, url);
    return string_arg(c, send(c, url, "absoluteString"));
}

Id url_object(Cpu& c, const std::string& text)
{
    return send(c, objc(c).host_class("NSURL")->addr, "URLWithString:", {string_autoreleased(c, text)});
}

std::vector<uint8_t> data_bytes(Cpu& c, Id data)
{
    if (!data) return {};
    uint64_t n = send(c, data, "length");
    GuestAddr p = send(c, data, "bytes");
    std::vector<uint8_t> out(n);
    if (n && p) c.mem.read_bytes(p, out.data(), n);
    return out;
}

bool same_header(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

void set_header(Headers& h, const std::string& name, const std::string& value, bool append)
{
    for (auto& [k, v] : h)
        if (same_header(k, name))
        {
            v = append ? v + "," + value : value;
            return;
        }
    h.push_back({name, value});
}

const std::string* find_header(const Headers& h, const std::string& name)
{
    for (auto& [k, v] : h)
        if (same_header(k, name)) return &v;
    return nullptr;
}

Headers headers_from_dict(Cpu& c, Id dict)
{
    Headers out;
    if (!dict) return out;
    for (auto& [k, v] : dict_entries(c, dict))
        out.push_back({string_arg(c, k), is_string(c, v) ? to_utf8(c, v) : string_arg(c, send(c, v, "description"))});
    return out;
}

Id headers_to_dict(Cpu& c, const Headers& h)
{
    std::vector<std::pair<Id, Id>> entries;
    for (auto& [k, v] : h)
        entries.push_back({string_autoreleased(c, k), string_autoreleased(c, v)});
    return make_dict(c, entries);
}

std::wstring widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string narrow(const std::wstring& w)
{
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

int64_t url_error(DWORD err)
{
    switch (err)
    {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return -1003;
    case ERROR_WINHTTP_TIMEOUT: return -1001;
    case ERROR_WINHTTP_CANNOT_CONNECT: return -1004;
    case ERROR_WINHTTP_SECURE_FAILURE: return -1202;
    case ERROR_WINHTTP_OPERATION_CANCELLED: return -999;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_INVALID_URL: return -1002;
    default: return -1005;
    }
}

const char* url_error_text(int64_t code)
{
    switch (code)
    {
    case -999: return "cancelled";
    case -1001: return "The request timed out.";
    case -1002: return "unsupported URL";
    case -1003: return "A server with the specified hostname could not be found.";
    case -1004: return "Could not connect to the server.";
    case -1009: return "The Internet connection appears to be offline.";
    case -1202: return "The certificate for this server is invalid.";
    default: return "The network connection was lost.";
    }
}

HINTERNET http_session()
{
    static HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(L"Orchard", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h)
        {
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
        return h;
    }();
    return s;
}

void perform(Task& t, const Headers& extra, double timeout)
{
    struct Handle
    {
        HINTERNET h = nullptr;
        ~Handle()
        {
            if (h) WinHttpCloseHandle(h);
        }
    } connect, request;
    auto fail = [&](DWORD err) {
        t.error_code = url_error(err);
        if (t.cancelled) t.error_code = -999;
    };

    std::wstring url = widen(t.request.url);
    URL_COMPONENTS parts = {sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwSchemeLength = DWORD(-1);
    if (!http_session() || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return fail(GetLastError());
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    connect.h = WinHttpConnect(http_session(), host.c_str(), parts.nPort, 0);
    if (!connect.h) return fail(GetLastError());
    request.h = WinHttpOpenRequest(connect.h, widen(t.request.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) return fail(GetLastError());
    int ms = int(std::max(1.0, timeout) * 1000);
    WinHttpSetTimeouts(request.h, ms, ms, ms, ms);

    Headers headers = extra;
    for (auto& [k, v] : t.request.headers)
        set_header(headers, k, v, false);
    if (!find_header(headers, "User-Agent")) headers.push_back({"User-Agent", "CFNetwork/1410.0.3 Darwin/22.6.0"});
    std::wstring header_block;
    for (auto& [k, v] : headers)
        header_block += widen(k) + L": " + widen(v) + L"\r\n";

    if (t.cancelled) return fail(ERROR_WINHTTP_OPERATION_CANCELLED);
    auto& body = t.request.body;
    if (!WinHttpSendRequest(request.h, header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(), DWORD(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data(), DWORD(body.size()), DWORD(body.size()), 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return fail(GetLastError());

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    t.status = status;

    size = 0;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                            WINHTTP_NO_HEADER_INDEX))
    {
        std::string text = narrow(raw.substr(0, size / sizeof(wchar_t)));
        size_t pos = text.find("\r\n");
        while (pos != std::string::npos && pos + 2 < text.size())
        {
            size_t end = text.find("\r\n", pos + 2);
            std::string line = text.substr(pos + 2, end == std::string::npos ? std::string::npos : end - pos - 2);
            size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(' '));
                set_header(t.response_headers, line.substr(0, colon), value, true);
            }
            pos = end;
        }
    }

    size = 0;
    WinHttpQueryOption(request.h, WINHTTP_OPTION_URL, nullptr, &size);
    std::wstring final_url(size / sizeof(wchar_t), L'\0');
    if (size && WinHttpQueryOption(request.h, WINHTTP_OPTION_URL, final_url.data(), &size))
        t.final_url = narrow(final_url.c_str());
    else
        t.final_url = t.request.url;

    for (;;)
    {
        if (t.cancelled) return fail(ERROR_WINHTTP_OPERATION_CANCELLED);
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) return fail(GetLastError());
        if (!avail) break;
        size_t old = t.data.size();
        t.data.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, t.data.data() + old, avail, &got)) return fail(GetLastError());
        t.data.resize(old + got);
    }
}

Id make_error(Cpu& c, int64_t code, const std::string& url)
{
    Id info = make_dict(c, {{string_autoreleased(c, "NSLocalizedDescription"), string_autoreleased(c, url_error_text(code))},
                            {string_autoreleased(c, "NSErrorFailingURLStringKey"), string_autoreleased(c, url)}});
    return send(c, objc(c).host_class("NSError")->addr,
                "errorWithDomain:code:userInfo:", {string_autoreleased(c, "NSURLErrorDomain"), uint64_t(code), info});
}

Id make_response(Cpu& c, const Task& t)
{
    Id r = make(c, "NSHTTPURLResponse");
    auto state = responses().get(r);
    state->url = t.final_url.empty() ? t.request.url : t.final_url;
    state->status = t.status;
    state->headers = t.response_headers;
    if (auto* len = find_header(t.response_headers, "Content-Length")) state->expected = std::atoll(len->c_str());
    if (auto* type = find_header(t.response_headers, "Content-Type")) state->mime = type->substr(0, type->find(';'));
    return r;
}

GuestAddr response_handler_block(Cpu& c)
{
    static GuestAddr block = [&] {
        GuestAddr invoke = c.rt.hle.make_stub("NSURLSession response disposition", [](Cpu&) {});
        GuestAddr desc = c.rt.mem.alloc_system(16, 8);
        c.rt.mem.write<uint64_t>(desc, 0);
        c.rt.mem.write<uint64_t>(desc + 8, 32);
        GuestAddr b = c.rt.mem.alloc_system(32, 16);
        c.rt.mem.write<uint64_t>(b, objc(c).host_class("__NSGlobalBlock__")->addr);
        c.rt.mem.write<uint32_t>(b + 8, 1u << 28);
        c.rt.mem.write<uint32_t>(b + 12, 0);
        c.rt.mem.write<uint64_t>(b + 16, invoke);
        c.rt.mem.write<uint64_t>(b + 24, desc);
        return b;
    }();
    return block;
}

void deliver(Cpu& c)
{
    Id task = c.arg(0);
    auto t = tasks().find(task);
    if (!t) return;
    auto session = sessions().find(t->session);
    Id session_obj = t->session;
    Id delegate = t->delegate ? t->delegate : (session ? session->delegate : 0);

    if (t->cancelled) t->error_code = -999;
    if (!t->error_code && t->status)
    {
        t->response = objc(c).retain(make_response(c, *t));
    }
    if (t->error_code) t->error = objc(c).retain(make_error(c, t->error_code, t->request.url));
    std::printf("[network] %s %s -> %lld (%zu bytes)\n", t->request.method.c_str(), t->request.url.c_str(),
                (long long)(t->error_code ? t->error_code : t->status), t->data.size());

    Id payload = 0;
    if (!t->error_code && t->kind == Kind::Download)
    {
        std::string guest = c.rt.vfs.home + "/tmp/CFNetworkDownload_" + std::to_string(g_next_download++) + ".tmp";
        if (auto host = c.rt.vfs.to_host(guest))
        {
            std::ofstream(*host, std::ios::binary).write(reinterpret_cast<const char*>(t->data.data()), std::streamsize(t->data.size()));
            payload = new_file_url(c, guest);
        }
    }
    else if (!t->error_code)
    {
        payload = make_data(c, t->data);
    }
    t->state = 3;

    if (t->handler)
    {
        call_block(c, t->handler, {payload, t->response, t->error});
        block_release(c, t->handler);
        t->handler = 0;
    }
    else if (delegate)
    {
        uint64_t n = t->data.size();
        if (t->kind == Kind::Upload && !t->request.body.empty() &&
            responds(c, delegate, "URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:"))
        {
            uint64_t sent = t->request.body.size();
            send(c, delegate,
                 "URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:", {session_obj, task, sent, sent, sent});
        }
        if (t->kind == Kind::Download)
        {
            if (payload && responds(c, delegate, "URLSession:downloadTask:didWriteData:totalBytesWritten:totalBytesExpectedToWrite:"))
                send(c, delegate,
                     "URLSession:downloadTask:didWriteData:totalBytesWritten:totalBytesExpectedToWrite:", {session_obj, task, n, n, n});
            if (payload && responds(c, delegate, "URLSession:downloadTask:didFinishDownloadingToURL:"))
                send(c, delegate, "URLSession:downloadTask:didFinishDownloadingToURL:", {session_obj, task, payload});
        }
        else
        {
            if (t->response && responds(c, delegate, "URLSession:dataTask:didReceiveResponse:completionHandler:"))
                send(c, delegate, "URLSession:dataTask:didReceiveResponse:completionHandler:",
                     {session_obj, task, t->response, response_handler_block(c)});
            if (payload && n && responds(c, delegate, "URLSession:dataTask:didReceiveData:"))
                send(c, delegate, "URLSession:dataTask:didReceiveData:", {session_obj, task, payload});
        }
        if (responds(c, delegate, "URLSession:task:didCompleteWithError:"))
            send(c, delegate, "URLSession:task:didCompleteWithError:", {session_obj, task, t->error});
    }
    t->data.clear();
    t->data.shrink_to_fit();
    objc(c).release(c, task);
}

GuestAddr delivery_stub(Cpu& c)
{
    static GuestAddr stub = c.rt.hle.make_stub("NSURLSession delivery", deliver);
    return stub;
}

void finish_on_queue(Cpu& c, Id task, Id queue)
{
    dispatch_function_async(queue, delivery_stub(c), task);
}

void resume(Cpu& c, Id task)
{
    auto t = tasks().find(task);
    if (!t || t->state != 1) return;
    t->state = 0;
    auto session = sessions().find(t->session);
    Id queue = session ? session->queue : 0;
    objc(c).retain(task);
    if (t->cancelled || !network_allowed())
    {
        t->error_code = t->cancelled ? -999 : -1009;
        return finish_on_queue(c, task, queue);
    }
    Headers extra = session ? session->config.headers : Headers{};
    double timeout = std::min(t->request.timeout, session ? session->config.timeout : 60.0);
    GuestAddr stub = delivery_stub(c);
    std::thread([t, extra, timeout, task, queue, stub] {
        perform(*t, extra, timeout);
        dispatch_function_async(queue, stub, task);
    }).detach();
}

Id new_task(Cpu& c, Id session, Id request, Kind kind, Id handler, Id delegate = 0)
{
    const char* cls = kind == Kind::Download ? "NSURLSessionDownloadTask"
                      : kind == Kind::Upload ? "NSURLSessionUploadTask"
                                             : "NSURLSessionDataTask";
    Id task = make(c, cls);
    auto t = tasks().get(task);
    t->session = objc(c).retain(session);
    t->kind = kind;
    t->ident = g_next_ident++;
    t->handler = handler ? block_copy(c, handler) : 0;
    t->delegate = delegate ? objc(c).retain(delegate) : 0;
    if (auto r = requests().find(request)) t->request = *r;
    t->original_request = objc(c).retain(request);
    if (session)
        if (auto s = sessions().find(session); s && s->delegate && responds(c, s->delegate, "URLSession:didCreateTask:"))
            send(c, s->delegate, "URLSession:didCreateTask:", {session, task});
    return objc(c).autorelease(c, task);
}

Id request_for_url(Cpu& c, Id url)
{
    Id r = make(c, "NSURLRequest");
    requests().get(r)->url = url_text(c, url);
    return objc(c).autorelease(c, r);
}

Id new_session(Cpu& c, Id config, Id delegate, Id queue)
{
    Id s = make(c, "NSURLSession");
    auto state = sessions().get(s);
    if (auto cfg = configs().find(config)) state->config = *cfg;
    state->delegate = delegate ? objc(c).retain(delegate) : 0;
    if (queue && objc(c).is_kind_of(queue, objc(c).class_named("NSOperationQueue")))
        state->queue = c.mem.read<uint64_t>(queue + 8);
    else if (delegate)
        state->queue = create_serial_queue(c, "com.apple.NSURLSession-delegate");
    return s;
}

void register_requests(objc::ObjcRuntime& o)
{
    auto init_url = [](Cpu& c) {
        auto r = requests().get(c.arg(0));
        r->url = url_text(c, c.arg(2));
        c.ret(c.arg(0));
    };
    static decltype(init_url) s_init = init_url;
    o.method("NSURLRequest", "initWithURL:", [](Cpu& c) { s_init(c); });
    o.method("NSURLRequest", "initWithURL:cachePolicy:timeoutInterval:", [](Cpu& c) {
        auto r = requests().get(c.arg(0));
        r->url = url_text(c, c.arg(2));
        r->cache_policy = c.arg(3);
        if (c.d(0) > 0) r->timeout = c.d(0);
        c.ret(c.arg(0));
    });
    o.class_method("NSURLRequest", "requestWithURL:", [](Cpu& c) {
        Id r = send(c, c.arg(0), "alloc");
        requests().get(r)->url = url_text(c, c.arg(2));
        c.ret(objc(c).autorelease(c, r));
    });
    o.class_method("NSURLRequest", "requestWithURL:cachePolicy:timeoutInterval:", [](Cpu& c) {
        double timeout = c.d(0);
        Id r = send(c, c.arg(0), "alloc");
        auto state = requests().get(r);
        state->url = url_text(c, c.arg(2));
        state->cache_policy = c.arg(3);
        if (timeout > 0) state->timeout = timeout;
        c.ret(objc(c).autorelease(c, r));
    });
    o.method("NSURLRequest", "init", [](Cpu& c) {
        requests().get(c.arg(0));
        c.ret(c.arg(0));
    });
    o.method("NSURLRequest", "dealloc", [](Cpu& c) {
        requests().erase(c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    auto copy_as = [](Cpu& c, const char* cls) {
        Id r = make(c, cls);
        *requests().get(r) = *requests().get(c.arg(0));
        c.ret(r);
    };
    static decltype(copy_as) s_copy = copy_as;
    o.method("NSURLRequest", "copyWithZone:", [](Cpu& c) { s_copy(c, "NSURLRequest"); });
    o.method("NSURLRequest", "mutableCopyWithZone:", [](Cpu& c) { s_copy(c, "NSMutableURLRequest"); });
    o.method("NSURLRequest", "URL", [](Cpu& c) { c.ret(url_object(c, requests().get(c.arg(0))->url)); });
    o.method("NSURLRequest", "HTTPMethod", [](Cpu& c) { c.ret(string_autoreleased(c, requests().get(c.arg(0))->method)); });
    o.method("NSURLRequest", "HTTPBody", [](Cpu& c) {
        auto r = requests().get(c.arg(0));
        c.ret(r->body.empty() ? 0 : make_data(c, r->body));
    });
    o.method("NSURLRequest", "HTTPBodyStream", [](Cpu& c) { c.ret(0); });
    o.method("NSURLRequest", "allHTTPHeaderFields", [](Cpu& c) { c.ret(headers_to_dict(c, requests().get(c.arg(0))->headers)); });
    o.method("NSURLRequest", "valueForHTTPHeaderField:", [](Cpu& c) {
        auto* v = find_header(requests().get(c.arg(0))->headers, string_arg(c, c.arg(2)));
        c.ret(v ? string_autoreleased(c, *v) : 0);
    });
    o.method("NSURLRequest", "timeoutInterval", [](Cpu& c) { c.set_d(0, requests().get(c.arg(0))->timeout); });
    o.method("NSURLRequest", "cachePolicy", [](Cpu& c) { c.ret(requests().get(c.arg(0))->cache_policy); });
    o.method("NSURLRequest", "HTTPShouldHandleCookies", [](Cpu& c) { c.ret(1); });
    o.method("NSURLRequest", "allowsCellularAccess", [](Cpu& c) { c.ret(1); });
    o.method("NSURLRequest", "networkServiceType", [](Cpu& c) { c.ret(0); });
    o.method("NSURLRequest", "mainDocumentURL", [](Cpu& c) { c.ret(0); });

    o.method("NSMutableURLRequest", "setURL:", [](Cpu& c) { requests().get(c.arg(0))->url = url_text(c, c.arg(2)); });
    o.method("NSMutableURLRequest", "setHTTPMethod:", [](Cpu& c) { requests().get(c.arg(0))->method = string_arg(c, c.arg(2)); });
    o.method("NSMutableURLRequest", "setHTTPBody:", [](Cpu& c) { requests().get(c.arg(0))->body = data_bytes(c, c.arg(2)); });
    o.method("NSMutableURLRequest", "setValue:forHTTPHeaderField:", [](Cpu& c) {
        auto r = requests().get(c.arg(0));
        std::string name = string_arg(c, c.arg(3));
        if (!c.arg(2))
        {
            std::erase_if(r->headers, [&](auto& h) { return same_header(h.first, name); });
            return;
        }
        set_header(r->headers, name, string_arg(c, c.arg(2)), false);
    });
    o.method("NSMutableURLRequest", "addValue:forHTTPHeaderField:", [](Cpu& c) {
        set_header(requests().get(c.arg(0))->headers, string_arg(c, c.arg(3)), string_arg(c, c.arg(2)), true);
    });
    o.method("NSMutableURLRequest",
             "setAllHTTPHeaderFields:", [](Cpu& c) { requests().get(c.arg(0))->headers = headers_from_dict(c, c.arg(2)); });
    o.method("NSMutableURLRequest", "setTimeoutInterval:", [](Cpu& c) { requests().get(c.arg(0))->timeout = c.d(0); });
    o.method("NSMutableURLRequest", "setCachePolicy:", [](Cpu& c) { requests().get(c.arg(0))->cache_policy = c.arg(2); });
    for (const char* sel : {"setHTTPShouldHandleCookies:", "setHTTPBodyStream:", "setAllowsCellularAccess:", "setNetworkServiceType:",
                            "setMainDocumentURL:", "setHTTPShouldUsePipelining:", "setAllowsExpensiveNetworkAccess:",
                            "setAllowsConstrainedNetworkAccess:", "setAssumesHTTP3Capable:", "setAttribution:"})
        o.method("NSMutableURLRequest", sel, [](Cpu& c) {});
}

void register_responses(objc::ObjcRuntime& o)
{
    o.method("NSHTTPURLResponse", "initWithURL:statusCode:HTTPVersion:headerFields:", [](Cpu& c) {
        auto r = responses().get(c.arg(0));
        r->url = url_text(c, c.arg(2));
        r->status = int64_t(c.arg(3));
        r->headers = headers_from_dict(c, c.arg(5));
        c.ret(c.arg(0));
    });
    o.method("NSURLResponse", "initWithURL:MIMEType:expectedContentLength:textEncodingName:", [](Cpu& c) {
        auto r = responses().get(c.arg(0));
        r->url = url_text(c, c.arg(2));
        r->mime = string_arg(c, c.arg(3));
        r->expected = int64_t(c.arg(4));
        c.ret(c.arg(0));
    });
    o.method("NSURLResponse", "dealloc", [](Cpu& c) {
        responses().erase(c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    o.method("NSURLResponse", "URL", [](Cpu& c) { c.ret(url_object(c, responses().get(c.arg(0))->url)); });
    o.method("NSURLResponse", "MIMEType", [](Cpu& c) {
        auto r = responses().get(c.arg(0));
        c.ret(r->mime.empty() ? 0 : string_autoreleased(c, r->mime));
    });
    o.method("NSURLResponse", "expectedContentLength", [](Cpu& c) { c.ret(uint64_t(responses().get(c.arg(0))->expected)); });
    o.method("NSURLResponse", "textEncodingName", [](Cpu& c) { c.ret(0); });
    o.method("NSURLResponse", "suggestedFilename", [](Cpu& c) {
        std::string url = responses().get(c.arg(0))->url;
        url = url.substr(0, url.find_first_of("?#"));
        std::string name = url.substr(url.find_last_of('/') + 1);
        c.ret(string_autoreleased(c, name.empty() ? "Unknown" : name));
    });
    o.method("NSURLResponse", "copyWithZone:", [](Cpu& c) { c.ret(objc(c).retain(c.arg(0))); });
    o.method("NSHTTPURLResponse", "statusCode", [](Cpu& c) { c.ret(uint64_t(responses().get(c.arg(0))->status)); });
    o.method("NSHTTPURLResponse", "allHeaderFields", [](Cpu& c) { c.ret(headers_to_dict(c, responses().get(c.arg(0))->headers)); });
    o.method("NSHTTPURLResponse", "valueForHTTPHeaderField:", [](Cpu& c) {
        auto* v = find_header(responses().get(c.arg(0))->headers, string_arg(c, c.arg(2)));
        c.ret(v ? string_autoreleased(c, *v) : 0);
    });
    o.class_method("NSHTTPURLResponse", "localizedStringForStatusCode:", [](Cpu& c) { c.ret(string_autoreleased(c, "status")); });
}

void register_sessions(objc::ObjcRuntime& o)
{
    auto config = [](Cpu& c) {
        Id cfg = make(c, "NSURLSessionConfiguration");
        configs().get(cfg);
        c.ret(objc(c).autorelease(c, cfg));
    };
    static decltype(config) s_config = config;
    o.class_method("NSURLSessionConfiguration", "defaultSessionConfiguration", [](Cpu& c) { s_config(c); });
    o.class_method("NSURLSessionConfiguration", "ephemeralSessionConfiguration", [](Cpu& c) { s_config(c); });
    o.class_method("NSURLSessionConfiguration", "backgroundSessionConfigurationWithIdentifier:", [](Cpu& c) { s_config(c); });
    o.method("NSURLSessionConfiguration", "dealloc", [](Cpu& c) {
        configs().erase(c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    o.method("NSURLSessionConfiguration", "copyWithZone:", [](Cpu& c) {
        Id cfg = make(c, "NSURLSessionConfiguration");
        *configs().get(cfg) = *configs().get(c.arg(0));
        c.ret(cfg);
    });
    o.method("NSURLSessionConfiguration", "HTTPAdditionalHeaders",
             [](Cpu& c) { c.ret(headers_to_dict(c, configs().get(c.arg(0))->headers)); });
    o.method("NSURLSessionConfiguration",
             "setHTTPAdditionalHeaders:", [](Cpu& c) { configs().get(c.arg(0))->headers = headers_from_dict(c, c.arg(2)); });
    o.method("NSURLSessionConfiguration", "timeoutIntervalForRequest", [](Cpu& c) { c.set_d(0, configs().get(c.arg(0))->timeout); });
    o.method("NSURLSessionConfiguration", "setTimeoutIntervalForRequest:", [](Cpu& c) {
        if (c.d(0) > 0) configs().get(c.arg(0))->timeout = c.d(0);
    });
    o.method("NSURLSessionConfiguration", "timeoutIntervalForResource", [](Cpu& c) { c.set_d(0, 604800); });
    o.method("NSURLSessionConfiguration", "HTTPMaximumConnectionsPerHost", [](Cpu& c) { c.ret(6); });
    o.method("NSURLSessionConfiguration", "allowsCellularAccess", [](Cpu& c) { c.ret(1); });
    o.method("NSURLSessionConfiguration", "HTTPShouldSetCookies", [](Cpu& c) { c.ret(1); });
    o.method("NSURLSessionConfiguration", "requestCachePolicy", [](Cpu& c) { c.ret(0); });
    o.method("NSURLSessionConfiguration", "protocolClasses", [](Cpu& c) { c.ret(make_array(c, {})); });
    o.method("NSURLSessionConfiguration", "identifier", [](Cpu& c) { c.ret(0); });

    o.class_method("NSURLSession", "sharedSession", [](Cpu& c) {
        static Id shared = new_session(c, 0, 0, 0);
        c.ret(shared);
    });
    o.class_method("NSURLSession",
                   "sessionWithConfiguration:", [](Cpu& c) { c.ret(objc(c).autorelease(c, new_session(c, c.arg(2), 0, 0))); });
    o.class_method("NSURLSession", "sessionWithConfiguration:delegate:delegateQueue:", [](Cpu& c) {
        c.ret(objc(c).autorelease(c, new_session(c, c.arg(2), c.arg(3), c.arg(4))));
    });
    o.method("NSURLSession", "delegate", [](Cpu& c) { c.ret(sessions().get(c.arg(0))->delegate); });
    o.method("NSURLSession", "configuration", [](Cpu& c) {
        Id cfg = make(c, "NSURLSessionConfiguration");
        configs().get(cfg)->headers = sessions().get(c.arg(0))->config.headers;
        c.ret(objc(c).autorelease(c, cfg));
    });
    o.method("NSURLSession", "delegateQueue", [](Cpu& c) { c.ret(0); });
    o.method("NSURLSession", "dataTaskWithRequest:", [](Cpu& c) { c.ret(new_task(c, c.arg(0), c.arg(2), Kind::Data, 0)); });
    o.method("NSURLSession",
             "dataTaskWithRequest:completionHandler:", [](Cpu& c) { c.ret(new_task(c, c.arg(0), c.arg(2), Kind::Data, c.arg(3))); });
    o.method("NSURLSession",
             "dataTaskWithRequest:delegate:", [](Cpu& c) { c.ret(new_task(c, c.arg(0), c.arg(2), Kind::Data, 0, c.arg(3))); });
    o.method("NSURLSession", "dataTaskWithURL:", [](Cpu& c) { c.ret(new_task(c, c.arg(0), request_for_url(c, c.arg(2)), Kind::Data, 0)); });
    o.method("NSURLSession", "dataTaskWithURL:completionHandler:", [](Cpu& c) {
        c.ret(new_task(c, c.arg(0), request_for_url(c, c.arg(2)), Kind::Data, c.arg(3)));
    });
    auto upload = [](Cpu& c, Id handler) {
        Id task = new_task(c, c.arg(0), c.arg(2), Kind::Upload, handler);
        tasks().get(task)->request.body = data_bytes(c, c.arg(3));
        c.ret(task);
    };
    static decltype(upload) s_upload = upload;
    o.method("NSURLSession", "uploadTaskWithRequest:fromData:", [](Cpu& c) { s_upload(c, 0); });
    o.method("NSURLSession", "uploadTaskWithRequest:fromData:completionHandler:", [](Cpu& c) { s_upload(c, c.arg(4)); });
    o.method("NSURLSession", "downloadTaskWithRequest:", [](Cpu& c) { c.ret(new_task(c, c.arg(0), c.arg(2), Kind::Download, 0)); });
    o.method("NSURLSession", "downloadTaskWithRequest:completionHandler:", [](Cpu& c) {
        c.ret(new_task(c, c.arg(0), c.arg(2), Kind::Download, c.arg(3)));
    });
    o.method("NSURLSession",
             "downloadTaskWithURL:", [](Cpu& c) { c.ret(new_task(c, c.arg(0), request_for_url(c, c.arg(2)), Kind::Download, 0)); });
    o.method("NSURLSession", "downloadTaskWithURL:completionHandler:", [](Cpu& c) {
        c.ret(new_task(c, c.arg(0), request_for_url(c, c.arg(2)), Kind::Download, c.arg(3)));
    });
    o.method("NSURLSession", "finishTasksAndInvalidate", [](Cpu& c) {});
    o.method("NSURLSession", "invalidateAndCancel", [](Cpu& c) {});
    o.method("NSURLSession", "getTasksWithCompletionHandler:", [](Cpu& c) {
        call_block(c, c.arg(2), {make_array(c, {}), make_array(c, {}), make_array(c, {})});
    });
    o.method("NSURLSession", "getAllTasksWithCompletionHandler:", [](Cpu& c) { call_block(c, c.arg(2), {make_array(c, {})}); });

    o.method("NSURLSessionTask", "dealloc", [](Cpu& c) {
        if (auto t = tasks().find(c.arg(0)))
        {
            for (Id obj : {t->session, t->delegate, t->original_request, t->response, t->error})
                if (obj) objc(c).release(c, obj);
            if (t->handler) block_release(c, t->handler);
        }
        tasks().erase(c.arg(0));
        objc(c).dispose(c.arg(0));
    });
    o.method("NSURLSessionTask", "resume", [](Cpu& c) { resume(c, c.arg(0)); });
    o.method("NSURLSessionTask", "suspend", [](Cpu& c) {});
    o.method("NSURLSessionTask", "cancel", [](Cpu& c) {
        auto t = tasks().get(c.arg(0));
        t->cancelled = true;
        if (t->state == 1)
        {
            t->state = 0;
            objc(c).retain(c.arg(0));
            t->error_code = -999;
            auto s = sessions().find(t->session);
            finish_on_queue(c, c.arg(0), s ? s->queue : 0);
        }
    });
    o.method("NSURLSessionTask", "state", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->state.load()); });
    o.method("NSURLSessionTask", "taskIdentifier", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->ident); });
    o.method("NSURLSessionTask", "originalRequest", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->original_request); });
    o.method("NSURLSessionTask", "currentRequest", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->original_request); });
    o.method("NSURLSessionTask", "response", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->response); });
    o.method("NSURLSessionTask", "error", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->error); });
    o.method("NSURLSessionTask", "delegate", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->delegate); });
    o.method("NSURLSessionTask", "setDelegate:", [](Cpu& c) {
        auto t = tasks().get(c.arg(0));
        if (t->delegate) objc(c).release(c, t->delegate);
        t->delegate = c.arg(2) ? objc(c).retain(c.arg(2)) : 0;
    });
    o.method("NSURLSessionTask", "taskDescription", [](Cpu& c) {
        auto t = tasks().get(c.arg(0));
        c.ret(t->description.empty() ? 0 : string_autoreleased(c, t->description));
    });
    o.method("NSURLSessionTask", "setTaskDescription:", [](Cpu& c) { tasks().get(c.arg(0))->description = string_arg(c, c.arg(2)); });
    o.method("NSURLSessionTask", "countOfBytesReceived", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->data.size()); });
    o.method("NSURLSessionTask", "countOfBytesExpectedToReceive", [](Cpu& c) {
        auto r = responses().find(tasks().get(c.arg(0))->response);
        c.ret(uint64_t(r ? r->expected : -1));
    });
    o.method("NSURLSessionTask", "countOfBytesSent", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->request.body.size()); });
    o.method("NSURLSessionTask", "countOfBytesExpectedToSend", [](Cpu& c) { c.ret(tasks().get(c.arg(0))->request.body.size()); });
    o.method("NSURLSessionTask", "priority", [](Cpu& c) { c.set_s(0, 0.5f); });
    o.method("NSURLSessionTask", "progress", [](Cpu& c) { c.ret(0); });
    for (const char* sel : {"setPriority:", "setEarliestBeginDate:", "setCountOfBytesClientExpectsToSend:",
                            "setCountOfBytesClientExpectsToReceive:", "setPrefersIncrementalDelivery:"})
        o.method("NSURLSessionTask", sel, [](Cpu& c) {});
}
}

void register_url_session(objc::ObjcRuntime& o)
{
    register_requests(o);
    register_responses(o);
    register_sessions(o);
    auto string_var = [](Runtime& rt, const char16_t* value) {
        GuestAddr var = rt.mem.alloc_system(8, 8);
        rt.mem.write<uint64_t>(var, new_string(rt, value));
        return var;
    };
    static decltype(string_var) s_var = string_var;
    Hle& h = o.rt.hle;
    h.data("_NSURLErrorDomain", [](Runtime& rt) { return s_var(rt, u"NSURLErrorDomain"); });
    h.data("_NSURLErrorFailingURLErrorKey", [](Runtime& rt) { return s_var(rt, u"NSErrorFailingURLKey"); });
    h.data("_NSURLErrorFailingURLStringErrorKey", [](Runtime& rt) { return s_var(rt, u"NSErrorFailingURLStringKey"); });
    h.data("_NSURLSessionTransferSizeUnknown", [](Runtime& rt) {
        GuestAddr var = rt.mem.alloc_system(8, 8);
        rt.mem.write<int64_t>(var, -1);
        return var;
    });
}

}
