#include "host/launcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace orchard
{
namespace
{
std::wstring widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

fs::path apps_cache()
{
    return local_cache_dir("apps");
}

std::string stamp_of(const fs::path& ipa)
{
    auto size = fs::file_size(ipa);
    auto time = fs::last_write_time(ipa).time_since_epoch().count();
    return std::to_string(size) + ":" + std::to_string(time);
}

fs::path find_app(const fs::path& dir)
{
    fs::path payload = dir / "Payload";
    if (fs::is_directory(payload))
        for (auto& e : fs::directory_iterator(payload))
            if (e.is_directory() && e.path().extension() == ".app") return e.path();
    return {};
}

void run_tar(const fs::path& ipa, const fs::path& dest)
{
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + ipa.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("could not run the Windows tar tool to unpack the IPA");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) throw std::runtime_error("unpacking the IPA failed (is it a valid, decrypted IPA?)");
}

bool set_value(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value, DWORD type = REG_SZ)
{
    HKEY k;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LSTATUS s = RegSetValueExW(k, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return s == ERROR_SUCCESS;
}
}

bool g_dialogs = true;

void set_dialogs_enabled(bool enabled)
{
    g_dialogs = enabled;
}

fs::path local_cache_dir(const std::string& name)
{
    PWSTR local = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) base = local;
    CoTaskMemFree(local);
    if (base.empty()) base = fs::temp_directory_path();
    return base / "Orchard" / name;
}

void show_error(const std::string& message)
{
    std::fprintf(stderr, "%s\n", message.c_str());
    if (g_dialogs) MessageBoxW(nullptr, widen(message).c_str(), L"Orchard", MB_OK | MB_ICONERROR);
}

fs::path prepare_app(const fs::path& input)
{
    if (fs::is_directory(input)) return input;
    std::string ext = input.extension().string();
    for (auto& ch : ext)
        ch = char(std::tolower(uint8_t(ch)));
    if (ext != ".ipa") throw std::runtime_error("expected an .ipa file or an unpacked .app folder: " + input.string());
    if (!fs::exists(input)) throw std::runtime_error("file not found: " + input.string());

    fs::path dest = apps_cache() / input.stem();
    fs::path marker = dest / ".orchard-unpacked";
    std::string stamp = stamp_of(input);
    std::string have;
    if (std::ifstream m{marker}) std::getline(m, have);
    if (have != stamp || find_app(dest).empty())
    {
        std::printf("unpacking %s to %s ...\n", input.filename().string().c_str(), dest.string().c_str());
        std::error_code ec;
        fs::remove_all(dest, ec);
        fs::create_directories(dest);
        run_tar(input, dest);
        std::ofstream(marker) << stamp << "\n";
    }
    fs::path app = find_app(dest);
    if (app.empty()) throw std::runtime_error("the IPA has no Payload/*.app inside");
    return app;
}

bool register_ipa_association(bool remove)
{
    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring progid = L"Orchard.ipa";
    bool ok = true;
    if (remove)
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, (classes + progid).c_str());
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (classes + L".ipa\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
        {
            RegDeleteValueW(k, progid.c_str());
            RegCloseKey(k);
        }
    }
    else
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        ok &= set_value(HKEY_CURRENT_USER, classes + progid, nullptr, L"iOS app (IPA)");
        ok &= set_value(HKEY_CURRENT_USER, classes + progid + L"\\DefaultIcon", nullptr, std::wstring(exe) + L",0");
        ok &= set_value(HKEY_CURRENT_USER, classes + progid + L"\\shell\\open\\command", nullptr, L"\"" + std::wstring(exe) + L"\" \"%1\"");
        ok &= set_value(HKEY_CURRENT_USER, classes + L".ipa\\OpenWithProgids", progid.c_str(), L"", REG_NONE);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

}
