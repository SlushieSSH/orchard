#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_map>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include "core/runtime.h"
#include "core/threads.h"
#include "cpu/cpu.h"
#include "frameworks/libSystem/errno.h"
#include "frameworks/libSystem/mach_time.h"
#include "hle/hle.h"

namespace fs = std::filesystem;

namespace orchard
{
namespace
{
constexpr uint64_t O_WRONLY_ = 1, O_RDWR_ = 2, O_NONBLOCK_ = 4, O_APPEND_ = 8, O_CREAT_ = 0x200, O_TRUNC_ = 0x400, O_EXCL_ = 0x800,
                   O_DIRECTORY_ = 0x100000;
constexpr uint16_t S_IFDIR_ = 0040000, S_IFREG_ = 0100000, S_IFCHR_ = 0020000;

struct Fd
{
    int host = -1;
    std::string guest_path;
    fs::path host_path;
    bool dir = false;
    std::vector<std::string> dir_entries;
    size_t dir_pos = 0;
    uint64_t status_flags = 0;
};

std::mutex fds_lock;
std::unordered_map<int, Fd> fds;
int next_fd = 3;

uint64_t fail(Cpu& c, int err)
{
    c.mem.write<int32_t>(c.pthread + Threads::kErrnoOffset, err);
    return uint64_t(-1);
}

std::string guest_path(Cpu& c, GuestAddr p)
{
    return p ? c.mem.read_cstr(p) : "";
}

std::optional<fs::path> host_of(Cpu& c, const std::string& path)
{
    return c.rt.vfs.to_host(path);
}

void write_timespec(Cpu& c, GuestAddr at, fs::file_time_type t)
{
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
    c.mem.write<int64_t>(at, ns / 1'000'000'000);
    c.mem.write<int64_t>(at + 8, ns % 1'000'000'000);
}

bool fill_stat(Cpu& c, const fs::path& p, GuestAddr st)
{
    std::error_code ec;
    auto status = fs::status(p, ec);
    if (ec || !fs::exists(status)) return false;
    std::memset(c.mem.host(st), 0, 144);
    bool dir = fs::is_directory(status);
    c.mem.write<int32_t>(st, 0x1000002);
    c.mem.write<uint16_t>(st + 4, uint16_t((dir ? S_IFDIR_ : S_IFREG_) | 0755));
    c.mem.write<uint16_t>(st + 6, 1);
    c.mem.write<uint64_t>(st + 8, std::hash<std::wstring>{}(p.native()));
    c.mem.write<uint32_t>(st + 16, 501);
    c.mem.write<uint32_t>(st + 20, 501);
    auto mtime = fs::last_write_time(p, ec);
    for (uint64_t off : {32, 48, 64, 80})
        write_timespec(c, st + off, mtime);
    uint64_t size = dir ? 64 : fs::file_size(p, ec);
    c.mem.write<int64_t>(st + 96, int64_t(size));
    c.mem.write<int64_t>(st + 104, int64_t((size + 511) / 512));
    c.mem.write<int32_t>(st + 112, 16384);
    return true;
}

void fill_stat_tty(Cpu& c, GuestAddr st)
{
    std::memset(c.mem.host(st), 0, 144);
    c.mem.write<uint16_t>(st + 4, uint16_t(S_IFCHR_ | 0620));
    c.mem.write<int32_t>(st + 112, 4096);
}

void do_open(Cpu& c, const std::string& raw, uint64_t flags)
{
    std::string path = Vfs::normalize(raw, c.rt.vfs.cwd);
    auto host = host_of(c, path);
    if (!host) return c.ret(fail(c, ENOENT_));
    std::error_code ec;
    bool exists = fs::exists(*host, ec);
    if (exists && (flags & O_CREAT_) && (flags & O_EXCL_)) return c.ret(fail(c, EEXIST_));
    if (!exists && !(flags & O_CREAT_)) return c.ret(fail(c, ENOENT_));
    Fd fd;
    fd.guest_path = path;
    fd.host_path = *host;
    fd.status_flags = flags;
    if (exists && fs::is_directory(*host, ec))
    {
        fd.dir = true;
    }
    else
    {
        if (flags & O_DIRECTORY_) return c.ret(fail(c, ENOTDIR_));
        int mode = _O_BINARY | _O_NOINHERIT;
        uint64_t acc = flags & 3;
        mode |= acc == O_WRONLY_ ? _O_WRONLY : acc == O_RDWR_ ? _O_RDWR : _O_RDONLY;
        if (flags & O_CREAT_) mode |= _O_CREAT;
        if (flags & O_TRUNC_) mode |= _O_TRUNC;
        if (flags & O_APPEND_) mode |= _O_APPEND;
        if (flags & O_CREAT_) fs::create_directories(host->parent_path(), ec);
        fd.host = _wopen(host->c_str(), mode, _S_IREAD | _S_IWRITE);
        if (fd.host < 0) return c.ret(fail(c, darwin_errno(errno)));
    }
    std::lock_guard g(fds_lock);
    int n = next_fd++;
    fds[n] = std::move(fd);
    c.ret(uint64_t(n));
}

Fd* fd_of(int n)
{
    auto it = fds.find(n);
    return it == fds.end() ? nullptr : &it->second;
}

void do_read(Cpu& c, int n, GuestAddr buf, uint64_t len, int64_t offset)
{
    if (n == 0) return c.ret(0);
    std::lock_guard g(fds_lock);
    Fd* fd = fd_of(n);
    if (!fd || fd->host < 0) return c.ret(fail(c, fd && fd->dir ? EISDIR_ : EBADF_));
    if (offset >= 0 && _lseeki64(fd->host, offset, SEEK_SET) < 0) return c.ret(fail(c, EINVAL_));
    std::vector<uint8_t> tmp(len);
    int got = _read(fd->host, tmp.data(), unsigned(std::min<uint64_t>(len, 0x7fffffff)));
    if (got < 0) return c.ret(fail(c, darwin_errno(errno)));
    if (got) c.mem.write_bytes(buf, tmp.data(), uint64_t(got));
    c.ret(uint64_t(got));
}

void do_write(Cpu& c, int n, GuestAddr buf, uint64_t len, int64_t offset)
{
    std::vector<char> tmp(len);
    if (len) c.mem.read_bytes(buf, tmp.data(), len);
    if (n == 1 || n == 2)
    {
        std::fwrite(tmp.data(), 1, len, n == 1 ? stdout : stderr);
        std::fflush(n == 1 ? stdout : stderr);
        return c.ret(len);
    }
    std::lock_guard g(fds_lock);
    Fd* fd = fd_of(n);
    if (!fd || fd->host < 0) return c.ret(fail(c, EBADF_));
    if (offset >= 0 && _lseeki64(fd->host, offset, SEEK_SET) < 0) return c.ret(fail(c, EINVAL_));
    int put = _write(fd->host, tmp.data(), unsigned(len));
    if (put < 0) return c.ret(fail(c, darwin_errno(errno)));
    c.ret(uint64_t(put));
}

void register_files(Hle& h)
{
    auto open = [](Cpu& c) { do_open(c, guest_path(c, c.arg(0)), c.arg(1)); };
    for (const char* n : {"_open", "___open", "_open$NOCANCEL", "___open_nocancel", "_open_dprotected_np"})
        h.fn(n, open);
    h.fn("_openat", [](Cpu& c) { do_open(c, guest_path(c, c.arg(1)), c.arg(2)); });
    auto close = [](Cpu& c) {
        std::lock_guard g(fds_lock);
        Fd* fd = fd_of(int(c.arg(0)));
        if (!fd) return c.ret(int(c.arg(0)) < 3 ? 0 : fail(c, EBADF_));
        if (fd->host >= 0) _close(fd->host);
        fds.erase(int(c.arg(0)));
        c.ret(0);
    };
    for (const char* n : {"_close", "___close_nocancel", "_close$NOCANCEL"})
        h.fn(n, close);
    auto read = [](Cpu& c) { do_read(c, int(c.arg(0)), c.arg(1), c.arg(2), -1); };
    for (const char* n : {"_read", "___read_nocancel", "_read$NOCANCEL"})
        h.fn(n, read);
    auto write = [](Cpu& c) { do_write(c, int(c.arg(0)), c.arg(1), c.arg(2), -1); };
    for (const char* n : {"_write", "___write_nocancel", "_write$NOCANCEL"})
        h.fn(n, write);
    for (const char* n : {"_pread", "___pread_nocancel"})
        h.fn(n, [](Cpu& c) { do_read(c, int(c.arg(0)), c.arg(1), c.arg(2), int64_t(c.arg(3))); });
    for (const char* n : {"_pwrite", "___pwrite_nocancel"})
        h.fn(n, [](Cpu& c) { do_write(c, int(c.arg(0)), c.arg(1), c.arg(2), int64_t(c.arg(3))); });
    h.fn("_fcopyfile", [](Cpu& c) {
        std::lock_guard g(fds_lock);
        Fd* from = fd_of(int(c.arg(0)));
        Fd* to = fd_of(int(c.arg(1)));
        if (!from || !to || from->host < 0 || to->host < 0) return c.ret(fail(c, EBADF_));
        _lseeki64(from->host, 0, SEEK_SET);
        std::vector<char> buf(1 << 20);
        for (int got; (got = _read(from->host, buf.data(), unsigned(buf.size()))) > 0;)
            if (_write(to->host, buf.data(), unsigned(got)) != got) return c.ret(fail(c, darwin_errno(errno)));
        c.ret(0);
    });
    h.fn("_copyfile", [](Cpu& c) {
        auto from = host_of(c, guest_path(c, c.arg(0)));
        auto to = host_of(c, guest_path(c, c.arg(1)));
        std::error_code ec;
        if (!from || !to || !fs::copy_file(*from, *to, fs::copy_options::overwrite_existing, ec)) return c.ret(fail(c, ENOENT_));
        c.ret(0);
    });
    h.fn("_lseek", [](Cpu& c) {
        std::lock_guard g(fds_lock);
        Fd* fd = fd_of(int(c.arg(0)));
        if (fd && fd->dir && c.arg(2) == SEEK_SET)
        {
            fd->dir_pos = c.arg(1);
            if (!fd->dir_pos) fd->dir_entries.clear();
            return c.ret(c.arg(1));
        }
        if (!fd || fd->host < 0) return c.ret(fail(c, EBADF_));
        int64_t r = _lseeki64(fd->host, int64_t(c.arg(1)), int(c.arg(2)));
        c.ret(r < 0 ? fail(c, EINVAL_) : uint64_t(r));
    });
    auto stat = [](Cpu& c) {
        auto host = host_of(c, guest_path(c, c.arg(0)));
        if (!host || !fill_stat(c, *host, c.arg(1))) return c.ret(fail(c, ENOENT_));
        c.ret(0);
    };
    for (const char* n : {"_stat", "_stat64", "_lstat", "_lstat64"})
        h.fn(n, stat);
    h.fn("_fstatat", [](Cpu& c) {
        auto host = host_of(c, guest_path(c, c.arg(1)));
        if (!host || !fill_stat(c, *host, c.arg(2))) return c.ret(fail(c, ENOENT_));
        c.ret(0);
    });
    auto fstat = [](Cpu& c) {
        int n = int(c.arg(0));
        if (n < 3)
        {
            fill_stat_tty(c, c.arg(1));
            return c.ret(0);
        }
        fs::path p;
        {
            std::lock_guard g(fds_lock);
            Fd* fd = fd_of(n);
            if (!fd) return c.ret(fail(c, EBADF_));
            p = fd->host_path;
        }
        if (!fill_stat(c, p, c.arg(1))) return c.ret(fail(c, EBADF_));
        c.ret(0);
    };
    for (const char* n : {"_fstat", "_fstat64"})
        h.fn(n, fstat);
    auto statfs = [](Cpu& c) {
        GuestAddr s = c.arg(1);
        std::memset(c.mem.host(s), 0, 2168);
        c.mem.write<uint32_t>(s + 0, 4096);
        c.mem.write<int32_t>(s + 4, 1 << 20);
        c.mem.write<uint64_t>(s + 8, 16ull << 20);
        c.mem.write<uint64_t>(s + 16, 8ull << 20);
        c.mem.write<uint64_t>(s + 24, 8ull << 20);
        c.mem.write<uint64_t>(s + 32, 1ull << 30);
        c.mem.write<uint64_t>(s + 40, 1ull << 29);
        c.mem.write<uint32_t>(s + 60, 0x1a);
        c.mem.write<uint32_t>(s + 64, 0x04801000);
        c.mem.write_bytes(s + 72, "apfs", 5);
        c.mem.write_bytes(s + 88, "/private/var", 13);
        c.mem.write_bytes(s + 1112, "/dev/disk0s1s2", 15);
        c.ret(0);
    };
    for (const char* n : {"_fstatfs", "_fstatfs64", "_statfs", "_statfs64"})
        h.fn(n, statfs);
    auto access = [](Cpu& c) {
        auto host = host_of(c, guest_path(c, c.arg(0)));
        std::error_code ec;
        c.ret(host && fs::exists(*host, ec) ? 0 : fail(c, ENOENT_));
    };
    h.fn("_access", access);
    h.fn("_faccessat", [](Cpu& c) {
        auto host = host_of(c, guest_path(c, c.arg(1)));
        std::error_code ec;
        c.ret(host && fs::exists(*host, ec) ? 0 : fail(c, ENOENT_));
    });
    h.fn("_mkdir", [](Cpu& c) {
        auto host = host_of(c, guest_path(c, c.arg(0)));
        std::error_code ec;
        if (!host) return c.ret(fail(c, EACCES_));
        if (fs::exists(*host, ec)) return c.ret(fail(c, EEXIST_));
        c.ret(fs::create_directory(*host, ec) ? 0 : fail(c, ENOENT_));
    });
    h.fn("_rmdir", [](Cpu& c) {
        auto host = host_of(c, guest_path(c, c.arg(0)));
        std::error_code ec;
        c.ret(host && fs::remove(*host, ec) ? 0 : fail(c, ENOENT_));
    });
    h.fn("_unlink", [](Cpu& c) {
        auto host = host_of(c, guest_path(c, c.arg(0)));
        std::error_code ec;
        c.ret(host && fs::remove(*host, ec) ? 0 : fail(c, ENOENT_));
    });
    h.fn("_rename", [](Cpu& c) {
        auto a = host_of(c, guest_path(c, c.arg(0))), b = host_of(c, guest_path(c, c.arg(1)));
        std::error_code ec;
        if (a && b) fs::rename(*a, *b, ec);
        c.ret(a && b && !ec ? 0 : fail(c, ENOENT_));
    });
    h.fn("_ftruncate", [](Cpu& c) {
        std::lock_guard g(fds_lock);
        Fd* fd = fd_of(int(c.arg(0)));
        if (!fd || fd->host < 0) return c.ret(fail(c, EBADF_));
        c.ret(_chsize_s(fd->host, int64_t(c.arg(1))) == 0 ? 0 : fail(c, EIO_));
    });
    h.fn("_fsync", [](Cpu& c) { c.ret(0); });
    h.fn("_flock", [](Cpu& c) { c.ret(0); });
    h.fn("_isatty", [](Cpu& c) { c.ret(0); });
    h.fn("_ioctl", [](Cpu& c) { c.ret(fail(c, ENOTTY_)); });
    auto fcntl = [](Cpu& c) {
        int cmd = int(c.arg(1));
        std::lock_guard g(fds_lock);
        Fd* fd = fd_of(int(c.arg(0)));
        if (!fd && int(c.arg(0)) >= 3) return c.ret(fail(c, EBADF_));
        switch (cmd)
        {
        case 3: return c.ret(fd ? fd->status_flags : 2);
        case 50:
            if (fd)
            {
                GuestAddr buf = c.mem.read<uint64_t>(c.sp());
                c.mem.write_bytes(buf, fd->guest_path.c_str(), fd->guest_path.size() + 1);
            }
            return c.ret(0);
        default: return c.ret(0);
        }
    };
    for (const char* n : {"_fcntl", "___fcntl", "___fcntl_nocancel", "_fcntl$NOCANCEL"})
        h.fn(n, fcntl);
    h.fn("_dup", [](Cpu& c) { c.ret(c.arg(0)); });
    h.fn("_dup2", [](Cpu& c) { c.ret(c.arg(1)); });
    h.fn("_getcwd", [](Cpu& c) {
        std::string cwd = c.rt.vfs.cwd;
        if (c.arg(1) < cwd.size() + 1) return c.ret(0);
        c.mem.write_bytes(c.arg(0), cwd.c_str(), cwd.size() + 1);
        c.ret(c.arg(0));
    });
    h.fn("___getcwd", [](Cpu& c) {
        std::string cwd = c.rt.vfs.cwd;
        if (c.arg(1) < cwd.size() + 1) return c.ret(fail(c, ERANGE_));
        c.mem.write_bytes(c.arg(0), cwd.c_str(), cwd.size() + 1);
        c.ret(0);
    });
    h.fn("_chdir", [](Cpu& c) {
        c.rt.vfs.cwd = Vfs::normalize(guest_path(c, c.arg(0)), c.rt.vfs.cwd);
        c.ret(0);
    });
    h.fn("_readlink", [](Cpu& c) { c.ret(fail(c, EINVAL_)); });

    auto getdirentries = [](Cpu& c) {
        std::lock_guard g(fds_lock);
        Fd* fd = fd_of(int(c.arg(0)));
        if (!fd || !fd->dir) return c.ret(fail(c, EBADF_));
        if (fd->dir_pos == 0 && fd->dir_entries.empty())
        {
            fd->dir_entries = {".", ".."};
            std::error_code ec;
            for (auto& e : fs::directory_iterator(fd->host_path, ec))
            {
                auto u8 = e.path().filename().u8string();
                fd->dir_entries.emplace_back(u8.begin(), u8.end());
            }
        }
        GuestAddr buf = c.arg(1);
        uint64_t cap = c.arg(2), used = 0;
        while (fd->dir_pos < fd->dir_entries.size())
        {
            const std::string& name = fd->dir_entries[fd->dir_pos];
            uint64_t reclen = (21 + name.size() + 1 + 7) & ~uint64_t(7);
            if (used + reclen > cap) break;
            GuestAddr e = buf + used;
            std::error_code ec;
            bool dir =
                name == "." || name == ".." || fs::is_directory(fd->host_path / fs::path(std::u8string(name.begin(), name.end())), ec);
            c.mem.write<uint64_t>(e, fd->dir_pos + 2);
            c.mem.write<uint64_t>(e + 8, fd->dir_pos + 1);
            c.mem.write<uint16_t>(e + 16, uint16_t(reclen));
            c.mem.write<uint16_t>(e + 18, uint16_t(name.size()));
            c.mem.write<uint8_t>(e + 20, dir ? 4 : 8);
            c.mem.write_bytes(e + 21, name.c_str(), name.size() + 1);
            used += reclen;
            ++fd->dir_pos;
        }
        if (c.arg(3)) c.mem.write<uint64_t>(c.arg(3), fd->dir_pos);
        c.ret(used);
    };
    h.fn("_getdirentries64", getdirentries);
    h.fn("___getdirentries64", getdirentries);
}

void register_memory_mapping(Hle& h)
{
    constexpr uint64_t MAP_FIXED_ = 0x10, MAP_ANON_ = 0x1000;
    h.fn("_mmap", [](Cpu& c) {
        GuestAddr want = c.arg(0);
        uint64_t len = c.arg(1), flags = c.arg(3), offset = c.arg(5);
        int n = int(c.arg(4));
        GuestAddr addr;
        if ((flags & MAP_FIXED_) && want)
        {
            c.mem.map(want, len, "mmap-fixed");
            addr = want;
        }
        else
        {
            addr = c.mem.map_anywhere(len, (flags & MAP_ANON_) ? "mmap-anon" : "mmap-file");
        }
        if (!(flags & MAP_ANON_) && n >= 0)
        {
            std::lock_guard g(fds_lock);
            Fd* fd = fd_of(n);
            if (!fd || fd->host < 0) return c.ret(fail(c, EBADF_));
            std::ifstream f(fd->host_path, std::ios::binary);
            f.seekg(int64_t(offset));
            std::vector<char> tmp(len);
            f.read(tmp.data(), std::streamsize(len));
            if (f.gcount() > 0) c.mem.write_bytes(addr, tmp.data(), uint64_t(f.gcount()));
        }
        c.ret(addr);
    });
    h.fn("_munmap", [](Cpu& c) {
        c.mem.unmap(c.arg(0), c.arg(1));
        c.ret(0);
    });
    h.fn("_mprotect", [](Cpu& c) { c.ret(0); });
    h.fn("_madvise", [](Cpu& c) {
        constexpr uint64_t MADV_DONTNEED_ = 4, MADV_FREE_ = 5, MADV_FREE_REUSABLE_ = 7;
        uint64_t advice = c.arg(2);
        if (advice == MADV_DONTNEED_ || advice == MADV_FREE_ || advice == MADV_FREE_REUSABLE_) c.mem.discard(c.arg(0), c.arg(1));
        c.ret(0);
    });
    h.fn("_mlock", [](Cpu& c) { c.ret(0); });
    h.fn("_msync", [](Cpu& c) { c.ret(0); });
    h.fn("_getpagesize", [](Cpu& c) { c.ret(layout::kPageSize); });
    h.fn("_sys_icache_invalidate", [](Cpu& c) {});
    h.fn("_sys_dcache_flush", [](Cpu& c) {});
}

void register_time(Hle& h)
{
    h.fn("_mach_absolute_time", [](Cpu& c) { c.ret(mach_now()); });
    h.fn("_mach_continuous_time", [](Cpu& c) { c.ret(mach_now()); });
    h.fn("_mach_approximate_time", [](Cpu& c) { c.ret(mach_now()); });
    h.fn("_mach_timebase_info", [](Cpu& c) {
        c.mem.write<uint32_t>(c.arg(0), 125);
        c.mem.write<uint32_t>(c.arg(0) + 4, 3);
        c.ret(0);
    });
    h.fn("_gettimeofday", [](Cpu& c) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (c.arg(0))
        {
            c.mem.write<int64_t>(c.arg(0), us / 1'000'000);
            c.mem.write<int32_t>(c.arg(0) + 8, int32_t(us % 1'000'000));
        }
        c.ret(0);
    });
    auto nanos = [](uint64_t clock) -> int64_t {
        if (clock == 0)
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    static decltype(nanos) s_nanos = nanos;
    h.fn("_clock_gettime", [](Cpu& c) {
        int64_t ns = s_nanos(c.arg(0));
        c.mem.write<int64_t>(c.arg(1), ns / 1'000'000'000);
        c.mem.write<int64_t>(c.arg(1) + 8, ns % 1'000'000'000);
        c.ret(0);
    });
    h.fn("_clock_gettime_nsec_np", [](Cpu& c) { c.ret(uint64_t(s_nanos(c.arg(0)))); });
}

std::optional<std::string> sysctl_string(const std::string& name)
{
    static const std::unordered_map<std::string, std::string> kStrings = {
        {"hw.machine", "iPhone10,3"},
        {"hw.model", "D22AP"},
        {"kern.osversion", "20H392"},
        {"kern.osrelease", "22.6.0"},
        {"kern.ostype", "Darwin"},
        {"kern.hostname", "iPhone"},
        {"kern.version", "Darwin Kernel Version 22.6.0: orchard"},
        {"hw.product", "iPhone10,3"},
        {"hw.target", "D22AP"},
    };
    auto it = kStrings.find(name);
    return it == kStrings.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::optional<uint64_t> sysctl_int(const std::string& name)
{
    static const std::unordered_map<std::string, uint64_t> kInts = {
        {"hw.ncpu", 6},
        {"hw.activecpu", 6},
        {"hw.physicalcpu", 6},
        {"hw.logicalcpu", 6},
        {"hw.physicalcpu_max", 6},
        {"hw.logicalcpu_max", 6},
        {"hw.perflevel0.physicalcpu", 2},
        {"hw.perflevel1.physicalcpu", 4},
        {"hw.nperflevels", 2},
        {"hw.memsize", 3ull << 30},
        {"hw.pagesize", 16384},
        {"hw.cachelinesize", 64},
        {"hw.l1dcachesize", 65536},
        {"hw.l2cachesize", 8 << 20},
        {"hw.cpufamily", 0xe81e7ef6},
        {"hw.cputype", 0x0100000c},
        {"hw.cpusubtype", 2},
        {"kern.maxfiles", 12288},
        {"kern.argmax", 262144},
        {"hw.optional.neon", 1},
        {"hw.optional.floatingpoint", 1},
        {"hw.optional.armv8_crc32", 1},
        {"hw.optional.arm.FEAT_LSE", 1},
        {"hw.optional.armv8_1_atomics", 1},
        {"kern.hv_vmm_present", 0},
    };
    auto it = kInts.find(name);
    return it == kInts.end() ? std::nullopt : std::optional<uint64_t>(it->second);
}

uint64_t sysctl_reply(Cpu& c, GuestAddr oldp, GuestAddr oldlenp, const void* data, uint64_t size)
{
    if (!oldlenp) return 0;
    uint64_t cap = c.mem.read<uint64_t>(oldlenp);
    c.mem.write<uint64_t>(oldlenp, size);
    if (!oldp) return 0;
    if (cap < size) return fail(c, ENOMEM_);
    c.mem.write_bytes(oldp, data, size);
    return 0;
}

uint64_t sysctl_named(Cpu& c, const std::string& name, GuestAddr oldp, GuestAddr oldlenp)
{
    if (auto s = sysctl_string(name)) return sysctl_reply(c, oldp, oldlenp, s->c_str(), s->size() + 1);
    if (auto v = sysctl_int(name))
    {
        uint64_t cap = oldlenp ? c.mem.read<uint64_t>(oldlenp) : 8;
        if (cap == 4 || name == "hw.ncpu" || name.starts_with("hw.optional") || name.find("cpu") != std::string::npos)
        {
            uint32_t v32 = uint32_t(*v);
            if (cap >= 8 && (name == "hw.memsize")) return sysctl_reply(c, oldp, oldlenp, &*v, 8);
            return sysctl_reply(c, oldp, oldlenp, &v32, 4);
        }
        return sysctl_reply(c, oldp, oldlenp, &*v, 8);
    }
    if (name == "kern.boottime")
    {
        int64_t tv[2] = {
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 3600, 0};
        return sysctl_reply(c, oldp, oldlenp, tv, 16);
    }
    return fail(c, ENOENT_);
}

void register_system_info(Hle& h)
{
    h.fn("_sysctlbyname", [](Cpu& c) { c.ret(sysctl_named(c, c.mem.read_cstr(c.arg(0)), c.arg(1), c.arg(2))); });
    h.fn("_sysctl", [](Cpu& c) {
        GuestAddr mib = c.arg(0);
        uint32_t n = uint32_t(c.arg(1));
        int32_t a = n > 0 ? c.mem.read<int32_t>(mib) : 0, b = n > 1 ? c.mem.read<int32_t>(mib + 4) : 0;
        std::string name;
        if (a == 6)
        {
            switch (b)
            {
            case 1: name = "hw.machine"; break;
            case 2: name = "hw.model"; break;
            case 3: name = "hw.ncpu"; break;
            case 7: name = "hw.pagesize"; break;
            case 24: name = "hw.memsize"; break;
            case 25: name = "hw.activecpu"; break;
            }
        }
        else if (a == 1)
        {
            switch (b)
            {
            case 1: name = "kern.ostype"; break;
            case 2: name = "kern.osrelease"; break;
            case 4: name = "kern.version"; break;
            case 10: name = "kern.hostname"; break;
            case 21: name = "kern.boottime"; break;
            case 65: name = "kern.osversion"; break;
            case 14: {
                std::vector<uint8_t> info(648, 0);
                return c.ret(sysctl_reply(c, c.arg(2), c.arg(3), info.data(), info.size()));
            }
            }
        }
        if (name.empty()) return c.ret(fail(c, ENOENT_));
        c.ret(sysctl_named(c, name, c.arg(2), c.arg(3)));
    });
    h.fn("___error", [](Cpu& c) { c.ret(c.pthread + Threads::kErrnoOffset); });
    h.fn("_getpid", [](Cpu& c) { c.ret(4242); });
    h.fn("_getppid", [](Cpu& c) { c.ret(1); });
    for (const char* n : {"_getuid", "_geteuid", "_getgid", "_getegid"})
        h.fn(n, [](Cpu& c) { c.ret(501); });
    h.fn("_getrlimit", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(1), 1 << 20);
        c.mem.write<uint64_t>(c.arg(1) + 8, 1 << 20);
        c.ret(0);
    });
    h.fn("_setrlimit", [](Cpu& c) { c.ret(0); });
    h.fn("_getrusage", [](Cpu& c) {
        std::memset(c.mem.host(c.arg(1)), 0, 144);
        c.ret(0);
    });
    h.fn("_issetugid", [](Cpu& c) { c.ret(0); });
    h.fn("_kill", [](Cpu& c) { c.ret(0); });
    h.fn("_raise", [](Cpu& c) { c.stop("guest raised signal " + std::to_string(int(c.arg(0)))); });
    h.fn("_signal", [](Cpu& c) { c.ret(0); });
    h.fn("_sigaction", [](Cpu& c) { c.ret(0); });
    auto sigaltstack = [](Cpu& c) {
        if (c.arg(1))
        {
            c.mem.write<uint64_t>(c.arg(1), 0);
            c.mem.write<uint64_t>(c.arg(1) + 8, 0);
            c.mem.write<uint32_t>(c.arg(1) + 16, 4);
        }
        c.ret(0);
    };
    h.fn("_sigaltstack", sigaltstack);
    h.fn("___sigaltstack", sigaltstack);
    auto sigprocmask = [](Cpu& c) {
        if (c.arg(2)) c.mem.write<uint32_t>(c.arg(2), 0);
        c.ret(0);
    };
    h.fn("_sigprocmask", sigprocmask);
    h.fn("___sigprocmask", sigprocmask);
    h.data("_vm_page_size", [](Runtime& rt) {
        GuestAddr a = rt.mem.alloc_system(8, 8);
        rt.mem.write<uint64_t>(a, layout::kPageSize);
        return a;
    });
    h.data("_vm_page_mask", [](Runtime& rt) {
        GuestAddr a = rt.mem.alloc_system(8, 8);
        rt.mem.write<uint64_t>(a, layout::kPageSize - 1);
        return a;
    });
    h.data("_mach_task_self_", [](Runtime& rt) {
        GuestAddr a = rt.mem.alloc_system(8, 8);
        rt.mem.write<uint32_t>(a, 0x103);
        return a;
    });
    h.fn("_mach_host_self", [](Cpu& c) { c.ret(0x203); });
    h.fn("_task_self_trap", [](Cpu& c) { c.ret(0x103); });
    h.fn("_mach_port_deallocate", [](Cpu& c) { c.ret(0); });
    h.fn("_host_page_size", [](Cpu& c) {
        c.mem.write<uint64_t>(c.arg(1), layout::kPageSize);
        c.ret(0);
    });
}

void fill_random(Cpu& c, GuestAddr buf, uint64_t len)
{
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard g(m);
    for (uint64_t i = 0; i < len; i += 8)
    {
        uint64_t v = rng();
        c.mem.write_bytes(buf + i, &v, std::min<uint64_t>(8, len - i));
    }
}

void register_environment(Hle& h)
{
    h.fn("_getenv", [](Cpu& c) {
        std::string name = c.mem.read_cstr(c.arg(0));
        std::lock_guard g(c.rt.env_lock);
        auto it = c.rt.env.find(name);
        if (it == c.rt.env.end()) return c.ret(0);
        GuestAddr& s = c.rt.env_strings[name + "=" + it->second];
        if (!s) s = c.mem.alloc_cstr_region(it->second);
        c.ret(s);
    });
    h.fn("_setenv", [](Cpu& c) {
        std::string name = c.mem.read_cstr(c.arg(0)), value = c.mem.read_cstr(c.arg(1));
        std::lock_guard g(c.rt.env_lock);
        if (c.arg(2) || !c.rt.env.count(name)) c.rt.env[name] = value;
        c.ret(0);
    });
    h.fn("_unsetenv", [](Cpu& c) {
        std::lock_guard g(c.rt.env_lock);
        c.rt.env.erase(c.mem.read_cstr(c.arg(0)));
        c.ret(0);
    });
}

void register_random(Hle& h)
{
    h.fn("_arc4random", [](Cpu& c) {
        uint32_t v;
        GuestAddr tmp = c.sp() - 64;
        fill_random(c, tmp, 4);
        v = c.mem.read<uint32_t>(tmp);
        c.ret(v);
    });
    h.fn("_arc4random_buf", [](Cpu& c) { fill_random(c, c.arg(0), c.arg(1)); });
    h.fn("_arc4random_uniform", [](Cpu& c) {
        uint32_t bound = uint32_t(c.arg(0));
        if (bound < 2) return c.ret(0);
        GuestAddr tmp = c.sp() - 64;
        fill_random(c, tmp, 4);
        c.ret(c.mem.read<uint32_t>(tmp) % bound);
    });
    h.fn("_getentropy", [](Cpu& c) {
        fill_random(c, c.arg(0), c.arg(1));
        c.ret(0);
    });
    h.fn("_CCRandomGenerateBytes", [](Cpu& c) {
        fill_random(c, c.arg(0), c.arg(1));
        c.ret(0);
    });
    h.fn("_SecRandomCopyBytes", [](Cpu& c) {
        fill_random(c, c.arg(2), c.arg(1));
        c.ret(0);
    });
}

}

void map_commpage(Runtime& rt)
{
    GuestAddr p = layout::kCommPage;
    rt.mem.map(p, layout::kPageSize, "commpage");
    rt.mem.write_bytes(p, "commpage 64-bit", 16);
    rt.mem.write<uint16_t>(p + 0x1e, 14);
    rt.mem.write<uint8_t>(p + 0x22, 6);
    rt.mem.write<uint8_t>(p + 0x34, 6);
    rt.mem.write<uint8_t>(p + 0x35, 6);
    rt.mem.write<uint8_t>(p + 0x36, 6);
    rt.mem.write<uint64_t>(p + 0x38, 3ull << 30);
    rt.mem.write<uint32_t>(p + 0x40, 0xe81e7ef6);
}

void register_network(Hle& h)
{
    h.fn("_getaddrinfo", [](Cpu& c) { c.ret(8); });
    h.fn("_freeaddrinfo", [](Cpu& c) {});
    h.fn("_gai_strerror", [](Cpu& c) {
        static GuestAddr msg = [&] {
            const char text[] = "nodename nor servname provided, or not known";
            GuestAddr a = c.rt.mem.alloc_system(sizeof(text), 16);
            c.mem.write_bytes(a, text, sizeof(text));
            return a;
        }();
        c.ret(msg);
    });
}

void register_posix(Hle& h)
{
    register_network(h);
    register_environment(h);
    register_random(h);
    register_files(h);
    register_memory_mapping(h);
    register_time(h);
    register_system_info(h);
}

}
