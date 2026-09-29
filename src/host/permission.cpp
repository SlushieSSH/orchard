#include "host/permission.h"

#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "                   \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace orchard
{
namespace
{
std::mutex g_lock;
std::filesystem::path g_store;
std::string g_bundle_id, g_app_name;
NetworkPolicy g_policy = NetworkPolicy::Ask;
bool g_decided = false, g_allowed = false;

std::wstring widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

void remember(bool allow)
{
    std::vector<std::string> lines;
    {
        std::ifstream in(g_store);
        for (std::string line; std::getline(in, line);)
            if (line.substr(0, line.find(' ')) != g_bundle_id && !line.empty()) lines.push_back(line);
    }
    lines.push_back(g_bundle_id + (allow ? " allow" : " deny"));
    std::ofstream out(g_store, std::ios::trunc);
    for (auto& l : lines)
        out << l << "\n";
}

int ask()
{
    std::wstring text = L"WARNING: " + widen(g_app_name) + L" is attempting to access the internet, allow?";
    TASKDIALOG_BUTTON buttons[] = {{100, L"Yes"}, {101, L"No"}, {102, L"Yes and do not ask again"}, {103, L"No and do not ask again"}};
    TASKDIALOGCONFIG cfg = {sizeof(cfg)};
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = L"Orchard";
    cfg.pszMainIcon = TD_WARNING_ICON;
    cfg.pszContent = text.c_str();
    cfg.pButtons = buttons;
    cfg.cButtons = UINT(std::size(buttons));
    cfg.nDefaultButton = 101;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) return 101;
    return pressed;
}
}

void init_network_permission(const std::filesystem::path& store, const std::string& bundle_id, const std::string& app_name,
                             NetworkPolicy override_policy)
{
    std::lock_guard g(g_lock);
    g_store = store;
    g_bundle_id = bundle_id;
    g_app_name = app_name;
    g_policy = override_policy;
    if (g_policy != NetworkPolicy::Ask) return;
    std::ifstream in(store);
    for (std::string line; std::getline(in, line);)
    {
        std::istringstream ls(line);
        std::string id, choice;
        if (ls >> id >> choice && id == bundle_id)
            g_policy = choice == "allow" ? NetworkPolicy::Allow : choice == "deny" ? NetworkPolicy::Deny : NetworkPolicy::Ask;
    }
}

bool network_allowed()
{
    std::lock_guard g(g_lock);
    if (g_decided) return g_allowed;
    if (g_policy != NetworkPolicy::Ask)
    {
        g_allowed = g_policy == NetworkPolicy::Allow;
    }
    else
    {
        int pressed = ask();
        g_allowed = pressed == 100 || pressed == 102;
        if (pressed == 102 || pressed == 103) remember(g_allowed);
    }
    g_decided = true;
    std::printf("[network] %s internet access\n", g_allowed ? "allowed" : "denied");
    return g_allowed;
}

}
