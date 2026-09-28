#include "config.h"
#include "log.h"

#include <windows.h>
#include <cmath>
#include <array>
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

    // Parses a hex color, "BAB3A5" or "#BAB3A5", into 0..1 components.
    bool ParseColor(std::wstring s, float rgb[3])
    {
        s = Trim(s);
        if (!s.empty() && s[0] == L'#') s.erase(0, 1);
        if (s.size() != 6) return false;
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(s.c_str(), &end, 16);
        if (!end || *end != 0) return false;
        rgb[0] = ((v >> 16) & 0xFF) / 255.0f;
        rgb[1] = ((v >> 8) & 0xFF) / 255.0f;
        rgb[2] = (v & 0xFF) / 255.0f;
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
    // and the touch panel centered below. Only the panel's top 822 of 1080 rows
    // are drawn (the rest is matted off on the cabinet), filling the full height
    // below the forward screens (1120x480 at 5120x1440).
    g_cfg.dests = { {1.0f / 3, 2.0f / 3, 0.0f, 0.0f},
                    {1.0f / 3, 2.0f / 3, 1.0f / 3, 0.0f},
                    {1.0f / 3, 2.0f / 3, 2.0f / 3, 0.0f},
                    {1120.0f / 5120, 1.0f / 3, 2000.0f / 5120, 2.0f / 3} };
    g_cfg.sources = { {0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f},
                      {0.5f, 0.5f, 0.0f, 0.5f}, {0.5f, kPanelVisibleRows / 2160, 0.5f, 0.5f} };

    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        LOG("Config not found (%ls); using defaults", ini.c_str());

    g_cfg.outW = ReadInt(ini, L"Output", L"Width", g_cfg.outW);
    g_cfg.outH = ReadInt(ini, L"Output", L"Height", g_cfg.outH);
    g_cfg.outX = ReadInt(ini, L"Output", L"X", g_cfg.outX);
    g_cfg.outY = ReadInt(ini, L"Output", L"Y", g_cfg.outY);
    g_cfg.vsync = ReadInt(ini, L"Output", L"VSync", 1) != 0;
    g_cfg.pixelSnap = ReadInt(ini, L"Output", L"PixelSnap", 1) != 0;
    g_cfg.gameWindowMode = ReadInt(ini, L"Output", L"GameWindow", 0);

    g_cfg.arcadeLayout = ReadInt(ini, L"Layout", L"ArcadeLayout", 1) != 0;
    float gap;
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeGap", L""), gap)) g_cfg.arcadeGap = gap;
    g_cfg.allowPanelOverlap = ReadInt(ini, L"Layout", L"ArcadeTouchPanelAllowOverlap", 0) != 0;
    float scaling;
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeTouchPanelScaling", L""), scaling)) g_cfg.panelScaling = scaling;
    g_cfg.arcadeCabinet = ReadInt(ini, L"Layout", L"ArcadeCabinet", 1) != 0;
    float light;
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeCabinetScreenLight", L""), light)) g_cfg.cabinetScreenLight = std::fmax(light, 0.0f);
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeCabinetRoomLight", L""), light)) g_cfg.cabinetRoomLight = std::fmax(light, 0.0f);
    const std::wstring color = ReadString(ini, L"Layout", L"ArcadeCabinetColor", L"");
    if (!color.empty() && !ParseColor(color, g_cfg.cabinetColor))
        LOG("Bad [Layout] ArcadeCabinetColor: %ls (using the default)", color.c_str());
    ReadRects(ini, L"Layout", L"P", g_cfg.dests);
    ReadRects(ini, L"Source", L"S", g_cfg.sources);

    g_cfg.panelWindow = ReadInt(ini, L"TouchPanelWindow", L"Enabled", 0) != 0;
    g_cfg.panelMonitor = ReadInt(ini, L"TouchPanelWindow", L"Monitor", g_cfg.panelMonitor);
    g_cfg.panelX = ReadInt(ini, L"TouchPanelWindow", L"X", g_cfg.panelX);
    g_cfg.panelY = ReadInt(ini, L"TouchPanelWindow", L"Y", g_cfg.panelY);
    g_cfg.panelW = ReadInt(ini, L"TouchPanelWindow", L"Width", g_cfg.panelW);
    g_cfg.panelH = ReadInt(ini, L"TouchPanelWindow", L"Height", g_cfg.panelH);
    g_cfg.panelVsync = ReadInt(ini, L"TouchPanelWindow", L"VSync", 0) != 0;
    g_cfg.panelDestFit = ReadString(ini, L"TouchPanelWindow", L"Layout", L"").empty();
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
    if (g_cfg.arcadeLayout)
    {
        LOG("Arcade layout, %.2f\" gaps, panel scaling %.2f, panel overlap %s, cabinet %s (ignores [Layout] P0..P3 and [Source]):",
            g_cfg.arcadeGap, g_cfg.panelScaling, g_cfg.allowPanelOverlap ? "allowed" : "off",
            g_cfg.arcadeCabinet ? "on" : "off");
        for (const Placement& p : Placements(kMainWindow, g_cfg.outW, g_cfg.outH))
            LOG("  Screen %d: %.0fx%.0f at %.0f,%.0f", p.screen, p.dest.sizeX * g_cfg.outW,
                p.dest.sizeY * g_cfg.outH, p.dest.originX * g_cfg.outW, p.dest.originY * g_cfg.outH);
    }
    else
    {
        LogRects("P", g_cfg.dests);
        LogRects("S", g_cfg.sources);
    }
    if (!g_cfg.arcadeLayout && g_cfg.dests.size() != g_cfg.sources.size())
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
        if (g_cfg.panelDestFit)
            LOG("  Layout: fit to the window, keeping the aspect ratio");
        else
            LOG("  Layout: size %.4f x %.4f, origin %.4f, %.4f", d.sizeX, d.sizeY, d.originX, d.originY);
        LOG("  Source: size %.4f x %.4f, origin %.4f, %.4f", s.sizeX, s.sizeY, s.originX, s.originY);
    }
}

