#include "gamewindow.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <cwchar>
#include <mutex>
#include <unordered_map>

namespace
{
    using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                            HWND, HMENU, HINSTANCE, LPVOID);
    CreateWindowExWFn g_origCreateWindowExW = nullptr;

    std::mutex g_mutex;
    std::unordered_map<HWND, WNDPROC> g_origProcs;

    LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        WNDPROC orig = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_origProcs.find(hwnd);
            if (it != g_origProcs.end()) orig = it->second;
            if (msg == WM_NCDESTROY && it != g_origProcs.end()) g_origProcs.erase(it);
        }
        if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);

        const LRESULT r = CallWindowProcW(orig, hwnd, msg, wp, lp);
        if (msg == WM_GETMINMAXINFO && lp)
        {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            if (mm->ptMaxTrackSize.x < 16384) mm->ptMaxTrackSize.x = 16384;
            if (mm->ptMaxTrackSize.y < 16384) mm->ptMaxTrackSize.y = 16384;
        }
        return r;
    }

    HWND WINAPI Hook_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y,
                                     int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    {
        HWND hwnd = g_origCreateWindowExW(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
        if (!hwnd) return hwnd;

        wchar_t className[64] = {};
        GetClassNameW(hwnd, className, 64);
        if (wcscmp(className, L"UnrealWindow") != 0) return hwnd;   // only the engine's own windows

        std::lock_guard<std::mutex> lock(g_mutex);
        auto prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
        if (prev)
        {
            g_origProcs[hwnd] = prev;
            LOG("Game window %p created (%dx%d); size limit lifted", hwnd, w, h);
        }
        return hwnd;
    }
}

bool InstallGameWindowHook()
{
    const bool ok =
        MH_CreateHookApi(L"user32", "CreateWindowExW", reinterpret_cast<void*>(&Hook_CreateWindowExW),
                         reinterpret_cast<void**>(&g_origCreateWindowExW)) == MH_OK &&
        MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    LOG(ok ? "Game window hook installed" : "Game window hook FAILED");
    return ok;
}
