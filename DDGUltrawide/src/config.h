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
    bool arcadeCabinet = true;          // draw the cabinet (wall, screen frames, console) around the screens
    float cabinetColor[3] = { 0xBA / 255.0f, 0xB3 / 255.0f, 0xA5 / 255.0f };   // the cabinet's beige-gray
    float cabinetScreenNits = 1000.0f;  // the screens' white, cd/m^2
    float cabinetRoomLux = 0.0f;        // the arcade's own lighting falling on the cabinet, lux (0 = none)

    // [Source] S0..: where each screen is in the game's own frame
    std::vector<Rect> sources;

    // [TouchPanelWindow] optional second window, full screen on its own monitor,
    // that shows the touch panel (screen 3) instead of the main output
    bool panelWindow = false;
    int panelMonitor = 0;               // Windows display number; 0 = first one not showing the main output
    int panelX = 0, panelY = 0;         // explicit placement, used when panelW and panelH > 0
    int panelW = 0, panelH = 0;
    bool panelVsync = false;            // off so the two windows don't wait on each other
    Rect panelDest{ 1.0f, 1.0f, 0.0f, 0.0f };      // where the panel is drawn in its window...
    bool panelDestFit = true;                      // ...unless Layout is left out: fit, keeping the aspect ratio
    Rect panelSource{ 0.5f, 822.0f / 2160, 0.5f, 0.5f };   // where the panel is in the game's frame (the used rows)

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

// Rows of the touch panel's 1080 that are actually used; the rest is covered by
// a matte on the cabinet, so it isn't drawn.
constexpr float kPanelVisibleRows = 822.0f;

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

// One surface of the cabinet drawn behind the arcade layout (ArcadeCabinet):
// where it is in the output (pixels), how far it stands out from the wall
// (inches), and what it's made of. The lighting (cabinetlight.cpp) shades them.
struct CabinetShape
{
    enum Kind
    {
        Box,        // flat at z0, its edges rounded over across `edge` pixels
        Ramp,       // sloping from z0 to z1: left to right (axis 0) or top to bottom (axis 1)
        Dome,       // a fastener head: centered at x0, y0 with radius x1, rising `edge` inches above z0
    } kind;
    float x0, y0, x1, y1;       // Box, Ramp: left, top, right, bottom
    float z0, z1;               // height above the wall, inches
    float edge;                 // Box: rounded edge width (pixels). Dome: head height (inches)
    int axis;                   // Ramp: 0 = across, 1 = down
    float albedo[3];            // 0..1, sRGB
    float roughness;            // 0 = mirror, 1 = matte
    float metalness;            // 0 = paint or plastic, 1 = metal
    bool powderCoat;            // satin powder coat: a fine orange-peel texture
};

// A light source or bounce surface for the cabinet's lighting, in cabinet
// inches (x right, y down, z out of the wall toward the player; the origin is
// the center of the center screen's picture, on the wall).
struct CabinetEmitter
{
    float pos[3];               // center
    float normal[3];            // the side it emits from
    float area;                 // square inches
    float uv[2];                // screen cell: where it is in the game's frame
    float lod;                  // screen cell: mip level covering it with about one texel
    float albedo[3];            // wall patch: its (linear) color
};

// Everything the cabinet's lighting needs for a width x height main window.
struct CabinetScene
{
    std::vector<CabinetShape> shapes;       // back to front
    std::vector<CabinetEmitter> cells;      // the game screens, cut into small area lights
    std::vector<CabinetEmitter> patches;    // the booth's walls, ceiling and floor, lit by the cells
    float pxPerInch;
    float originX, originY;                 // the origin, in output pixels
    float cornerIn;                         // where the side cabinets' faces start, inches either side of the middle
    float faceAngle;                        // how far they turn toward the player (radians)
};

// The cabinet (wall, screen frames, brackets, fasteners, console) and its
// lighting setup. False when it isn't drawn.
bool GetCabinetScene(int width, int height, CabinetScene& scene);
