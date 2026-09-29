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
    float persp;
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeCameraDistance", L""), persp)) g_cfg.arcadeCameraMm = std::fmax(persp, 0.0f);
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeFov", L""), persp)) g_cfg.arcadeFovDeg = std::fmin(std::fmax(persp, 0.0f), 170.0f);
    float light;
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeCabinetScreenNits", L""), light)) g_cfg.cabinetScreenNits = std::fmax(light, 1.0f);
    if (ParseNumber(ReadString(ini, L"Layout", L"ArcadeCabinetRoomLux", L""), light)) g_cfg.cabinetRoomLux = std::fmax(light, 0.0f);
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

    // Cabinet screen sizes: 55" center, 43" sides (the installation manual's
    // parts lists: "LCD ASSY (55INCH WIDE)" and "(43INCH WIDE)"). The center's
    // width in inches (16:9) converts inches on the cabinet (gaps, frames) to pixels.
    constexpr float kSideScale = 43.0f / 55.0f;
    constexpr float kCenterWidthInches = 55.0f * 16.0f / 18.357560f;   // 18.36 = sqrt(16^2 + 9^2)
    // The side cabinets' faces turn this far toward the player from the corner
    // where they meet the center cabinet (the manual's plan view, page 10)
    constexpr float kFaceAngleDeg = 55.0f;

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
    constexpr float Mm(float mm) { return mm / 25.4f; }

    struct FrameSpec
    {
        float border;               // the monitor's black border, visible around the picture
        float topRail;              // frame above the opening (the picture and its border)
        float side;                 // each side: the rail, or the bracket (both flanges)
        float bottomRail;           // frame below the opening
        bool brackets;              // Z brackets down the sides instead of side rails
        bool bottomBracket;         // a Z bracket below the frame, holding it to the wall
        // Fasteners, placed in mm from the frame's outer edges
        int topFasteners;           // along the top, spread evenly...
        float topInsetMm;           // ...from this far in from each end
        float topDropMm;            // this far down from the top
        int sideFasteners;          // down each side...
        const float* sideDropsMm;   // ...this far down from the top
        float sideInsetMm;          // this far in from the side
        float fastenerMm;           // head diameter
    };

    // The center screen, from the manual's head-on drawing of the center cabinet
    // (page 124, "CENTER VIDEO CABINET ASSY [15]", scaled by its 1300 mm width):
    // the monitor area (the picture) spans 1210.8 x 678.1 mm between the "CENTER
    // MONITOR PANEL BRACKET-L/R", each 44.4 mm wide (21.5 mm wall flange, then a
    // 22.9 mm raised flange), with 4 M4 truss-head screws down each wall flange.
    // It runs to 6.3 mm below the cabinet's top edge, and 12.7 mm of frame shows
    // below it. The 5 screws across its top hold the acrylic monitor panel, so
    // they belong with the acrylic (not drawn yet).
    constexpr float kCenterSideDropsMm[4] = { 20.3f, 255.6f, 490.8f, 670.3f };

    // The side screens, from the manual's three-quarter drawings of the side
    // cabinets (pages 144-145, "SIDE VIDEO CABINET ASSY-L/R [1]"), un-foreshortened
    // using the drawing's three axes and scaled by the cabinet face (the 1035 x
    // 650 mm footprint's long side, 1222 mm; it also matches the 1955 mm height).
    // The "SIDE MONITOR PANEL" (acrylic) is 994.4 x 598.3 mm over the 43" picture
    // (952.4 x 535.7 mm), held by 6 M5 truss-head screws: 4 along the top, 13.3 mm
    // down, spread from 43.3 mm in from each end, and 1 per side, 9.4 mm in and
    // 312.5 mm down. The "SIDE MONITOR PANEL STOPPER" bar (about 20 mm tall, 3
    // screws) sits under it. No black bezel shows: photos of the cabinet show paint
    // from the picture's edge to the acrylic's, with the side screws centered in
    // that 21 mm strip.
    //
    // Screws (all chrome Torx tamper-proof truss heads, per the parts lists; a
    // truss head is about twice the thread across):
    //   - center brackets: M4x10, 8 mm heads
    //   - center acrylic, along the top: M4x10 on a 14 mm chrome washer (with the acrylic, later)
    //   - side acrylic and its stopper bar: M5x10, 10 mm heads
    constexpr float kSideSideDropsMm[1] = { 312.5f };

    constexpr FrameSpec kSideFrame = { 0.0f, Mm(31.3f), Mm(21.0f), Mm(31.3f), false, true,
                                       4, 43.3f, 13.3f, 1, kSideSideDropsMm, 9.4f, 10.0f };
    constexpr FrameSpec kFrames[3] = {
        kSideFrame,                                                                                     // left
        { 0.0f, Mm(6.3f), Mm(44.4f), Mm(12.7f), true, false, 0, 0, 0, 4, kCenterSideDropsMm, 12.8f, 8.0f }, // center
        kSideFrame,                                                                                     // right
    };
    // Across a side bracket (its width is FrameSpec::side): wall flange, the bend
    // up, raised flange (the manual: 21.5 and 22.9 mm, meeting at a bend)
    constexpr float kFlangeParts[3] = { 20.5f, 2.0f, 21.9f };
    constexpr float kBracketIn = Mm(20.0f);     // height of the bracket below a frame (the side screens' stopper bar)
    constexpr float kBracketLipIn = Mm(4.0f);   // its lip, along the top
    constexpr float kSeamIn = 0.06f;            // width of a visible seam
    constexpr float kBracketFastenerIn = Mm(10.0f);   // the stopper bar's screws: M5 truss heads, as on the side panel
    constexpr int kBracketFasteners = 3;        // along the bottom bracket: both ends and the middle
    constexpr float kPanelSurroundIn = 0.5f;    // black surround of the touch panel in the console

    // The acrylic "MONITOR PANEL"s (PMMA, per the parts lists), about half an
    // inch thick. The side screens' covers their whole frame (994.4 x 598.3 mm),
    // screwed on over the rails. The center screen's spans the picture between
    // its brackets, from the frame's top to its bottom, on 5.5 mm spacers over
    // the brackets' raised flanges.
    constexpr float kAcrylicIn = 0.5f;

    // The hood under the center screen: HOOD-L and HOOD-R (boxes of black
    // painted steel, rounded over along their top outer edges, the front
    // speakers set into their fronts) and HOOD-(CENTER) (a plate between them,
    // over the touch monitor). It reaches out from the center screen toward the
    // player at about shoulder height, its top covering the bottom 1/30 of the
    // picture (measured on a real cabinet). From the manual's front view of the
    // center cabinet (page 124, to scale): 1300 mm across, its front showing
    // 245 mm down to the control panel's front edge, the pieces meeting 340 mm
    // in from each end, the speaker grilles 75 to 290 mm in and 44 to 211 mm
    // down, and the touch monitor's opening 481 x 183 mm, 40 mm down. From the
    // exploded views (page 122; estimated): about 350 mm deep, its front
    // leaning back about 75 mm over its 317 mm height.
    constexpr float kHoodCoverFraction = 1.0f / 30;
    constexpr float kHoodFaceMm = 245.0f;
    constexpr float kHoodHeightMm = 317.0f;
    constexpr float kHoodDepthMm = 350.0f;
    constexpr float kHoodLeanMm = 75.0f;
    constexpr float kHoodCornerMm = 30.0f;
    constexpr float kHoodSplitMm = 340.0f;
    constexpr float kHoodGrilleMm[4] = { 75.0f, 44.0f, 290.0f, 211.0f };      // in from the end, down: from, to
    constexpr float kTouchOpeningMm[3] = { 481.0f, 183.0f, 40.0f };           // width, height, down from the top
    constexpr float kHoodAlbedo = 0.045f;       // black paint (sRGB)
    constexpr float kCenterAcrylicSpacerIn = 5.5f / 25.4f;

    // Room each screen's frame takes around its picture, in inches
    constexpr float FrameSideIn(int s) { return kFrames[s].side + kFrames[s].border; }
    constexpr float FrameAboveIn(int s) { return kFrames[s].topRail + kFrames[s].border; }
    constexpr float FrameBelowIn(int s)
    {
        return kFrames[s].border + kFrames[s].bottomRail + (kFrames[s].bottomBracket ? kBracketIn : 0.0f);
    }

    // Whether the arcade layout's cabinet is seen in perspective (it uses the
    // cabinet's geometry)
    bool PerspectiveOn()
    {
        return g_cfg.arcadeLayout && g_cfg.arcadeCabinet && g_cfg.arcadeCameraMm > 0;
    }

    // The perspective camera's distance from the center screen, in inches
    float CameraIn()
    {
        return std::fmax(g_cfg.arcadeCameraMm, 50.0f) / 25.4f;
    }

    // Seen from the camera, what a side face shows at x (from the center
    // picture's middle, past the corner c0; all in the same units): how far
    // along the face from the corner, and t, the fraction of the way along the
    // camera ray to the face (flat drawing = view * t, vertically). The face
    // turns by sa, ca toward a camera Z from the center screen.
    void FaceAt(float x, float c0, float sa, float ca, float Z, float& along, float& t)
    {
        t = (sa * c0 + ca * Z) / (sa * x + ca * Z);
        along = ca * (t * x - c0) + sa * Z * (1 - t);
    }

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
        const float rowWidth = 1 + 2 * kSideScale + 2 * gapScale                              // 3 screens, 2 gaps,
                       + 2 * (sideScale[0] + sideScale[1] + sideScale[2]);              // 6 frame sides
        const float rowHeight = 9.0f / 16.0f + aboveScale + belowScale;                 // center screen and its frame
        // Center screen: limited by the width or the height (left over under the
        // panel, or all of it when overlap is allowed)
        const bool perspective = PerspectiveOn();
        const float widthLimit = W / rowWidth;
        const bool overlap = !perspective && withPanel && g_cfg.allowPanelOverlap &&
                             std::fmin(widthLimit, H / rowHeight) * rowHeight + panelH > H;
        float centerW = std::fmin(widthLimit, (overlap ? H : H - panelH) / rowHeight);

        // With perspective (ArcadeCameraDistance), the view is a camera's: the
        // side frames, turned toward it, grow toward their outer edges, and
        // whatever falls outside its field of view is cut off. The center screen
        // is sized by that field of view (ArcadeFov), or by default fills the
        // window: its frame (with the panel below) fits the height, and the
        // frame out to the corners the width.
        float middleY = 0;                                                               // the center picture's middle
        float panelTopY = 0;                                                             // the panel's top, from the picture's middle
        if (perspective)
        {
            const float camera = CameraIn() / kCenterWidthInches;                        // in center widths
            const float colAbove = 9.0f / 32.0f + aboveScale;
            float colBelow = 9.0f / 32.0f + belowScale;
            if (withPanel)
            {
                // The panel goes where the hood's opening shows: past the hood's
                // top, as seen from the camera, and down its front
                const float cw = kCenterWidthInches * 25.4f;                             // mm per center width
                const float down = kTouchOpeningMm[2];
                const float y = 9.0f / 32.0f - 9.0f / 16.0f * kHoodCoverFraction + down / cw;
                const float z = (kHoodDepthMm + kHoodLeanMm * down / kHoodHeightMm) / cw;
                colBelow = std::fmax(colBelow, y * camera / std::fmax(camera - z, 0.01f));
            }
            panelTopY = colBelow;
            if (g_cfg.arcadeFovDeg > 0)
                centerW = W / (2 * camera * std::tan(g_cfg.arcadeFovDeg * 3.14159265f / 360));
            else
                centerW = std::fmin(W / (1 + 2 * sideScale[1]), (H - panelH) / (colAbove + colBelow));

            // How tall the side frames get where they leave the view (or end)
            const float angle = kFaceAngleDeg * 3.14159265f / 180, ca = std::cos(angle), sa = std::sin(angle);
            const float corner = 0.5f + sideScale[1];
            const float span = kSideScale + 2 * sideScale[0];                            // a side frame, flat
            float along = span, t = 1;
            const float edge = W / 2 / centerW;
            if (edge > corner) FaceAt(edge, corner, sa, ca, camera, along, t);
            else along = 0;
            along = std::fmin(std::fmax(along, 0.0f), span);
            const float k = camera / std::fmax(camera - along * sa, 0.01f);
            const float sideTop = 9.0f / 32.0f - 9.0f / 16.0f * kSideScale - scale(FrameAboveIn(0));
            const float sideBottom = 9.0f / 32.0f + scale(FrameBelowIn(0));

            // Center everything vertically when it all fits; otherwise, as close
            // to that as keeps the center column (and the panel) in view
            const float allAbove = centerW * std::fmax(colAbove, -sideTop * k);
            const float allBelow = std::fmax(centerW * colBelow + panelH, centerW * sideBottom * k);
            const float colTop = centerW * colAbove, colBottom = centerW * colBelow + panelH;
            middleY = (H - (allAbove + allBelow)) / 2 + allAbove;
            if (colTop + colBottom <= H)
                middleY = std::fmin(std::fmax(middleY, colTop), H - colBottom);
            else
                middleY = (H - (colTop + colBottom)) / 2 + colTop;
        }
        const float centerH = centerW * 9.0f / 16.0f;
        const float sideW = centerW * kSideScale, sideH = centerH * kSideScale;
        const float gap = centerW * gapScale;
        const float above = centerW * aboveScale, below = centerW * belowScale;

        float top = (overlap ? 0.0f : (H - (centerW * rowHeight + panelH)) / 2) + above;
        if (perspective) top = middleY - centerW * 9.0f / 32.0f;
        const float bottom = top + centerH;    // shared bottom edge of the forward screens
        const float panelTop = overlap ? H - panelH : perspective ? middleY + centerW * panelTopY : bottom + below;

        // Pixel edges, left to right: left screen, frames and gap, center screen, frames and gap, right screen
        auto px = [](float v) { return static_cast<float>(std::floor(v + 0.5f)); };
        const float betweenL = centerW * (sideScale[0] + sideScale[1]) + gap;   // left picture to center picture
        const float betweenR = centerW * (sideScale[1] + sideScale[2]) + gap;   // center picture to right picture
        const float centerL = (W - centerW) / 2, centerR = centerL + centerW;
        const float left = centerL - betweenL - sideW;                          // left screen's picture
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

    // The booth, from the manual (page 10: 2600 x 1590 x 2160 mm overall; the
    // plan view and the units' sizes). The front row is a side cabinet, the center
    // cabinet and a side cabinet (650 + 1300 + 650 = 2600 mm), so the flat layout's
    // corner (where the side frames meet the center frame) is the real corner. The
    // side cabinets' faces turn 55 degrees toward the player from there (the plan's
    // diagonals; their 1035 x 650 mm footprint gives 58). Inside: side walls 1191 mm
    // either side of the middle, the back wall (the seat cabinet) 1317 mm behind the
    // screens, and the ceiling (under the ~90 mm roof) about 2070 mm up. The center
    // picture's middle is about 1495 mm up (the monitor ends 6 mm below the 1840 mm
    // center cabinet's top). All of it is the cabinet's paint; the floor is dark.
    constexpr float kBoothHalfWidthMm = 1191.0f;
    constexpr float kBackWallMm = 1317.0f;
    constexpr float kCeilingMm = 2070.0f;
    constexpr float kPictureCenterMm = 1495.0f;
    constexpr float kFloorAlbedo = 0.08f;       // linear

    // Light cells per screen (across, down)
    constexpr int kCellsForward[2] = { 8, 4 };
    constexpr int kCellsPanel[2] = { 4, 2 };

    float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
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
    const Material chrome = { { 0.80f, 0.80f, 0.81f }, 0.3f, 1.0f, false };   // the screws: chrome, about 60% reflective

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
        add({ CabinetShape::Dome, x, y, r, 0, z, z, diameter * 0.3f, 0 }, chrome);
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
        const float in0 = r[0] - f.border * u, in1 = r[2] + f.border * u;
        const float iy0 = r[1] - f.border * u, iy1 = r[3] + f.border * u;
        const float oy1 = iy1 + f.bottomRail * u;               // bottom of the frame
        const float by1 = oy1 + (f.bottomBracket ? kBracketIn : 0.0f) * u;   // bottom of the bracket below it
        const float lip = kBracketLipIn * u;
        frameLeft = std::fmin(frameLeft, ox0);
        frameRight = std::fmax(frameRight, ox1);
        frameTop = std::fmin(frameTop, oy0);

        // The acrylic over it all
        if (f.brackets) scene.acrylics.push_back({ in0, oy0, in1, oy1, kFlangeHighZ + kCenterAcrylicSpacerIn + kAcrylicIn });
        else scene.acrylics.push_back({ ox0, oy0, ox1, oy1, kRailZ + kAcrylicIn });

        // Rails (one piece on the side screens), and the monitor's border in the opening
        box(ox0, oy0, ox1, oy1, kRailZ, railPaint, kRoundedEdgeIn);
        if (f.border > 0) box(in0, iy0, in1, iy1, kBorderZ, blackPlastic);
        box(r[0], r[1], r[2], r[3], kGlassZ, blackPlastic);   // under the picture

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
        }

        // Bottom bracket: its lip sloping down from the frame, then its face
        if (f.bottomBracket)
        {
            ramp(ox0, oy1, ox1, oy1 + lip, kRailZ, kBracketFaceZ, 1, railPaint);
            box(ox0, oy1 + lip, ox1, by1, kBracketFaceZ, railPaint, kRoundedEdgeIn / 2);
        }

        // Fasteners, where the manual puts them (mm from the frame's outer edges)
        const float mmPx = u / 25.4f;
        for (int i = 0; i < f.topFasteners; ++i)
            fastener(spread(ox0 + f.topInsetMm * mmPx, ox1 - f.topInsetMm * mmPx, f.topFasteners, i),
                     oy0 + f.topDropMm * mmPx, Mm(f.fastenerMm), kRailZ);
        const float sideZ = f.brackets ? kFlangeLowZ : kRailZ;   // brackets: on the wall flanges
        for (int i = 0; i < f.sideFasteners; ++i)
        {
            const float y = oy0 + f.sideDropsMm[i] * mmPx;
            fastener(ox0 + f.sideInsetMm * mmPx, y, Mm(f.fastenerMm), sideZ);
            fastener(ox1 - f.sideInsetMm * mmPx, y, Mm(f.fastenerMm), sideZ);
        }
        if (f.bottomBracket)
        {
            const float bracketY = (oy1 + lip + by1) / 2;
            const float inset = f.side * u / 2;
            for (int i = 0; i < kBracketFasteners; ++i)
                fastener(spread(ox0 + inset, ox1 - inset, kBracketFasteners, i), bracketY, kBracketFastenerIn, kBracketFaceZ);
        }
    }

    // Console below the center screen, down to the bottom of the window. The
    // hood over it, and the touch panel's surround, are drawn over everything
    // later, in 3D (CabinetLightHood); this is what's under it.
    const float cx0 = pic[1][0] - FrameSideIn(1) * u, cx1 = pic[1][2] + FrameSideIn(1) * u;
    const float consoleTop = pic[1][3] + FrameBelowIn(1) * u;
    box(cx0, consoleTop, cx1, H, kConsoleZ, consolePlastic, kRoundedEdgeIn * 2);
    scene.hoodTop = inY(pic[1][3]) - (pic[1][3] - pic[1][1]) / u * kHoodCoverFraction;
    scene.hoodDepth = Mm(kHoodDepthMm);
    scene.hoodSlope = kHoodLeanMm / kHoodHeightMm;
    scene.hoodHeight = Mm(kHoodHeightMm);
    scene.hoodFace = Mm(kHoodFaceMm);
    scene.hoodSplit = Mm(kHoodSplitMm);
    for (int i = 0; i < 4; ++i) scene.hoodGrille[i] = Mm(kHoodGrilleMm[i]);
    scene.hoodCorner = Mm(kHoodCornerMm);
    scene.hoodOpening[0] = Mm(kTouchOpeningMm[0] / 2);
    scene.hoodOpening[1] = Mm(kTouchOpeningMm[2]);
    scene.hoodOpening[2] = Mm(kTouchOpeningMm[2] + kTouchOpeningMm[1]);
    scene.hoodAlbedo = ToLinear(kHoodAlbedo);
    if (has[kTouchPanelScreen])
        for (int i = 0; i < 4; ++i) scene.panelRect[i] = pic[kTouchPanelScreen][i];
    scene.panelSurround = kPanelSurroundIn * u;

    // The cabinet in 3D, for the lighting (it's drawn flat): past the corner,
    // the flat layout continues along the side cabinets' faces, which turn
    // toward the player. place() gives a point's 3D position and its surface's
    // axes (ex: along the flat layout's x, ez: out of the surface).
    const float corner = inX(pic[1][2] + FrameSideIn(1) * u);   // the corner, inches from the middle
    const float faceAngle = kFaceAngleDeg * 3.14159265f / 180;
    const float ca = std::cos(faceAngle), sa = std::sin(faceAngle);
    scene.cornerIn = corner;
    scene.faceAngle = faceAngle;
    struct Placed { float p[3], ex[3], ez[3]; };
    auto place = [&](float fx, float fy, float zLocal)
    {
        Placed q = { { fx, fy, zLocal }, { 1, 0, 0 }, { 0, 0, 1 } };
        const float ax = std::fabs(fx);
        if (ax > corner)
        {
            const float sgn = fx < 0 ? -1.0f : 1.0f, along = ax - corner;
            const float ex[3] = { ca, 0, sgn * sa }, ez[3] = { -sgn * sa, 0, ca };
            for (int k = 0; k < 3; ++k)
            {
                q.ex[k] = ex[k];
                q.ez[k] = ez[k];
                q.p[k] = (k == 0 ? sgn * corner : k == 1 ? fy : 0.0f) + sgn * ex[k] * along + ez[k] * zLocal;
            }
        }
        return q;
    };

    // Light cells: each screen cut into a grid of small area lights, each taking
    // its color from the matching part of the game's frame
    const float frameW = g_cfg.renderW > 0 ? static_cast<float>(g_cfg.renderW) : 3840.0f;
    const float frameH = g_cfg.renderH > 0 ? static_cast<float>(g_cfg.renderH) : 2160.0f;
    for (int s = 0; s < 4; ++s)
    {
        if (!has[s]) continue;
        const float* r = pic[s];
        const bool panel = s == kTouchPanelScreen;
        const int nx = panel ? kCellsPanel[0] : kCellsForward[0], ny = panel ? kCellsPanel[1] : kCellsForward[1];
        const float cellW = (r[2] - r[0]) / nx / u, cellH = (r[3] - r[1]) / ny / u;
        const float texels = std::fmax(src[s].sizeX * frameW / nx, src[s].sizeY * frameH / ny);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                CabinetEmitter c = {};
                const float fx = inX(r[0] + (r[2] - r[0]) * (i + 0.5f) / nx), fy = inY(r[1] + (r[3] - r[1]) * (j + 0.5f) / ny);
                if (panel)
                {
                    // Lies in the console, facing up and out
                    c.pos[0] = fx; c.pos[1] = fy; c.pos[2] = kConsoleZ;
                    c.normal[1] = -0.6f; c.normal[2] = 0.8f;
                }
                else
                {
                    const Placed q = place(fx, fy, kGlassZ);
                    for (int k = 0; k < 3; ++k) { c.pos[k] = q.p[k]; c.normal[k] = q.ez[k]; }
                }
                c.area = cellW * cellH;
                c.uv[0] = src[s].originX + src[s].sizeX * (i + 0.5f) / nx;
                c.uv[1] = src[s].originY + src[s].sizeY * (j + 0.5f) / ny;
                c.lod = std::log2(std::fmax(texels, 1.0f));
                scene.cells.push_back(c);
            }
    }

    // Booth patches: the booth's surfaces cut into grids. They're lit by the
    // cells and by each other (several bounces), and light the cabinet.
    const float mmIn = 1 / 25.4f;
    const float halfW = kBoothHalfWidthMm * mmIn, back = kBackWallMm * mmIn;
    const float ceilY = -(kCeilingMm - kPictureCenterMm) * mmIn, floorY = kPictureCenterMm * mmIn;
    const float faceLen = (halfW - corner) / ca;                  // along each side cabinet's face, to the side wall
    const float faceEndZ = faceLen * sa;
    scene.faceLenIn = faceLen;
    scene.boothHalfWidth = halfW;
    scene.boothBack = back;
    scene.boothCeiling = ceilY;
    scene.boothFloor = floorY;
    scene.floorAlbedo = kFloorAlbedo;
    scene.acrylicIn = kAcrylicIn;
    // Who's looking: the perspective view's camera, or else the seated player
    // (about 1200 mm up and 800 mm back)
    scene.eye[0] = 0;
    scene.eye[1] = PerspectiveOn() ? 0.0f : 11.6f;
    scene.eye[2] = PerspectiveOn() ? CameraIn() : 31.5f;
    const float wallLin[3] = { ToLinear(wall[0]), ToLinear(wall[1]), ToLinear(wall[2]) };
    const float floorLin[3] = { kFloorAlbedo, kFloorAlbedo, kFloorAlbedo };
    const float glassLin[3] = { 0.03f, 0.03f, 0.03f };           // a screen's glass, or the dark console
    auto addPatch = [&](const float pos[3], const float normal[3], float area, const float albedo[3])
    {
        CabinetEmitter p = {};
        for (int k = 0; k < 3; ++k) { p.pos[k] = pos[k]; p.normal[k] = normal[k]; p.albedo[k] = albedo[k]; }
        p.area = area;
        scene.patches.push_back(p);
    };
    // A flat face spanned by corner + a*axisA + b*axisB, cut into na x nb patches
    auto face = [&](const float c0[3], const float axisA[3], const float axisB[3], int na, int nb,
                    const float normal[3], const float albedo[3])
    {
        const float lenA = std::sqrt(axisA[0] * axisA[0] + axisA[1] * axisA[1] + axisA[2] * axisA[2]);
        const float lenB = std::sqrt(axisB[0] * axisB[0] + axisB[1] * axisB[1] + axisB[2] * axisB[2]);
        for (int j = 0; j < nb; ++j)
            for (int i = 0; i < na; ++i)
            {
                float pos[3];
                for (int k = 0; k < 3; ++k) pos[k] = c0[k] + axisA[k] * (i + 0.5f) / na + axisB[k] * (j + 0.5f) / nb;
                addPatch(pos, normal, lenA / na * lenB / nb, albedo);
            }
    };
    // Is a flat-layout point (inches) on a screen or the console? Those parts of
    // the front are dark rather than painted.
    auto dark = [&](float fx, float fy)
    {
        const float x = scene.originX + fx * u, y = scene.originY + fy * u;
        for (int s = 0; s < 3; ++s)
            if (has[s] && x >= pic[s][0] && x <= pic[s][2] && y >= pic[s][1] && y <= pic[s][3]) return true;
        return y >= consoleTop && std::fabs(fx) <= corner;
    };
    {
        const float h = floorY - ceilY;
        const float backC[3] = { -halfW, ceilY, back }, backA[3] = { 2 * halfW, 0, 0 }, backB[3] = { 0, h, 0 }, backN[3] = { 0, 0, -1 };
        face(backC, backA, backB, 8, 4, backN, wallLin);
        const float sideA[3] = { 0, 0, back - faceEndZ }, sideB[3] = { 0, h, 0 };
        const float leftC[3] = { -halfW, ceilY, faceEndZ }, leftN[3] = { 1, 0, 0 };
        face(leftC, sideA, sideB, 3, 4, leftN, wallLin);
        const float rightC[3] = { halfW, ceilY, faceEndZ }, rightN[3] = { -1, 0, 0 };
        face(rightC, sideA, sideB, 3, 4, rightN, wallLin);
        const float ceilC[3] = { -halfW, ceilY, 0 }, flatA[3] = { 2 * halfW, 0, 0 }, flatB[3] = { 0, 0, back }, ceilN[3] = { 0, 1, 0 };
        face(ceilC, flatA, flatB, 6, 4, ceilN, wallLin);
        const float floorC[3] = { -halfW, floorY, 0 }, floorN[3] = { 0, -1, 0 };
        face(floorC, flatA, flatB, 6, 4, floorN, floorLin);

        // The front: the center wall (4 x 5), and each side cabinet's face (3 x 4)
        const int fcx = 4, fcy = 5;
        for (int j = 0; j < fcy; ++j)
            for (int i = 0; i < fcx; ++i)
            {
                const float fx = -corner + 2 * corner * (i + 0.5f) / fcx, fy = ceilY + h * (j + 0.5f) / fcy;
                const float pos[3] = { fx, fy, 0 }, n[3] = { 0, 0, 1 };
                addPatch(pos, n, 2 * corner / fcx * h / fcy, dark(fx, fy) ? glassLin : wallLin);
            }
        for (int side = -1; side <= 1; side += 2)
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 3; ++i)
                {
                    const float fx = side * (corner + faceLen * (i + 0.5f) / 3), fy = ceilY + h * (j + 0.5f) / 4;
                    const Placed q = place(fx, fy, 0);
                    addPatch(q.p, q.ez, faceLen / 3 * h / 4, dark(fx, fy) ? glassLin : wallLin);
                }
    }
    return true;
}

