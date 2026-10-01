#pragma once
#include <string>
#include <array>
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
    float arcadeCameraMm = 2250.0f;     // perspective camera in front of the center screen (0 = flat)
    float arcadeFovDeg = 0.0f;          // its horizontal field of view (0 = the center screen fills the window)
    float cabinetColor[3] = { 0xBA / 255.0f, 0xB3 / 255.0f, 0xA5 / 255.0f };   // the cabinet's beige-gray
    float cabinetScreenNits = 1000.0f;  // the screens' white, cd/m^2
    float cabinetRoomLux = 0.0f;        // the arcade's own lighting falling on the cabinet, lux (0 = none)
    float roofLights = 1.0f;            // the booth's roof LED panels, in screen whites (0 = off)
    float roofLightGlowMm = 25.0f;      // their soft edge in reflections, mm
    float hoodGlossDeg = 1.15f;         // how far the hood's paint blurs its reflections, degrees

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
    float emission;             // wall patch: light it gives off itself (a window, a roof light), in screen whites
};

// Everything the cabinet's lighting needs for a width x height main window.
// A clear acrylic sheet over a screen and its frame, which reflects the booth
struct CabinetAcrylic
{
    float x0, y0, x1, y1;                   // output pixels
    float z;                                // its front face (inches out from the wall)
};

struct CabinetScene
{
    std::vector<CabinetShape> shapes;       // back to front
    std::vector<CabinetEmitter> cells;      // the game screens, cut into small area lights
    std::vector<CabinetEmitter> patches;    // the booth's walls, ceiling and floor, lit by the cells
    std::vector<CabinetAcrylic> acrylics;   // drawn over the screens
    float pxPerInch;
    float originX, originY;                 // the origin, in output pixels
    float cornerIn;                         // where the side cabinets' faces start, inches either side of the middle
    float faceAngle;                        // how far they turn toward the player (radians)
    float faceLenIn;                        // how far those faces run, to the booth's side walls
    // The booth, in inches from the center picture's middle (y down, z toward the player)
    float boothHalfWidth, boothBack, boothCeiling, boothFloor;
    float floorAlbedo;                      // linear
    // The booth's windows (inches, as the booth): 3 in its back wall (x from,
    // y from, x to, y to) and 1 in each side wall (z from, y from, z to, y to)
    float backWindows[3][4], sideWindow[4];
    // The two roof LED panels (inches, on the ceiling): either side of the
    // middle (x from, x to; mirrored), front to back (z from, z to), and how
    // bright (in screen whites)
    float roofLeds[4], roofLedRadiance;
    float eye[3];                           // who's looking, for reflections
    float acrylicIn;                        // the acrylic sheets' thickness
    // The hood under the center screen (drawn over everything, in 3D; it spans
    // the center cabinet, corner to corner), in inches: its top (y), how far out
    // it comes, how far its front leans back (inches out per inch down), how
    // tall that front is and how much of it shows above the control panel,
    // where its pieces meet (in from each end), its speaker grilles (in from
    // each end and down its front: from, to), its rounded top outer corners'
    // radius, the touch monitor's opening (half width; down its front: from,
    // to), and its paint's albedo (linear)
    float hoodTop, hoodDepth, hoodSlope, hoodHeight, hoodFace, hoodSplit;
    float hoodGrille[4], hoodCorner, hoodOpening[3], hoodAlbedo;
    float grilleCells[4];                   // the grilles' holes: across, down; radius and rim (of their spacing)
    // On its top: the center plate's flanges, resting on the end pieces (where
    // they start, either side of the middle; how wide; their sheet's
    // thickness), and the screws (x, z, head radius), black
    float hoodFlange[3];
    std::vector<std::array<float, 3>> hoodScrews;
    // The touch panel, if it's in this window (output pixels; else all 0), and
    // the black surround it sits in (pixels). With perspective, it's a sheet
    // leaning with the hood's front, a little in front of it (PerspectivePanel):
    // its half width and top and bottom (y), in inches (else all 0).
    float panelRect[4], panelSurround;
    float panelSheet[3];
    // ...and its frame, on the same sheet (inches along it, scaled with the
    // panel): its border at the sides, top and bottom, its edges' rounding;
    // its screws' inset from its sides, how far down from its top (3), and
    // their head radius
    float panelFrame[4], panelFrameScrews[5];
};

// The cabinet (wall, screen frames, brackets, fasteners, console) and its
// lighting setup. False when it isn't drawn.
bool GetCabinetScene(int width, int height, CabinetScene& scene);

// The perspective view of the arcade layout's cabinet (ArcadeCameraDistance): the
// scene is drawn flat onto a canvas marginPx wider than the output on each side,
// then warped. The center wall is seen straight on and stays as drawn; past the
// corner, the side cabinets' faces turn toward a camera cameraIn behind the
// center screen, and each output pixel there shows the point on the face its
// camera ray hits.
struct PerspectiveView
{
    float originX, originY;     // the center picture's middle (output pixels; the canvas is offset by marginPx)
    float pxPerInch;
    float cornerIn;             // where the side faces start, inches either side of the middle
    float sinA, cosA;           // how far they turn toward the camera
    float cameraIn;             // the camera's distance from the center screen
    float fovDeg;               // the output's horizontal field of view
    int marginPx;
};

// False when there's no perspective (it's off, or not the arcade layout's cabinet).
bool GetPerspective(int width, int height, PerspectiveView& view);

// With perspective, the touch panel (in the main window) leans back with the
// hood's front, as the speakers do, a little in front of it: seen from the
// camera, a quad, not a rectangle. Its corners in output pixels (top left, top
// right, bottom right, bottom left): its top edge where its placement's is,
// its bottom edge (a little wider) at its placement's bottom. False when it
// isn't drawn that way (no perspective, or no panel in the window).
bool PerspectivePanel(int width, int height, float quad[4][2]);

// Between the quad's own square (u, v: 0 to 1 across and down) and output
// pixels, perspective-correct
void PanelToOutput(const float quad[4][2], float u, float v, float& x, float& y);
bool OutputToPanel(const float quad[4][2], float x, float y, float& u, float& v);
// ...and as a matrix (row major), output pixels to (u, v, w) with (u, v) = (u, v) / w
void OutputToPanelMatrix(const float quad[4][2], float m[9]);
