#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A screen rectangle as fractions of the window.
struct Rect
{
    float sizeX, sizeY, originX, originY;
};

struct Config
{
    // [Mode]
    bool compositor = true;             // render stock 2x2, rearrange at output

    // [Display] (output size when compositing)
    int resW = 5120;
    int resH = 1440;
    bool pixelSnap = true;
    std::wstring extraCommandLine;      // appended to the game's command line

    // [Output] (compositor)
    int outX = 0, outY = 0;             // output window position on the desktop
    bool outVSync = true;
    int gameWindowMode = 0;             // 0 = keep ours above it (owned), 1 = move it off-screen

    // [Touch] (compositor)
    bool touchEnabled = true;
    std::vector<int> touchRegions{ 3 }; // which screens accept clicks (index into Layout/Source)
    bool touchHideCursor = false;       // hide the cursor over the output window

    // [Layout] P0..P3: destination rects (compositor) or player rects (layout hook)
    std::vector<Rect> players;

    // [Source] S0..S3: regions of the game's frame, as fractions (compositor)
    std::vector<Rect> sources;

    // [UI]
    bool uiScaleEnabled = true;
    int uiReferencePlayer = 0;          // player whose tile should be DesignHeight UI units tall
    float uiDesignHeight = 1080.0f;     // the widgets' design height in UI units

    // [Addresses] (specific to TG4AC-Win64-Shipping.exe 5.80.02)
    uint64_t layoutPlayersRva = 0;      // RVA of the viewport client's LayoutPlayers
    uint64_t dpiScaleRva = 0;           // RVA of UUserInterfaceSettings::GetDPIScaleBasedOnSize
    uint32_t offSplitscreenInfo = 0x60; // TArray<FSplitscreenData> in the viewport client
    uint32_t offActiveType = 0x78;      // ActiveSplitscreenType (byte)
};

extern Config g_cfg;

bool LoadConfig(const std::wstring& iniPath);

// Players' rects with edges snapped to whole pixels at resW x resH
// (plus 0.1px so the engine's truncation can't fall a pixel short).
std::vector<Rect> SnappedLayout();
