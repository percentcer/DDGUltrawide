// DDGUltrawide: proxy version.dll for Densha de Go!! AC (5.80.02)
//
// Windows loads this in place of System32\version.dll because it sits next to
// the game's executable. It forwards all real version.dll calls (proxy.cpp and
// proxy_thunks.asm) and installs our hooks on a background thread.

#include "cmdline.h"
#include "compositor.h"
#include "config.h"
#include "layout.h"
#include "log.h"
#include "proxy.h"
#include "touch.h"
#include "uiscale.h"

#include <windows.h>
#include <MinHook.h>
#include <string>

namespace
{
    std::wstring g_dir;   // folder this DLL lives in (with trailing backslash)

    std::wstring ModuleDir(HMODULE module)
    {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring s(path, len);
        size_t slash = s.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
    }

    bool g_hooksReady = false;

    DWORD WINAPI InitThread(LPVOID)
    {
        if (!g_hooksReady) return 0;
        if (g_cfg.compositor)
        {
            InstallCompositor();
            InstallTouchHooks();
        }
        else
        {
            InstallLayoutHook();
            InstallUIScaleHook();
        }
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_dir = ModuleDir(module);
        logx::Init(g_dir + L"DDGUltrawide.log");
        LOG("DDGUltrawide loaded into %p", GetModuleHandleW(nullptr));

        // Must happen before the game calls any version.dll function.
        if (!LoadOriginalVersionDll())
        {
            LOG("FATAL: could not load the real version.dll");
            return FALSE;
        }

        LoadConfig(g_dir + L"DDGUltrawide.ini");
        g_hooksReady = MH_Initialize() == MH_OK;
        if (!g_hooksReady) LOG("MH_Initialize failed");

        // The command line hook has to be in place before the game's startup code
        // reads the command line, so it's installed right here.
        if (g_hooksReady) InstallCommandLineHook(g_cfg.extraCommandLine);

        // The remaining hooks go on a separate thread to keep work under the loader lock small.
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
