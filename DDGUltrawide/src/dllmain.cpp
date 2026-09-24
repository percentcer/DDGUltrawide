// DDGUltrawide: proxy version.dll for Densha de Go!! AC (5.80.02)
//
// Windows loads this in place of System32\version.dll because it sits next to
// the game's executable. It forwards all real version.dll calls (proxy.cpp and
// proxy_thunks.asm), then:
//   - compositor.cpp: lets the game render its stock 3840x2160 2x2 frame, and
//     redraws the four screens into its own window in the configured layout
//   - touch.cpp:      forwards clicks on the output window back to the game
//   - cmdline.cpp:    optionally appends options to the game's command line

#include "cmdline.h"
#include "compositor.h"
#include "config.h"
#include "log.h"
#include "proxy.h"
#include "touch.h"

#include <windows.h>
#include <MinHook.h>
#include <string>

namespace
{
    std::wstring ModuleDir(HMODULE module)
    {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
        const std::wstring s(path, len);
        const size_t slash = s.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
    }

    DWORD WINAPI InitThread(LPVOID)
    {
        InstallCompositor();
        InstallTouchHooks();
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(module);
    const std::wstring dir = ModuleDir(module);
    logx::Init(dir + L"DDGUltrawide.log");
    LOG("DDGUltrawide loaded into %p", GetModuleHandleW(nullptr));

    // Must happen before the game calls any version.dll function.
    if (!LoadOriginalVersionDll())
    {
        LOG("FATAL: could not load the real version.dll");
        return FALSE;
    }

    LoadConfig(dir + L"DDGUltrawide.ini");
    if (MH_Initialize() != MH_OK)
    {
        LOG("MH_Initialize failed; running without hooks");
        return TRUE;
    }

    // The command line hook has to be in place before the game's startup code
    // reads the command line, so it's installed right here.
    InstallCommandLineHook(g_cfg.extraCommandLine);

    // The remaining hooks go on a separate thread to keep work under the loader lock small.
    const HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
