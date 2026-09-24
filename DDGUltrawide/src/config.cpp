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

    // Parses "0.5", "1/3", "5/12", etc.
    bool ParseNumber(std::wstring s, float& out)
    {
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        if (s.empty()) return false;

        size_t slash = s.find(L'/');
        wchar_t* end = nullptr;
        if (slash == std::wstring::npos)
        {
            out = static_cast<float>(wcstod(s.c_str(), &end));
            return end && *end == 0;
        }
        std::wstring a = s.substr(0, slash), b = s.substr(slash + 1);
        double num = wcstod(a.c_str(), &end);
        if (!end || *end != 0) return false;
        double den = wcstod(b.c_str(), &end);
        if (!end || *end != 0 || den == 0) return false;
        out = static_cast<float>(num / den);
        return true;
    }

    // "SizeX, SizeY, OriginX, OriginY"
    bool ParseRect(const std::wstring& s, Rect& r)
    {
        float v[4];
        size_t start = 0;
        for (int i = 0; i < 4; ++i)
        {
            size_t comma = s.find(L',', start);
            std::wstring part = s.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            if (!ParseNumber(part, v[i])) return false;
            if (comma == std::wstring::npos && i < 3) return false;
            start = comma + 1;
        }
        r = { v[0], v[1], v[2], v[3] };
        return true;
    }
}

bool LoadConfig(const std::wstring& ini)
{
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LOG("Config not found: %ls", ini.c_str());
        return false;
    }

    g_cfg.compositor = GetPrivateProfileIntW(L"Mode", L"Compositor", 1, ini.c_str()) != 0;
    g_cfg.outX = GetPrivateProfileIntW(L"Output", L"X", 0, ini.c_str());
    g_cfg.outY = GetPrivateProfileIntW(L"Output", L"Y", 0, ini.c_str());
    g_cfg.outVSync = GetPrivateProfileIntW(L"Output", L"VSync", 1, ini.c_str()) != 0;
    g_cfg.gameWindowMode = GetPrivateProfileIntW(L"Output", L"GameWindow", 0, ini.c_str());

    g_cfg.touchEnabled = GetPrivateProfileIntW(L"Touch", L"Enabled", 1, ini.c_str()) != 0;
    g_cfg.touchHideCursor = GetPrivateProfileIntW(L"Touch", L"HideCursor", 0, ini.c_str()) != 0;
    {
        std::wstring list = ReadString(ini, L"Touch", L"Screens", L"3");
        g_cfg.touchRegions.clear();
        size_t start = 0;
        while (start <= list.size())
        {
            size_t comma = list.find(L',', start);
            std::wstring part = list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            float v;
            if (ParseNumber(part, v)) g_cfg.touchRegions.push_back(static_cast<int>(v));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
    }

    g_cfg.resW = GetPrivateProfileIntW(L"Display", L"Width", g_cfg.resW, ini.c_str());
    g_cfg.resH = GetPrivateProfileIntW(L"Display", L"Height", g_cfg.resH, ini.c_str());
    g_cfg.pixelSnap = GetPrivateProfileIntW(L"Display", L"PixelSnap", 1, ini.c_str()) != 0;
    g_cfg.extraCommandLine = ReadString(ini, L"Display", L"ExtraCommandLine", L"");

    g_cfg.players.clear();
    for (int i = 0; i < 8; ++i)
    {
        wchar_t key[8];
        swprintf(key, 8, L"P%d", i);
        std::wstring val = ReadString(ini, L"Layout", key, L"");
        if (val.empty()) break;
        Rect r;
        if (!ParseRect(val, r))
        {
            LOG("Bad [Layout] %ls: %ls", key, val.c_str());
            return false;
        }
        g_cfg.players.push_back(r);
    }

    g_cfg.uiScaleEnabled = GetPrivateProfileIntW(L"UI", L"FixScale", 1, ini.c_str()) != 0;
    g_cfg.uiReferencePlayer = GetPrivateProfileIntW(L"UI", L"ReferencePlayer", 0, ini.c_str());
    {
        float dh = 1080.0f;
        if (ParseNumber(ReadString(ini, L"UI", L"DesignHeight", L"1080"), dh)) g_cfg.uiDesignHeight = dh;
    }

    // Source regions; default to the stock 2x2 quadrants (TL, TR, BL, BR)
    g_cfg.sources.clear();
    const Rect defaults[4] = { {0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f},
                               {0.5f, 0.5f, 0.0f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f} };
    for (int i = 0; i < 8; ++i)
    {
        wchar_t key[8];
        swprintf(key, 8, L"S%d", i);
        std::wstring val = ReadString(ini, L"Source", key, L"");
        Rect r;
        if (!val.empty() && ParseRect(val, r)) g_cfg.sources.push_back(r);
        else if (val.empty() && i < 4) g_cfg.sources.push_back(defaults[i]);
        else if (!val.empty()) { LOG("Bad [Source] %ls: %ls", key, val.c_str()); return false; }
        else break;
    }

    g_cfg.layoutPlayersRva = wcstoull(ReadString(ini, L"Addresses", L"LayoutPlayersRVA", L"0").c_str(), nullptr, 0);
    g_cfg.dpiScaleRva = wcstoull(ReadString(ini, L"Addresses", L"DPIScaleRVA", L"0").c_str(), nullptr, 0);
    g_cfg.offSplitscreenInfo = static_cast<uint32_t>(wcstoul(ReadString(ini, L"Addresses", L"SplitscreenInfoOffset", L"0x60").c_str(), nullptr, 0));
    g_cfg.offActiveType = static_cast<uint32_t>(wcstoul(ReadString(ini, L"Addresses", L"ActiveSplitscreenTypeOffset", L"0x78").c_str(), nullptr, 0));

    LOG("Mode: %s", g_cfg.compositor ? "compositor" : "layout hooks");
    LOG("Config: %dx%d, pixel snap %d, %zu players, LayoutPlayers RVA 0x%llX, DPIScale RVA 0x%llX",
        g_cfg.resW, g_cfg.resH, g_cfg.pixelSnap ? 1 : 0, g_cfg.players.size(),
        static_cast<unsigned long long>(g_cfg.layoutPlayersRva),
        static_cast<unsigned long long>(g_cfg.dpiScaleRva));
    for (size_t i = 0; i < g_cfg.players.size(); ++i)
    {
        const Rect& r = g_cfg.players[i];
        LOG("  P%zu: size %.4f x %.4f, origin %.4f, %.4f", i, r.sizeX, r.sizeY, r.originX, r.originY);
    }
    return true;
}

std::vector<Rect> SnappedLayout()
{
    if (!g_cfg.pixelSnap || g_cfg.resW <= 0 || g_cfg.resH <= 0) return g_cfg.players;

    auto axis = [](float origin, float size, int total, float& outOrigin, float& outSize)
    {
        const double p0 = std::floor(origin * total + 0.5);
        const double p1 = std::floor((origin + size) * total + 0.5);
        outOrigin = (p0 == 0) ? 0.0f : static_cast<float>((p0 + 0.1) / total);
        outSize = static_cast<float>((p1 - p0 + 0.1) / total);
    };

    std::vector<Rect> out;
    for (const Rect& r : g_cfg.players)
    {
        Rect s;
        axis(r.originX, r.sizeX, g_cfg.resW, s.originX, s.sizeX);
        axis(r.originY, r.sizeY, g_cfg.resH, s.originY, s.sizeY);
        out.push_back(s);
    }
    return out;
}