bool GetPerspective(int width, int height, PerspectiveView& v)
{
    v = {};
    if (!PerspectiveOn() || width <= 0 || height <= 0) return false;
    const float W = static_cast<float>(width), H = static_cast<float>(height);
    Rect c = {};
    bool found = false;
    for (const Placement& p : Placements(kMainWindow, width, height))
        if (p.screen == 1) { c = p.dest; found = true; }
    if (!found) return false;
    v.pxPerInch = c.sizeX * W / kCenterWidthInches;
    v.originX = (c.originX + c.sizeX / 2) * W;
    v.originY = (c.originY + c.sizeY / 2) * H;
    v.cornerIn = kCenterWidthInches / 2 + FrameSideIn(1);
    const float angle = kFaceAngleDeg * 3.14159265f / 180;
    v.sinA = std::sin(angle);
    v.cosA = std::cos(angle);
    v.cameraIn = CameraIn();
    v.fovDeg = 2 * std::atan(W / 2 / v.pxPerInch / v.cameraIn) * 57.29578f;

    // How far past the output's edges the flat drawing reaches, for what the
    // output's edges show: the canvas is widened by that much on each side
    // (the cabinet's wall continues past the side frames, as it does flat)
    const float edge = std::fmax(v.originX, W - v.originX) / v.pxPerInch;   // inches, to the farther edge
    float flatEdge = edge;
    if (edge > v.cornerIn)
    {
        float along, t;
        FaceAt(edge, v.cornerIn, v.sinA, v.cosA, v.cameraIn, along, t);
        flatEdge = v.cornerIn + along;
    }
    const int widest = (16384 - width) / 2 - 2;                                  // the largest texture there is
    v.marginPx = static_cast<int>(std::ceil(std::fmax(0.0f, (flatEdge - edge) * v.pxPerInch))) + 2;
    if (v.marginPx > widest) v.marginPx = widest;
    return true;
}
