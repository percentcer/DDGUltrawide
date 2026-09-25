#include "config.h"
#include "log.h"

#include <windows.h>
#include <cmath>
#include <cwchar>
#include <string>

Config g_cfg;

namespace
{
    std::wstring ReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def)
    {
        wchar_t buf[2048];
        GetPrivateProfileStringW(section, key, def, buf, 2048, ini.c_str());
        return buf;
    }

    int ReadInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def)
    {
        return GetPrivateProfileIntW(section, key, def, ini.c_str());
    }

    std::wstring Trim(std::wstring s)
    {
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        return s;
    }

    // Parses "0.5", "1/3", "5/12", etc.
    bool ParseNumber(std::wstring s, float& out)
    {
        s = Trim(s);
        if (s.empty()) return false;

        wchar_t* end = nullptr;
        const size_t slash = s.find(L'/');
        if (slash == std::wstring::npos)
        {
            out = static_cast<float>(wcstod(s.c_str(), &end));
            return end && *end == 0;
        }
        const std::wstring a = Trim(s.substr(0, slash)), b = Trim(s.substr(slash + 1));
        const double num = wcstod(a.c_str(), &end);
        if (!end || *end != 0) return false;
        const double den = wcstod(b.c_str(), &end);
        if (!end || *end != 0 || den == 0) return false;
        out = static_cast<float>(num / den);
        return true;
    }

    std::vector<std::wstring> SplitCommas(const std::wstring& s)
    {
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (;;)
        {
            const size_t comma = s.find(L',', start);
            parts.push_back(s.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
        return parts;
    }

    // "SizeX, SizeY, OriginX, OriginY"
    bool ParseRect(const std::wstring& s, Rect& r)
    {
        const std::vector<std::wstring> parts = SplitCommas(s);
        if (parts.size() != 4) return false;
        float v[4];
        for (int i = 0; i < 4; ++i)
            if (!ParseNumber(parts[i], v[i])) return false;
        r = { v[0], v[1], v[2], v[3] };
        return true;
    }

    // Reads a single rect; keeps the default if the key is missing or malformed.
    void ReadRect(const std::wstring& ini, const wchar_t* section, const wchar_t* key, Rect& target)
    {
        const std::wstring val = ReadString(ini, section, key, L"");
        if (val.empty()) return;
        Rect r;
        if (ParseRect(val, r)) target = r;
        else LOG("Bad [%ls] %ls: %ls (using the default)", section, key, val.c_str());
    }

    // Reads Prefix0, Prefix1, ... until the first missing key. Keeps the defaults
    // if the section has no entries or any entry is malformed.
    void ReadRects(const std::wstring& ini, const wchar_t* section, const wchar_t* prefix, std::vector<Rect>& target)
    {
        std::vector<Rect> rects;
        for (int i = 0; i < 16; ++i)
        {
            wchar_t key[16];
            swprintf(key, 16, L"%ls%d", prefix, i);
            const std::wstring val = ReadString(ini, section, key, L"");
            if (val.empty()) break;
            Rect r;
            if (!ParseRect(val, r))
            {
                LOG("Bad [%ls] %ls: %ls (using defaults for this section)", section, key, val.c_str());
                return;
            }
            rects.push_back(r);
        }
        if (!rects.empty()) target = rects;
    }

    void LogRects(const char* name, const std::vector<Rect>& rects)
    {
        for (size_t i = 0; i < rects.size(); ++i)
        {
            const Rect& r = rects[i];
            LOG("  %s%zu: size %.4f x %.4f, origin %.4f, %.4f", name, i, r.sizeX, r.sizeY, r.originX, r.originY);
        }
    }
}

void LoadConfig(const std::wstring& ini)
{
    // Defaults: the cabinet's 2x2 frame drawn as three screens across the top
    // and the touch panel centered below. The panel's bottom 256 of 1080 rows
    // are matted off on the cabinet, so only its top 824 rows are drawn,
    // filling the full height below the forward screens (1118x480 at 5120x1440).
    g_cfg.dests = { {1.0f / 3, 2.0f / 3, 0.0f, 0.0f},
                    {1.0f / 3, 2.0f / 3, 1.0f / 3, 0.0f},
                    {1.0f / 3, 2.0f / 3, 2.0f / 3, 0.0f},
                    {1118.0f / 5120, 1.0f / 3, 2001.0f / 5120, 2.0f / 3} };
    g_cfg.sources = { {0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f},
                      {0.5f, 0.5f, 0.0f, 0.5f}, {0.5f, 824.0f / 2160, 0.5f, 0.5f} };

    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        LOG("Config not found (%ls); using defaults", ini.c_str());

    g_cfg.outW = ReadInt(ini, L"Output", L"Width", g_cfg.outW);
    g_cfg.outH = ReadInt(ini, L"Output", L"Height", g_cfg.outH);
    g_cfg.outX = ReadInt(ini, L"Output", L"X", g_cfg.outX);
    g_cfg.outY = ReadInt(ini, L"Output", L"Y", g_cfg.outY);
    g_cfg.vsync = ReadInt(ini, L"Output", L"VSync", 1) != 0;
    g_cfg.pixelSnap = ReadInt(ini, L"Output", L"PixelSnap", 1) != 0;
    g_cfg.gameWindowMode = ReadInt(ini, L"Output", L"GameWindow", 0);

    ReadRects(ini, L"Layout", L"P", g_cfg.dests);
    ReadRects(ini, L"Source", L"S", g_cfg.sources);

    g_cfg.panelWindow = ReadInt(ini, L"TouchPanelWindow", L"Enabled", 0) != 0;
    g_cfg.panelMonitor = ReadInt(ini, L"TouchPanelWindow", L"Monitor", g_cfg.panelMonitor);
    g_cfg.panelX = ReadInt(ini, L"TouchPanelWindow", L"X", g_cfg.panelX);
    g_cfg.panelY = ReadInt(ini, L"TouchPanelWindow", L"Y", g_cfg.panelY);
    g_cfg.panelW = ReadInt(ini, L"TouchPanelWindow", L"Width", g_cfg.panelW);
    g_cfg.panelH = ReadInt(ini, L"TouchPanelWindow", L"Height", g_cfg.panelH);
    g_cfg.panelVsync = ReadInt(ini, L"TouchPanelWindow", L"VSync", 0) != 0;
    ReadRect(ini, L"TouchPanelWindow", L"Layout", g_cfg.panelDest);
    ReadRect(ini, L"TouchPanelWindow", L"Source", g_cfg.panelSource);

    g_cfg.touchEnabled = ReadInt(ini, L"Touch", L"Enabled", 1) != 0;
    g_cfg.hideCursor = ReadInt(ini, L"Touch", L"HideCursor", 0) != 0;
    g_cfg.touchScreens.clear();
    for (const std::wstring& part : SplitCommas(ReadString(ini, L"Touch", L"Screens", L"3")))
    {
        float v;
        if (ParseNumber(part, v)) g_cfg.touchScreens.push_back(static_cast<int>(v));
    }

    g_cfg.renderW = ReadInt(ini, L"Game", L"RenderWidth", g_cfg.renderW);
    g_cfg.renderH = ReadInt(ini, L"Game", L"RenderHeight", g_cfg.renderH);
    g_cfg.extraCommandLine = ReadString(ini, L"Game", L"ExtraCommandLine", L"");

    LOG("Output: %dx%d at %d,%d, vsync %d, pixel snap %d, game window mode %d",
        g_cfg.outW, g_cfg.outH, g_cfg.outX, g_cfg.outY, g_cfg.vsync ? 1 : 0,
        g_cfg.pixelSnap ? 1 : 0, g_cfg.gameWindowMode);
    if (g_cfg.renderW > 0 && g_cfg.renderH > 0)
        LOG("Game render size: %dx%d", g_cfg.renderW, g_cfg.renderH);
    else
        LOG("Game render size: left to the game");
    LogRects("P", g_cfg.dests);
    LogRects("S", g_cfg.sources);
    if (g_cfg.dests.size() != g_cfg.sources.size())
        LOG("Note: %zu layout entries but %zu source entries; drawing %zu screens",
            g_cfg.dests.size(), g_cfg.sources.size(), g_cfg.ScreenCount());
    if (g_cfg.panelWindow)
    {
        if (g_cfg.panelW > 0 && g_cfg.panelH > 0)
            LOG("Touch panel window: %dx%d at %d,%d, vsync %d", g_cfg.panelW, g_cfg.panelH,
                g_cfg.panelX, g_cfg.panelY, g_cfg.panelVsync ? 1 : 0);
        else
            LOG("Touch panel window: monitor %d%s, vsync %d", g_cfg.panelMonitor,
                g_cfg.panelMonitor == 0 ? " (auto)" : "", g_cfg.panelVsync ? 1 : 0);
        const Rect& d = g_cfg.panelDest;
        const Rect& s = g_cfg.panelSource;
        LOG("  Layout: size %.4f x %.4f, origin %.4f, %.4f", d.sizeX, d.sizeY, d.originX, d.originY);
        LOG("  Source: size %.4f x %.4f, origin %.4f, %.4f", s.sizeX, s.sizeY, s.originX, s.originY);
    }
}

std::vector<Placement> Placements(int window, int width, int height)
{
    auto snap = [&](const Rect& r)
    {
        if (!g_cfg.pixelSnap || width <= 0 || height <= 0) return r;
        auto axis = [](float origin, float size, int total, float& outOrigin, float& outSize)
        {
            const double p0 = std::floor(origin * total + 0.5);
            const double p1 = std::floor((origin + size) * total + 0.5);
            outOrigin = static_cast<float>(p0 / total);
            outSize = static_cast<float>((p1 - p0) / total);
        };
        Rect s;
        axis(r.originX, r.sizeX, width, s.originX, s.sizeX);
        axis(r.originY, r.sizeY, height, s.originY, s.sizeY);
        return s;
    };

    std::vector<Placement> out;
    if (window == kPanelWindow)
    {
        if (g_cfg.panelWindow)
            out.push_back({ kTouchPanelScreen, snap(g_cfg.panelDest), g_cfg.panelSource });
        return out;
    }
    for (size_t i = 0; i < g_cfg.ScreenCount(); ++i)
    {
        const int screen = static_cast<int>(i);
        if (g_cfg.panelWindow && screen == kTouchPanelScreen) continue;
        out.push_back({ screen, snap(g_cfg.dests[i]), g_cfg.sources[i] });
    }
    return out;
}