namespace
{
    // The cabinet's 2x2 frame
    const Rect kStockSources[4] = { {0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f},
                                    {0.5f, 0.5f, 0.0f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f} };

    // The largest rect with the source's aspect ratio (in game pixels) that fits
    // a width x height window, centered.
    Rect FitRect(const Rect& source, int width, int height)
    {
        const float frameW = g_cfg.renderW > 0 ? static_cast<float>(g_cfg.renderW) : 3840.0f;
        const float frameH = g_cfg.renderH > 0 ? static_cast<float>(g_cfg.renderH) : 2160.0f;
        const float aspect = (source.sizeX * frameW) / (source.sizeY * frameH);
        const float windowAspect = static_cast<float>(width) / height;
        if (aspect > windowAspect)
        {
            const float h = windowAspect / aspect;   // fraction of the window's height
            return { 1.0f, h, 0.0f, (1.0f - h) / 2 };
        }
        const float w = aspect / windowAspect;       // fraction of the window's width
        return { w, 1.0f, (1.0f - w) / 2, 0.0f };
    }

    // Cabinet screen sizes: 55" center, 42" sides. The center's width in inches
    // (16:9) converts inches on the cabinet (gaps, frames) to pixels.
    constexpr float kSideScale = 42.0f / 55.0f;
    constexpr float kCenterWidthInches = 55.0f * 16.0f / 18.357560f;   // 18.36 = sqrt(16^2 + 9^2)

