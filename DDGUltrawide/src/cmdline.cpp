#include "cmdline.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <string>

namespace
{
    std::wstring g_cmdW;   // lives for the whole process
    std::string g_cmdA;

    using GetCommandLineWFn = LPWSTR(WINAPI*)();
    using GetCommandLineAFn = LPSTR(WINAPI*)();
    GetCommandLineWFn g_origW = nullptr;
    GetCommandLineAFn g_origA = nullptr;

    LPWSTR WINAPI Hook_GetCommandLineW() { return g_cmdW.data(); }
    LPSTR WINAPI Hook_GetCommandLineA() { return g_cmdA.data(); }

    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }
}

bool InstallCommandLineHook(const std::wstring& extra)
{
    if (extra.empty())
    {
        LOG("No ExtraCommandLine set; command line unchanged");
        return false;
    }

    g_cmdW = std::wstring(GetCommandLineW()) + L" " + extra;
    g_cmdA = Narrow(g_cmdW);
    LOG("Command line: %ls", g_cmdW.c_str());

    // kernel32's versions forward to kernelbase; hook the real ones.
    bool ok = MH_CreateHookApi(L"kernelbase", "GetCommandLineW", reinterpret_cast<void*>(&Hook_GetCommandLineW),
                               reinterpret_cast<void**>(&g_origW)) == MH_OK;
    ok = ok && MH_CreateHookApi(L"kernelbase", "GetCommandLineA", reinterpret_cast<void*>(&Hook_GetCommandLineA),
                                reinterpret_cast<void**>(&g_origA)) == MH_OK;
    ok = ok && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;

    LOG(ok ? "Command line hook installed" : "Command line hook FAILED");
    return ok;
}
