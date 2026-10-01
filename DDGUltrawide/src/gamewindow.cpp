#include "gamewindow.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <cmath>
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
    std::unordered_map<HWND, bool> g_locked;    // top-level: its client area is kept at the game's render size
    bool g_placed = false;                      // PlaceGameWindow: keep its position too
    const UINT kFocusMessage = RegisterWindowMessageW(L"DDGUltrawide.FocusGameWindow");

    // Brings the window to the foreground (on its own thread). Windows only
    // lets a process take the foreground in some situations, so this briefly
    // shares input with the thread that has it.
    void TakeForeground(HWND hwnd)
    {
        HWND fg = GetForegroundWindow();
        if (fg == hwnd) return;
        const DWORD me = GetCurrentThreadId();
        const DWORD them = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const bool attached = them && them != me && AttachThreadInput(me, them, TRUE);
        ShowWindow(hwnd, SW_SHOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        if (attached) AttachThreadInput(me, them, FALSE);
        LOG(GetForegroundWindow() == hwnd ? "Game window brought to the foreground"
                                          : "Could not bring the game window to the foreground");
    }

    // Where the game window's client area goes (desktop pixels): with the
    // touch panel's part of the frame at the top left of the output that
    // shows it (the panel's own window, or the main one)
    POINT ClientOrigin()
    {
        float x0 = 0.5f, y0 = 0.5f;                                      // (the touch panel's quadrant)
        for (const Placement& p : Placements(kMainWindow, g_cfg.outW, g_cfg.outH))
            if (p.screen == kTouchPanelScreen) { x0 = p.source.originX; y0 = p.source.originY; }
        const bool own = g_cfg.panelWindow && g_cfg.panelW > 0 && g_cfg.panelH > 0;
        const int dx = own ? g_cfg.panelX : g_cfg.outX, dy = own ? g_cfg.panelY : g_cfg.outY;
        return { dx - static_cast<LONG>(x0 * g_cfg.renderW), dy - static_cast<LONG>(y0 * g_cfg.renderH) };
    }

    LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        WNDPROC orig = nullptr;
        bool locked = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_origProcs.find(hwnd);
            if (it != g_origProcs.end()) orig = it->second;
            auto lk = g_locked.find(hwnd);
            if (lk != g_locked.end()) locked = lk->second;
            if (msg == WM_NCDESTROY && it != g_origProcs.end()) g_origProcs.erase(it);
            if (msg == WM_NCDESTROY && lk != g_locked.end()) g_locked.erase(lk);
        }
        if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);
        // Keep it out of Alt+Tab and the taskbar, whatever styles the game
        // sets on it later (see Hook_CreateWindowExW)
        if (msg == WM_STYLECHANGING && wp == static_cast<WPARAM>(GWL_EXSTYLE) && lp && locked)
        {
            auto* st = reinterpret_cast<STYLESTRUCT*>(lp);
            st->styleNew = (st->styleNew | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
        }
        if (msg == kFocusMessage && kFocusMessage)
        {
            TakeForeground(hwnd);
            return 0;
        }

        // Keep its client area at the game's render size: the game renders at
        // its window's size, and window managers (such as PowerToys
        // FancyZones) can resize it, which squashes or shifts the game's
        // screens in its frame. The window's outer size follows from its
        // current style (the game changes it). Moving and minimizing are left alone.
        if (msg == WM_WINDOWPOSCHANGING && lp && locked && g_cfg.renderW > 0 && g_cfg.renderH > 0 && !IsIconic(hwnd))
        {
            auto* wpos = reinterpret_cast<WINDOWPOS*>(lp);
            RECT want = { 0, 0, g_cfg.renderW, g_cfg.renderH };
            AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
            const int cx = want.right - want.left, cy = want.bottom - want.top;
            if (g_placed && !(wpos->flags & SWP_NOMOVE))
            {
                const POINT o = ClientOrigin();
                wpos->x = o.x + want.left;
                wpos->y = o.y + want.top;
            }
            if (!(wpos->flags & SWP_NOSIZE) && (wpos->cx != cx || wpos->cy != cy))
            {
                static int logged = 0;
                if (logged++ < 5)
                    LOG("Game window resize to %dx%d changed to %dx%d (a %dx%d client area)",
                        wpos->cx, wpos->cy, cx, cy, g_cfg.renderW, g_cfg.renderH);
                wpos->cx = cx;
                wpos->cy = cy;
            }
        }

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
        // The game's main window stays out of Alt+Tab and the taskbar: our
        // output window stands in for it there (so its thumbnail shows the
        // output, not the game's raw frame). Set before it's ever shown.
        const bool unreal = cls && HIWORD(reinterpret_cast<ULONG_PTR>(cls)) != 0 && wcscmp(cls, L"UnrealWindow") == 0;
        if (unreal && !parent && !(style & WS_CHILD))
            exStyle = (exStyle | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;

        HWND hwnd = g_origCreateWindowExW(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
        if (!hwnd) return hwnd;

        wchar_t className[64] = {};
        GetClassNameW(hwnd, className, 64);
        if (wcscmp(className, L"UnrealWindow") != 0) return hwnd;   // only the engine's own windows

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto prev = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
            if (!prev) return hwnd;
            g_origProcs[hwnd] = prev;
            g_locked[hwnd] = !parent;
        }
        LOG("Game window %p created (%dx%d); size limit lifted", hwnd, w, h);

        // (Outside the lock: changing its style calls back into SubclassProc)
        if (!parent && !(style & WS_CHILD))
        {
            const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            const LONG_PTR want = (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
            if (ex != want) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);
            LOG("  kept out of Alt+Tab and the taskbar (extended style %08llx -> %08llx)",
                static_cast<unsigned long long>(ex), static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        }
        return hwnd;
    }
}

void PlaceGameWindow(HWND game)
{
    if (!game || g_cfg.renderW <= 0 || g_cfg.renderH <= 0) return;
    g_placed = true;
    const POINT o = ClientOrigin();
    RECT want = { 0, 0, g_cfg.renderW, g_cfg.renderH };
    AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongPtrW(game, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(game, GWL_EXSTYLE)));
    // Asynchronously: this runs on our output windows' thread, and the game's
    // thread (which owns its window) may be waiting on ours, presenting to our
    // windows; waiting on it back would deadlock
    SetWindowPos(game, nullptr, o.x + want.left, o.y + want.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS );
    LOG("Game window placed with its client area at %ld,%ld (the touch screens on the desktop)", o.x, o.y);
}

void FocusGameWindow(HWND game)
{
    if (game && kFocusMessage) PostMessageW(game, kFocusMessage, 0, 0);
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