    // ---------------------------------------------------------------------
    // The drawn cabinet (ArcadeCabinet), in cabinet inches, measured from video
    // of a real cabinet. Around each forward screen's picture, from the inside out:
    //   - the monitor's own black border
    //   - the frame. On the side screens it's one piece: rails around all four
    //     sides, with no seams. The center screen has a full-height Z bracket
    //     down each side: a flange fastened to the wall, a step, and a raised
    //     flange beside the picture. Its top and bottom rails run between the
    //     brackets, as wide as the picture, with vertical seams where they meet.
    //   - below the frame, a Z bracket (a bent-over lip, then its face) that
    //     holds the screen to the wall
    // Large fasteners sit on the rails, small ones on the brackets.
    // ---------------------------------------------------------------------
    struct FrameSpec
    {
        float topRail;        // height of the top rail
        float side;           // width of each side: the rail, or the bracket's flange and lip
        float bottomRail;     // height of the bottom rail
        bool brackets;        // Z brackets instead of side rails
        int topFasteners;     // along the top rail, corner to corner
        int sideFasteners;    // down each side rail or bracket flange
    };
    constexpr FrameSpec kFrames[3] = {
        { 1.5f, 1.5f, 1.0f, false, 4, 1 },   // left
        { 1.7f, 1.6f, 1.0f, true,  5, 4 },   // center
        { 1.5f, 1.5f, 1.0f, false, 4, 1 },   // right
    };
    constexpr float kBorderIn = 0.3f;           // monitor's black border
    // Across a side bracket (its width is FrameSpec::side): wall flange, step,
    // raised flange, in these proportions
    constexpr float kFlangeParts[3] = { 27.0f, 8.0f, 27.0f };
    constexpr float kBracketIn = 1.0f;          // height of the Z bracket below each frame
    constexpr float kBracketLipIn = 0.25f;      // its lip, along the top
    constexpr float kSeamIn = 0.06f;            // width of a visible seam
    constexpr float kLargeFastenerIn = 0.6f;    // diameter of a rail fastener
    constexpr float kSmallFastenerIn = 0.3f;    // diameter of a bracket fastener
    constexpr int kBracketFasteners = 3;        // along the bottom bracket: both ends and the middle
    constexpr float kPanelSurroundIn = 0.5f;    // black surround of the touch panel in the console

    // Room each screen's frame takes around its picture, in inches
    constexpr float FrameSideIn(int s) { return kFrames[s].side + kBorderIn; }
    constexpr float FrameAboveIn(int s) { return kFrames[s].topRail + kBorderIn; }
    constexpr float FrameBelowIn(int s) { return kBorderIn + kFrames[s].bottomRail + kBracketIn; }

