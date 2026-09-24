#pragma once
#include <string>
#include <vector>

// A screen rectangle as fractions of a frame or window.
struct Rect
{
    float sizeX, sizeY, originX, originY;
};

struct Config
{
    // [Output] the window the screens are drawn into
    int outW = 5120, outH = 1440;       // size in pixels
    int outX = 0, outY = 0;             // position on the desktop
    bool vsync = true;
    bool pixelSnap = true;              // snap screen edges to whole pixels
    int gameWindowMode = 0;             // 0 = keep ours above it (owned), 1 = move it off-screen

    // [Layout] P0..: where each screen is drawn in the output
    std::vector<Rect> dests;

    // [Source] S0..: where each screen is in the game's own frame
    std::vector<Rect> sources;

    // [Touch]
    bool touchEnabled = true;
    std::vector<int> touchScreens{ 3 }; // which screens accept clicks (index into Layout/Source)
    bool hideCursor = false;            // hide the cursor over the output window

    // [Game]
    std::wstring extraCommandLine;      // appended to the game's command line

    // Number of screens that have both a destination and a source
    size_t ScreenCount() const { return dests.size() < sources.size() ? dests.size() : sources.size(); }
};

extern Config g_cfg;

// Loads the ini; missing values keep their defaults (the 3+1 layout at 5120x1440).
void LoadConfig(const std::wstring& iniPath);

// Destination rects with edges snapped to whole output pixels (when PixelSnap
// is on), so neighboring screens share an exact pixel boundary.
std::vector<Rect> SnappedDests();
