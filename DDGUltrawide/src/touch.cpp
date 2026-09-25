#include "touch.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <windowsx.h>
#include <MinHook.h>
#include <atomic>
#include <vector>

namespace
{
    std::atomic<HWND> g_out[kWindowCount];
    std::atomic<HWND> g_game{ nullptr };

    // Screen and window captured by the current press (-1 = none). Presses stay
    // mapped to the screen they started on until released.
    std::atomic<int> g_captureRegion{ -1 };
    std::atomic<int> g_captureWindow{ -1 };

    // True while the real cursor is over the output window and being remapped.
    std::atomic<bool> g_remapping{ false };

    bool g_loggedFirstClick = false;

    using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
    using WindowFromPointFn = HWND(WINAPI*)(POINT);
    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    GetCursorPosFn g_origGetCursorPos = nullptr;
    WindowFromPointFn g_origWindowFromPoint = nullptr;
    SetCursorPosFn g_origSetCursorPos = nullptr;
    bool g_loggedSetCursor = false;

    bool IsTouchRegion(int i)
    {
        for (int r : g_cfg.touchScreens)
            if (r == i) return true;
        return false;
    }

    int WindowIndex(HWND hwnd)
    {
        for (int w = 0; w < kWindowCount; ++w)
            if (hwnd && g_out[w].load() == hwnd) return w;
        return -1;
    }

    // Output client point (in one of our windows) -> game client point.
    // forceRegion >= 0 clamps the point into that screen (used during a press).
    bool MapClientPoint(int window, int ox, int oy, int forceRegion, POINT& game, int& regionOut)
    {
        HWND gw = g_game.load();
        HWND ow = window >= 0 ? g_out[window].load() : nullptr;
        if (!gw || !ow) return false;

        RECT gc, oc;
        if (!GetClientRect(gw, &gc) || gc.right <= 0 || gc.bottom <= 0) return false;
        if (!GetClientRect(ow, &oc) || oc.right <= 0 || oc.bottom <= 0) return false;

        const float fx = (ox + 0.5f) / oc.right;
        const float fy = (oy + 0.5f) / oc.bottom;

        for (const Placement& p : Placements(window, oc.right, oc.bottom))
        {
            if (forceRegion >= 0 ? p.screen != forceRegion : !IsTouchRegion(p.screen)) continue;

            const Rect& d = p.dest;
            float u = (fx - d.originX) / d.sizeX;
            float v = (fy - d.originY) / d.sizeY;
            if (forceRegion < 0 && (u < 0 || u >= 1 || v < 0 || v >= 1)) continue;
            u = u < 0 ? 0 : (u > 0.9999f ? 0.9999f : u);
            v = v < 0 ? 0 : (v > 0.9999f ? 0.9999f : v);

            const Rect& s = p.source;
            game.x = static_cast<LONG>((s.originX + u * s.sizeX) * gc.right);
            game.y = static_cast<LONG>((s.originY + v * s.sizeY) * gc.bottom);
            regionOut = p.screen;
            return true;
        }
        return false;
    }

    // Which of our windows a screen point is over, and the point in its client area.
    int WindowAtScreenPoint(const POINT& screen, POINT& client)
    {
        for (int w = 0; w < kWindowCount; ++w)
        {
            HWND ow = g_out[w].load();
            RECT oc;
            if (!ow || !IsWindowVisible(ow) || !GetClientRect(ow, &oc)) continue;
            POINT c = screen;
            if (!ScreenToClient(ow, &c)) continue;
            if (c.x >= 0 && c.y >= 0 && c.x < oc.right && c.y < oc.bottom)
            {
                client = c;
                return w;
            }
        }
        return -1;
    }

    // Real screen point -> screen point inside the game window, if it's over one of our windows.
    bool MapScreenPoint(const POINT& screen, POINT& mapped)
    {
        HWND gw = g_game.load();
        if (!gw) return false;

        const int capture = g_captureRegion.load();
        const int captureWindow = g_captureWindow.load();
        POINT c;
        int window;
        if (capture >= 0 && captureWindow >= 0)
        {
            // A press keeps mapping through the window it started in, even past its edge.
            window = captureWindow;
            HWND ow = g_out[window].load();
            c = screen;
            if (!ow || !ScreenToClient(ow, &c)) return false;
        }
        else
        {
            window = WindowAtScreenPoint(screen, c);
            if (window < 0) return false;
        }

        POINT g;
        int region;
        if (!MapClientPoint(window, c.x, c.y, capture, g, region))
        {
            // Over the output but not over a touch region: park the game's cursor
            // just outside its client area so nothing in the game reacts.
            g.x = -16;
            g.y = -16;
        }
        if (!ClientToScreen(gw, &g)) return false;
        mapped = g;
        return true;
    }