    // The arcade layout in a width x height window: the three forward screens at
    // their cabinet proportions, side by side with a flush bottom edge, and (when
    // it isn't in its own window) the touch panel centered under the center screen
    // at a third of the height, times ArcadeTouchPanelScaling. With ArcadeCabinet,
    // room is left for each screen's frame instead of ArcadeGap, and neighboring
    // frames touch. Gaps and frames are in cabinet inches, scaled like the
    // screens. The center screen
    // gets as much of the rest as fits; everything is centered in the window.
    // Edges are whole pixels, so neighboring screens share an exact boundary.
    //
    // With ArcadeTouchPanelAllowOverlap, the height left over under the panel no
    // longer limits the center screen: the screens can use the full width, the
    // center screen's top edge sits at the top of the window, and the panel sits
    // at the bottom, drawn over the center screen's bottom edge where they meet.
    std::vector<Placement> ArcadePlacements(int width, int height, bool withPanel)
    {
        const float W = static_cast<float>(width), H = static_cast<float>(height);

        float panelH = 0, panelW = 0;
        if (withPanel)
        {
            panelH = H / 3 * std::fmin(std::fmax(g_cfg.panelScaling, 0.05f), 3.0f);
            panelW = panelH * 1920.0f / kPanelVisibleRows;
        }

        // Everything relative to the center screen's width. With the cabinet, the
        // space between pictures is two frame sides, and the frames touch.
        const bool cabinet = g_cfg.arcadeCabinet;
        const float gapScale = cabinet ? 0.0f : std::fmax(g_cfg.arcadeGap, 0.0f) / kCenterWidthInches;
        auto scale = [&](float inches) { return cabinet ? inches / kCenterWidthInches : 0.0f; };
        const float sideScale[3] = { scale(FrameSideIn(0)), scale(FrameSideIn(1)), scale(FrameSideIn(2)) };   // one frame side
        const float aboveScale = scale(FrameAboveIn(1)), belowScale = scale(FrameBelowIn(1));
        const float rowWidth = 1 + 2 * kSideScale + 2 * gapScale                        // 3 screens, 2 gaps,
                             + 2 * (sideScale[0] + sideScale[1] + sideScale[2]);        // 6 frame sides
        const float rowHeight = 9.0f / 16.0f + aboveScale + belowScale;                 // center screen and its frame

        // Center screen: limited by the width or the height (left over under the
        // panel, or all of it when overlap is allowed)
        const float widthLimit = W / rowWidth;
        const bool overlap = withPanel && g_cfg.allowPanelOverlap &&
                             std::fmin(widthLimit, H / rowHeight) * rowHeight + panelH > H;
        const float centerW = std::fmin(widthLimit, (overlap ? H : H - panelH) / rowHeight);
        const float centerH = centerW * 9.0f / 16.0f;
        const float sideW = centerW * kSideScale, sideH = centerH * kSideScale;
        const float gap = centerW * gapScale;
        const float above = centerW * aboveScale, below = centerW * belowScale;

        const float left = (W - centerW * rowWidth) / 2 + centerW * sideScale[0];   // left screen's picture
        const float top = (overlap ? 0.0f : (H - (centerW * rowHeight + panelH)) / 2) + above;
        const float bottom = top + centerH;    // shared bottom edge of the forward screens
        const float panelTop = overlap ? H - panelH : bottom + below;

        // Pixel edges, left to right: left screen, frames and gap, center screen, frames and gap, right screen
        auto px = [](float v) { return static_cast<float>(std::floor(v + 0.5f)); };
        const float betweenL = centerW * (sideScale[0] + sideScale[1]) + gap;   // left picture to center picture
        const float betweenR = centerW * (sideScale[1] + sideScale[2]) + gap;   // center picture to right picture
        const float centerL = left + sideW + betweenL, centerR = centerL + centerW;
        const float x0 = px(left), x1 = px(left + sideW), x2 = px(centerL), x3 = px(centerR);
        const float x4 = px(centerR + betweenR), x5 = px(centerR + betweenR + sideW);
        const float yb = px(bottom), yc = px(top), ys = px(bottom - sideH);
        auto rect = [&](float l, float t, float r, float b) { return Rect{ (r - l) / W, (b - t) / H, l / W, t / H }; };

        std::vector<Placement> out;
        out.push_back({ 0, rect(x0, ys, x1, yb), kStockSources[0] });
        out.push_back({ 1, rect(x2, yc, x3, yb), kStockSources[1] });
        out.push_back({ 2, rect(x4, ys, x5, yb), kStockSources[2] });
        if (withPanel)
        {
            const float mid = (centerL + centerR) / 2;
            Rect src = kStockSources[kTouchPanelScreen];
            src.sizeY *= kPanelVisibleRows / 1080.0f;
            // Last, so it's drawn over the center screen when they overlap
            out.push_back({ kTouchPanelScreen,
                            rect(px(mid - panelW / 2), px(panelTop), px(mid + panelW / 2), px(panelTop + panelH)), src });
        }
        return out;
    }
}

