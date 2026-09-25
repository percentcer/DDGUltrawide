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
    // [Layout] ArcadeLayout: compute the layout from the cabinet's screen sizes
    // instead (ignores P0.. and [Source])
    bool arcadeLayout = true;
    float arcadeGap = 2.0f;             // gap between the forward screens, in cabinet inches
    bool allowPanelOverlap = false;     // let the touch panel cover the center screen's bottom edge
    float panelScaling = 0.75f;         // touch panel size, relative to a third of the output's height

    // [Source] S0..: where each screen is in the game's own frame
    std::vector<Rect> sources;

    // [TouchPanelWindow] optional second window, full screen on its own monitor,
    // that shows the touch panel (screen 3) instead of the main output
    bool panelWindow = false;
    int panelMonitor = 0;               // Windows display number; 0 = first one not showing the main output
    int panelX = 0, panelY = 0;         // explicit placement, used when panelW and panelH > 0
    int panelW = 0, panelH = 0;
    bool panelVsync = false;            // off so the two windows don't wait on each other
    Rect panelDest{ 1.0f, 1.0f, 0.0f, 0.0f };      // where the panel is drawn in its window
    Rect panelSource{ 0.5f, 0.5f, 0.5f, 0.5f };    // where the panel is in the game's frame

    // [Touch]
    bool touchEnabled = true;
    std::vector<int> touchScreens{ 3 }; // which screens accept clicks (index into Layout/Source)
    bool hideCursor = false;            // hide the cursor over the output window

    // [Game]
    int renderW = 3840, renderH = 2160; // size to make the game's window (0 = leave it alone)
    std::wstring extraCommandLine;      // appended to the game's command line

    // Number of screens that have both a destination and a source
    size_t ScreenCount() const { return dests.size() < sources.size() ? dests.size() : sources.size(); }
};

extern Config g_cfg;

// Loads the ini; missing values keep their defaults (the 3+1 layout at 5120x1440).
void LoadConfig(const std::wstring& iniPath);

// The screen that is the touch panel (bottom-right of the cabinet's 2x2 frame).
constexpr int kTouchPanelScreen = 3;

// Our output windows.
enum OutputWindow { kMainWindow = 0, kPanelWindow = 1, kWindowCount = 2 };

// One screen drawn into one of our windows.
struct Placement
{
    int screen;     // index into [Layout]/[Source]
    Rect dest;      // fractions of the window, edges snapped to whole pixels when PixelSnap is on
    Rect source;    // fractions of the game's frame
};

// The screens drawn into a window of the given client size. With the touch
// panel window enabled, screen 3 moves from the main window to that window.
std::vector<Placement> Placements(int window, int width, int height);