    // Game screen point -> output screen point (reverse of MapScreenPoint).
    // Returns 1 if mapped, 0 if the point is inside the game window but not on a
    // touch region (can't be shown), -1 if it isn't in the game window at all.
    int UnmapScreenPoint(int x, int y, POINT& out)
    {
        HWND gw = g_game.load();
        if (!gw) return -1;

        POINT g = { x, y };
        RECT gc;
        if (!ScreenToClient(gw, &g) || !GetClientRect(gw, &gc) || gc.right <= 0 || gc.bottom <= 0) return -1;
        if (g.x < 0 || g.y < 0 || g.x >= gc.right || g.y >= gc.bottom) return -1;

        const float fx = (g.x + 0.5f) / gc.right;
        const float fy = (g.y + 0.5f) / gc.bottom;

        for (int w = 0; w < kWindowCount; ++w)
        {
            HWND ow = g_out[w].load();
            RECT oc;
            if (!ow || !GetClientRect(ow, &oc) || oc.right <= 0 || oc.bottom <= 0) continue;

            for (const Placement& p : Placements(w, oc.right, oc.bottom))
            {
                if (!IsTouchRegion(p.screen)) continue;
                const Rect& s = p.source;
                const float u = (fx - s.originX) / s.sizeX;
                const float v = (fy - s.originY) / s.sizeY;
                if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;

                const Rect& d = p.dest;
                POINT o = { static_cast<LONG>((d.originX + u * d.sizeX) * oc.right),
                            static_cast<LONG>((d.originY + v * d.sizeY) * oc.bottom) };
                if (!ClientToScreen(ow, &o)) return -1;
                out = o;
                return 1;
            }
        }
        return 0;
    }

    BOOL WINAPI Hook_SetCursorPos(int x, int y)
    {
        POINT o;
        const int r = UnmapScreenPoint(x, y, o);
        if (!g_loggedSetCursor && r >= 0)
        {
            g_loggedSetCursor = true;
            LOG("Game moved the cursor to %d,%d: %s", x, y,
                r == 1 ? "mapped back onto the output" : "not on a touch region, ignored");
        }
        if (r == 1) return g_origSetCursorPos(o.x, o.y);
        if (r == 0) return TRUE;   // pretend it worked; leave the real cursor alone
        return g_origSetCursorPos(x, y);
    }

    BOOL WINAPI Hook_GetCursorPos(LPPOINT p)
    {
        const BOOL ok = g_origGetCursorPos(p);
        if (!ok || !p) return ok;
        POINT m;
        const bool remap = MapScreenPoint(*p, m);
        g_remapping.store(remap);
        if (remap) *p = m;
        return ok;
    }

    HWND WINAPI Hook_WindowFromPoint(POINT p)
    {
        HWND gw = g_game.load();
        if (gw && g_remapping.load())
        {
            RECT r;
            if (GetWindowRect(gw, &r) && p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
                return gw;
        }
        return g_origWindowFromPoint(p);
    }

    bool IsMouseMessage(UINT msg)
    {
        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            return true;
        }
        return false;
    }

    bool IsButtonDown(UINT msg)
    {
        return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
    }

    bool IsButtonUp(UINT msg)
    {
        return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP;
    }
}

bool InstallTouchHooks()
{
    if (!g_cfg.touchEnabled)
    {
        LOG("Touch forwarding disabled in the ini");
        return false;
    }
    bool ok = MH_CreateHookApi(L"user32", "GetCursorPos", reinterpret_cast<void*>(&Hook_GetCursorPos),
                               reinterpret_cast<void**>(&g_origGetCursorPos)) == MH_OK;
    ok = ok && MH_CreateHookApi(L"user32", "WindowFromPoint", reinterpret_cast<void*>(&Hook_WindowFromPoint),
                                reinterpret_cast<void**>(&g_origWindowFromPoint)) == MH_OK;
    ok = ok && MH_CreateHookApi(L"user32", "SetCursorPos", reinterpret_cast<void*>(&Hook_SetCursorPos),
                                reinterpret_cast<void**>(&g_origSetCursorPos)) == MH_OK;
    ok = ok && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    LOG(ok ? "Touch hooks installed (%zu touch screens)" : "Touch hooks FAILED", g_cfg.touchScreens.size());
    return ok;
}

void TouchSetOutputWindow(int window, HWND out)
{
    if (window >= 0 && window < kWindowCount) g_out[window].store(out);
}
void TouchSetGameWindow(HWND game) { g_game.store(game); }

bool TouchHandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    if (msg == WM_SETCURSOR && g_cfg.hideCursor && LOWORD(lp) == HTCLIENT)
    {
        SetCursor(nullptr);
        result = TRUE;
        return true;
    }
    if (msg == WM_CAPTURECHANGED)
    {
        g_captureRegion.store(-1);
        g_captureWindow.store(-1);
        return false;
    }
    if (!g_cfg.touchEnabled || !IsMouseMessage(msg)) return false;

    HWND gw = g_game.load();
    const int window = WindowIndex(hwnd);
    if (!gw || window < 0) return false;

    const int ox = GET_X_LPARAM(lp);
    const int oy = GET_Y_LPARAM(lp);
    const int capture = g_captureRegion.load();

    POINT g;
    int region;
    const bool mapped = MapClientPoint(window, ox, oy, capture, g, region);

    if (IsButtonDown(msg))
    {
        if (!mapped) { result = 0; return true; }   // press outside touch regions: ignore
        if (capture < 0)
        {
            g_captureRegion.store(region);
            g_captureWindow.store(window);
            SetCapture(hwnd);
        }
        if (!g_loggedFirstClick)
        {
            g_loggedFirstClick = true;
            LOG("First click: window %d at %d,%d -> region %d, game client %ld,%ld",
                window, ox, oy, region, g.x, g.y);
        }
    }

    if (mapped)
        PostMessageW(gw, msg, wp, MAKELPARAM(static_cast<WORD>(g.x), static_cast<WORD>(g.y)));

    if (IsButtonUp(msg) && capture >= 0 && (wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0)
    {
        g_captureRegion.store(-1);
        g_captureWindow.store(-1);
        ReleaseCapture();
    }
    result = 0;
    return true;
}