std::vector<Placement> Placements(int window, int width, int height)
{
    if (g_cfg.arcadeLayout && window == kMainWindow)
        return ArcadePlacements(width, height, !g_cfg.panelWindow);

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
        if (g_cfg.panelWindow && width > 0 && height > 0)
        {
            const Rect dest = g_cfg.panelDestFit ? FitRect(g_cfg.panelSource, width, height) : g_cfg.panelDest;
            out.push_back({ kTouchPanelScreen, snap(dest), g_cfg.panelSource });
        }
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

namespace
{
    // Depths, in inches out from the wall
    constexpr float kRailZ = 1.0f;              // face of the frame rails
    constexpr float kFlangeLowZ = 0.1f;         // a bracket's wall flange
    constexpr float kFlangeHighZ = 1.0f;        // a bracket's raised flange
    constexpr float kBorderZ = 0.9f;            // the monitor's border, just behind the rails
    constexpr float kGlassZ = 0.8f;             // the screens' pictures
    constexpr float kBracketFaceZ = 0.25f;      // face of the bottom Z bracket
    constexpr float kConsoleZ = 12.0f;          // top of the console in front of the center screen
    constexpr float kRoundedEdgeIn = 0.12f;     // rounded-over edges of rails and the console
    constexpr float kSeamDepthIn = 0.3f;        // how far a seam's groove goes in

    // The booth around the cabinet, which the screens light and which lights the
    // cabinet in turn: its back wall behind the player, side walls, ceiling and floor
    constexpr float kBoothDepthIn = 70.0f;      // wall to back wall
    constexpr float kBoothSideIn = 8.0f;        // side walls, beyond the outer frames
    constexpr float kBoothCeilingIn = 10.0f;    // ceiling, above the center screen's frame
    constexpr float kBoothFloorIn = 45.0f;      // floor, below the center screen's picture
    constexpr float kFloorAlbedo = 0.08f;       // dark carpet (linear)

    // The side screens face in toward the player a little, as on the cabinet
    constexpr float kSideScreenAngleDeg = 25.0f;
    // Light cells per screen (across, down)
    constexpr int kCellsForward[2] = { 8, 4 };
    constexpr int kCellsPanel[2] = { 4, 2 };

    float ToLinear(float c) { return std::pow(c, 2.2f); }
}

bool GetCabinetScene(int width, int height, CabinetScene& scene)
{
    scene = {};
    if (!g_cfg.arcadeLayout || !g_cfg.arcadeCabinet || width <= 0 || height <= 0) return false;

    // Pictures in pixels (left, top, right, bottom), their sources, and pixels per cabinet inch
    const float W = static_cast<float>(width), H = static_cast<float>(height);
    float pic[4][4] = {};
    Rect src[4] = {};
    bool has[4] = {};
    for (const Placement& p : Placements(kMainWindow, width, height))
    {
        if (p.screen < 0 || p.screen > 3) continue;
        float* r = pic[p.screen];
        r[0] = p.dest.originX * W;
        r[1] = p.dest.originY * H;
        r[2] = (p.dest.originX + p.dest.sizeX) * W;
        r[3] = (p.dest.originY + p.dest.sizeY) * H;
        src[p.screen] = p.source;
        has[p.screen] = true;
    }
    if (!has[1]) return false;
    const float u = (pic[1][2] - pic[1][0]) / kCenterWidthInches;
    if (u <= 0) return false;
    scene.pxPerInch = u;
    scene.originX = (pic[1][0] + pic[1][2]) / 2;
    scene.originY = (pic[1][1] + pic[1][3]) / 2;
    auto inX = [&](float px) { return (px - scene.originX) / u; };
    auto inY = [&](float py) { return (py - scene.originY) / u; };

    // Materials
    struct Material { std::array<float, 3> albedo; float roughness, metalness; bool powderCoat; };
    const float* wall = g_cfg.cabinetColor;
    // The walls, rails and brackets: satin powder coat (semi-gloss, orange peel)
    auto paint = [&](float k) { return Material{ { std::fmin(wall[0] * k, 1.0f), std::fmin(wall[1] * k, 1.0f), std::fmin(wall[2] * k, 1.0f) }, 0.45f, 0.0f, true }; };
    const Material wallPaint = paint(1.0f), railPaint = paint(1.04f), seamPaint = paint(0.6f);
    const Material blackPlastic = { { 0.03f, 0.03f, 0.035f }, 0.45f, 0.0f, false };
    const Material consolePlastic = { { 0.07f, 0.075f, 0.08f }, 0.5f, 0.0f, false };
    const Material steel = { { 0.62f, 0.62f, 0.63f }, 0.3f, 1.0f, false };

    std::vector<CabinetShape>& shapes = scene.shapes;
    auto px = [](float v) { return static_cast<float>(std::floor(v + 0.5f)); };
    auto add = [&](CabinetShape s, const Material& m)
    {
        for (int i = 0; i < 3; ++i) s.albedo[i] = m.albedo[i];
        s.roughness = m.roughness;
        s.metalness = m.metalness;
        s.powderCoat = m.powderCoat;
        shapes.push_back(s);
    };
    auto box = [&](float x0, float y0, float x1, float y1, float z, const Material& m, float roundedIn = 0)
    {
        CabinetShape s = { CabinetShape::Box, px(x0), px(y0), px(x1), px(y1), z, z, roundedIn * u, 0 };
        if (s.x1 > s.x0 && s.y1 > s.y0) add(s, m);
    };
    auto ramp = [&](float x0, float y0, float x1, float y1, float z0, float z1, int axis, const Material& m)
    {
        CabinetShape s = { CabinetShape::Ramp, px(x0), px(y0), px(x1), px(y1), z0, z1, 0, axis };
        if (s.x1 > s.x0 && s.y1 > s.y0) add(s, m);
    };
    const float seam = std::fmax(kSeamIn * u, 1.0f);
    auto hseam = [&](float x0, float x1, float y, float z) { box(x0, y - seam / 2, x1, y + seam / 2, z - kSeamDepthIn, seamPaint); };
    auto vseam = [&](float x, float y0, float y1, float z) { box(x - seam / 2, y0, x + seam / 2, y1, z - kSeamDepthIn, seamPaint); };
    auto fastener = [&](float x, float y, float diameter, float z)
    {
        const float r = diameter * u / 2;
        add({ CabinetShape::Dome, x, y, r, 0, z, z, diameter * 0.3f, 0 }, steel);
    };
    // n points spread evenly from a to b, ends included (the middle when n = 1)
    auto spread = [](float a, float b, int n, int i) { return n <= 1 ? (a + b) / 2 : a + (b - a) * i / (n - 1); };

    box(0, 0, W, H, 0, wallPaint);   // wall

    float frameLeft = W, frameRight = 0, frameTop = H;
    for (int s = 0; s < 3; ++s)
    {
        const float* r = pic[s];
        if (!has[s]) continue;
        const FrameSpec& f = kFrames[s];
        // Frame edges: outside, the opening (picture plus the monitor's border), and the rails between
        const float ox0 = r[0] - FrameSideIn(s) * u, ox1 = r[2] + FrameSideIn(s) * u;
        const float oy0 = r[1] - FrameAboveIn(s) * u;
        const float in0 = r[0] - kBorderIn * u, in1 = r[2] + kBorderIn * u;
        const float iy0 = r[1] - kBorderIn * u, iy1 = r[3] + kBorderIn * u;
        const float oy1 = iy1 + f.bottomRail * u;               // bottom of the frame
        const float by1 = oy1 + kBracketIn * u;                 // bottom of the bracket below it
        const float lip = kBracketLipIn * u;
        frameLeft = std::fmin(frameLeft, ox0);
        frameRight = std::fmax(frameRight, ox1);
        frameTop = std::fmin(frameTop, oy0);

        // Rails (one piece on the side screens), and the monitor's border in the opening
        box(ox0, oy0, ox1, oy1, kRailZ, railPaint, kRoundedEdgeIn);
        box(in0, iy0, in1, iy1, kBorderZ, blackPlastic);
        box(r[0], r[1], r[2], r[3], kGlassZ, blackPlastic);   // under the picture

        float wall0 = 0, wall1 = 0;   // centers of the side brackets' wall flanges, for their fasteners
        if (f.brackets)
        {
            // Side brackets, full height, from the frame's edge to the picture's
            // border (so the top and bottom rails are as wide as the picture):
            // a wall flange, a step up, and a raised flange
            const float total = kFlangeParts[0] + kFlangeParts[1] + kFlangeParts[2];
            auto bracket = [&](float x0, float x1, bool wallOnLeft)
            {
                const float w = x1 - x0;
                const float a = x0 + w * kFlangeParts[0] / total;          // step starts
                const float b = a + w * kFlangeParts[1] / total;           // step ends
                const float zl = wallOnLeft ? kFlangeLowZ : kFlangeHighZ, zr = wallOnLeft ? kFlangeHighZ : kFlangeLowZ;
                box(x0, oy0, a, oy1, zl, railPaint);
                ramp(a, oy0, b, oy1, zl, zr, 0, railPaint);
                box(b, oy0, x1, oy1, zr, railPaint);
            };
            bracket(ox0, in0, true);
            bracket(in1, ox1, false);
            vseam(in0, oy0, iy0, kRailZ); vseam(in0, iy1, oy1, kRailZ);
            vseam(in1, oy0, iy0, kRailZ); vseam(in1, iy1, oy1, kRailZ);
            wall0 = ox0 + (in0 - ox0) * kFlangeParts[0] / total / 2;
            wall1 = ox1 - (ox1 - in1) * kFlangeParts[2] / total / 2;
        }

        // Bottom bracket: its lip sloping down from the frame, then its face
        ramp(ox0, oy1, ox1, oy1 + lip, kRailZ, kBracketFaceZ, 1, railPaint);
        box(ox0, oy1 + lip, ox1, by1, kBracketFaceZ, railPaint, kRoundedEdgeIn / 2);

        // Fasteners: large ones along the top rail (the end ones half the rail's
        // height in from the opening's corners) and on side rails, small ones
        // on the brackets
        const float topY = (oy0 + iy0) / 2, topInset = f.topRail * u / 2;
        for (int i = 0; i < f.topFasteners; ++i)
            fastener(spread(in0 + topInset, in1 - topInset, f.topFasteners, i), topY, kLargeFastenerIn, kRailZ);
        for (int i = 0; i < f.sideFasteners; ++i)
        {
            if (f.brackets)
            {
                // Down the wall flanges, from level with the top rail to level with the bottom rail
                const float y = spread(topY, (iy1 + oy1) / 2, f.sideFasteners, i);
                fastener(wall0, y, kSmallFastenerIn, kFlangeLowZ);
                fastener(wall1, y, kSmallFastenerIn, kFlangeLowZ);
            }
            else
            {
                const float y = r[1] + (r[3] - r[1]) * (i + 0.5f) / f.sideFasteners;
                fastener((ox0 + in0) / 2, y, kLargeFastenerIn, kRailZ);
                fastener((in1 + ox1) / 2, y, kLargeFastenerIn, kRailZ);
            }
        }
        const float bracketY = (oy1 + lip + by1) / 2;
        const float inset = f.side * u / 2;
        for (int i = 0; i < kBracketFasteners; ++i)
            fastener(spread(ox0 + inset, ox1 - inset, kBracketFasteners, i), bracketY, kSmallFastenerIn, kBracketFaceZ);
    }

    // Console below the center screen, down to the bottom of the window, with
    // the touch panel set into it in a black surround
    const float cx0 = pic[1][0] - FrameSideIn(1) * u, cx1 = pic[1][2] + FrameSideIn(1) * u;
    const float consoleTop = pic[1][3] + FrameBelowIn(1) * u;
    box(cx0, consoleTop, cx1, H, kConsoleZ, consolePlastic, kRoundedEdgeIn * 2);
    if (has[kTouchPanelScreen])
    {
        const float* p = pic[kTouchPanelScreen];
        const float m = kPanelSurroundIn * u;
        box(p[0] - m, p[1] - m, p[2] + m, p[3] + m, kConsoleZ + 0.2f, blackPlastic, kRoundedEdgeIn);
    }

    // Light cells: each screen cut into a grid of small area lights, each taking
    // its color from the matching part of the game's frame
    const float frameW = g_cfg.renderW > 0 ? static_cast<float>(g_cfg.renderW) : 3840.0f;
    const float frameH = g_cfg.renderH > 0 ? static_cast<float>(g_cfg.renderH) : 2160.0f;
    const float angle = kSideScreenAngleDeg * 3.14159265f / 180;
    for (int s = 0; s < 4; ++s)
    {
        if (!has[s]) continue;
        const float* r = pic[s];
        const bool panel = s == kTouchPanelScreen;
        const int nx = panel ? kCellsPanel[0] : kCellsForward[0], ny = panel ? kCellsPanel[1] : kCellsForward[1];
        float normal[3] = { 0, 0, 1 };
        if (s == 0) { normal[0] = std::sin(angle); normal[2] = std::cos(angle); }
        if (s == 2) { normal[0] = -std::sin(angle); normal[2] = std::cos(angle); }
        if (panel) { normal[1] = -0.6f; normal[2] = 0.8f; }       // lies in the console, facing up and out
        const float z = panel ? kConsoleZ : kGlassZ;
        const float cellW = (r[2] - r[0]) / nx / u, cellH = (r[3] - r[1]) / ny / u;
        const float texels = std::fmax(src[s].sizeX * frameW / nx, src[s].sizeY * frameH / ny);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                CabinetEmitter c = {};
                c.pos[0] = inX(r[0] + (r[2] - r[0]) * (i + 0.5f) / nx);
                c.pos[1] = inY(r[1] + (r[3] - r[1]) * (j + 0.5f) / ny);
                c.pos[2] = z;
                for (int k = 0; k < 3; ++k) c.normal[k] = normal[k];
                c.area = cellW * cellH;
                c.uv[0] = src[s].originX + src[s].sizeX * (i + 0.5f) / nx;
                c.uv[1] = src[s].originY + src[s].sizeY * (j + 0.5f) / ny;
                c.lod = std::log2(std::fmax(texels, 1.0f));
                scene.cells.push_back(c);
            }
    }

    // Booth patches: a box around the cabinet (back wall, side walls, ceiling,
    // floor), each face cut into a grid
    const float bx0 = inX(frameLeft) - kBoothSideIn, bx1 = inX(frameRight) + kBoothSideIn;
    const float by0 = inY(frameTop) - kBoothCeilingIn, by1 = inY(pic[1][3]) + kBoothFloorIn;
    const float bz = kBoothDepthIn;
    const float wallLin[3] = { ToLinear(wall[0]), ToLinear(wall[1]), ToLinear(wall[2]) };
    const float floorLin[3] = { kFloorAlbedo, kFloorAlbedo, kFloorAlbedo };
    // A face spanned by corner + a*axisA + b*axisB, cut into na x nb patches
    auto face = [&](const float corner[3], const float axisA[3], const float axisB[3], int na, int nb,
                    const float normal[3], const float albedo[3])
    {
        const float lenA = std::sqrt(axisA[0] * axisA[0] + axisA[1] * axisA[1] + axisA[2] * axisA[2]);
        const float lenB = std::sqrt(axisB[0] * axisB[0] + axisB[1] * axisB[1] + axisB[2] * axisB[2]);
        for (int j = 0; j < nb; ++j)
            for (int i = 0; i < na; ++i)
            {
                CabinetEmitter p = {};
                for (int k = 0; k < 3; ++k)
                {
                    p.pos[k] = corner[k] + axisA[k] * (i + 0.5f) / na + axisB[k] * (j + 0.5f) / nb;
                    p.normal[k] = normal[k];
                    p.albedo[k] = albedo[k];
                }
                p.area = lenA / na * lenB / nb;
                scene.patches.push_back(p);
            }
    };
    {
        const float back[3] = { bx0, by0, bz }, backA[3] = { bx1 - bx0, 0, 0 }, backB[3] = { 0, by1 - by0, 0 }, backN[3] = { 0, 0, -1 };
        face(back, backA, backB, 8, 4, backN, wallLin);
        const float left[3] = { bx0, by0, 0 }, sideA[3] = { 0, 0, bz }, sideB[3] = { 0, by1 - by0, 0 }, leftN[3] = { 1, 0, 0 };
        face(left, sideA, sideB, 4, 4, leftN, wallLin);
        const float right[3] = { bx1, by0, 0 }, rightN[3] = { -1, 0, 0 };
        face(right, sideA, sideB, 4, 4, rightN, wallLin);
        const float ceil[3] = { bx0, by0, 0 }, flatA[3] = { bx1 - bx0, 0, 0 }, flatB[3] = { 0, 0, bz }, ceilN[3] = { 0, 1, 0 };
        face(ceil, flatA, flatB, 8, 4, ceilN, wallLin);
        const float floor_[3] = { bx0, by1, 0 }, floorN[3] = { 0, -1, 0 };
        face(floor_, flatA, flatB, 8, 4, floorN, floorLin);
    }
    return true;
}
