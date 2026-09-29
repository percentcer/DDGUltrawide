#include "cabinetlight.h"
#include "config.h"
#include "log.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <vector>

// How it works, per frame:
//   1. Cells: each light cell's color is read from the game's frame, one mip
//      texel covering the cell.
//   2. Patches: each patch of the booth (back wall, side walls, ceiling, floor)
//      gathers light from the cells and reflects it with its own color.
//   3. Ambient cube, at 1/8 resolution: at each point on the cabinet, light from
//      every cell and patch (plus the arcade's own lighting) is summed from each
//      of 6 directions. Lighting changes slowly across the cabinet, so this is
//      where the time is saved.
//   4. Gloss, also at 1/8 resolution: like the ambient cube, but with narrow
//      lobes, so it holds the light seen in each direction (what a semi-gloss
//      surface reflects) rather than the light falling on a surface.
//   5. Shading, at full resolution: each pixel's surface (from the G-buffer)
//      takes diffuse light from the ambient cube by its normal, and specular
//      light from the gloss by its reflection, with Fresnel. Powder-coated
//      surfaces also get a fine orange-peel texture in their normals.
//   6. Booth map: each booth surface's own look (the light on it, times its
//      color) as a small grid, for reflections.
//   7. Acrylic, once the screens are drawn: each pixel under a sheet follows
//      its reflection off the sheet's front and back faces to what it hits:
//      the booth (from the booth map), the hood, or the front of the cabinet
//      itself (the screens and frames, as just drawn).
//      The chrome screw heads are done the same way: each pixel's reflection
//      off the head's curve, followed to what it hits.
//   8. The hood, last: each pixel's view is traced to the hood (a box reaching
//      out under the center screen), which takes its own light from the booth
//      map and glossily reflects the rest.
// The G-buffer (each pixel's color, normal and material) is drawn once from the
// cabinet's shapes, and again only when the window size changes.

namespace
{
    template <typename T>
    void SafeRelease(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    constexpr int kMaxCells = 128;
    constexpr int kMaxPatches = 160;
    constexpr int kMaxHoodScrews = 16;
    constexpr int kCubeScale = 8;           // ambient cube resolution: 1 texel per 8x8 pixels
    // Light levels are physical, in units of the screens' white: a screen emits
    // exactly what it shows (1 = white = ArcadeCabinetScreenNits), and the output
    // shows the screens' white as display white, so the pictures look as drawn.
    // The arcade's own lighting (ArcadeCabinetRoomLux) is converted to match.

    const char* kShader = R"(
#define MAX_CELLS 128
#define MAX_PATCHES 160
#define MAX_HOOD_SCREWS 16
static const float PI = 3.14159265;

cbuffer Shape : register(b0)
{
    float4 sRect;       // left, top, right, bottom (px). Dome: center x, y, radius
    float4 sParams;     // z0, z1, edge, kind (0 box, 1 ramp, 2 dome)
    float4 sAlbedo;     // linear rgb, ramp axis
    float4 sMat;        // roughness, metalness, pixels per inch, powder coat
    float4 sTarget;     // target width, height; the hood: its canvas's offset, perspective
};

cbuffer Scene : register(b1)
{
    float4 cellPos[MAX_CELLS];      // xyz (inches), area
    float4 cellNrm[MAX_CELLS];      // xyz
    float4 cellUV[MAX_CELLS];       // uv in the game's frame, mip level
    float4 patchPos[MAX_PATCHES];   // xyz, area
    float4 patchNrm[MAX_PATCHES];   // xyz
    float4 patchAlb[MAX_PATCHES];   // linear rgb
    float4 counts;                  // cells, patches, cube scale
    float4 view;                    // width, height, pixels per inch
    float4 origin;                  // origin x, y (px)
    float4 light;                   // screen light (1), room light (radiance, in screen whites), exposure
    float4 wall;                    // the cabinet's wall color (linear)
    float4 geom;                    // the side cabinets' faces: corner (inches from the middle), sin, cos of their turn
    float4 booth;                   // half width, back wall z, ceiling y, floor y (inches)
    float4 booth2;                  // where the side walls start (z), floor albedo, the side faces' length
    float4 eye;                     // the viewer (inches), the acrylic's thickness
    float4 hood;                    // the hood under the center screen: half width, top y, depth, slope
    float4 hood2;                   // its front's height, how much of it shows, where its pieces meet, albedo
    float4 hoodGrille;              // its speaker grilles: in from each end, down its front (from, to)
    float4 hoodOpening;             // the touch monitor's opening: half width, down its front (from, to); corners' radius
    float4 panelRect;               // the touch panel in the canvas (px; 0 when it isn't there)
    float4 panelSurround;           // its surround (px)
    float4 hoodFlange;              // the center plate's flanges: start (x, either side), width, thickness; the screws' count
    float4 hoodScrews[MAX_HOOD_SCREWS];   // the screws on the hood's top: x, z, head radius
};

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> cube0 : register(t3);   // light arriving from +x
Texture2D<float4> cube1 : register(t4);   // from -x
Texture2D<float4> cube2 : register(t5);   // from +y (below)
Texture2D<float4> cube3 : register(t6);   // from -y (above)
Texture2D<float4> cube4 : register(t7);   // from +z (in front)
Texture2D<float4> cube5 : register(t8);   // from -z (the wall; unused)
Texture2D<float4> gloss0 : register(t9);   // light seen looking +x
Texture2D<float4> gloss1 : register(t10);  // looking -x
Texture2D<float4> gloss2 : register(t11);  // looking +y (down)
Texture2D<float4> gloss3 : register(t12);  // looking -y (up)
Texture2D<float4> gloss4 : register(t13);  // looking +z (out, toward the player and the booth)
Texture2D<float4> gAlbedo : register(t14);  // 7. (the chrome screws) the G-buffer's color
Texture2D<float4> gGeo : register(t15);     // ...and its normals
SamplerState linearClamp : register(s0);

static const float GLOSS_POWER = 8;        // the gloss lobes: cos^8
// The seated player's eye (inches from the center picture's middle): about
// 1200 mm up (the picture's middle is about 1495 mm up) and 800 mm back
static const float3 EYE = float3(0, 11.6, 31.5);
static const float3 OVERHEAD = float3(0, -0.894, 0.447);   // toward the arcade's ceiling lights

// Exact sRGB transfer curves (the game's frame and our output are sRGB)
float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4);
}

float3 LinearToSrgb(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1 / 2.4) - 0.055;
}

// ---- Full-window quad ----
float4 VSFull(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2(id & 1, id >> 1);
    return float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
}

// A fastener's button head, l out from its middle (in radii): its height and
// slope (per radius). A broad, gently crowned top, rounding over at the
// shoulder down to its edge.
void ButtonHead(float l, out float h, out float dh)
{
    const float TOP = 0.5;                              // the top, out to this fraction of the radius
    if (l < TOP) { h = 1 - 0.08 * (l / TOP) * (l / TOP); dh = -0.16 * l / (TOP * TOP); return; }
    float k = min((l - TOP) / (1 - TOP), 0.98), q = sqrt(1 - k * k);
    h = 0.92 * q;
    dh = -0.92 * k / q / (1 - TOP);
}

// ---- G-buffer: one shape per draw ----
float4 VSShape(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2(id & 1, id >> 1);
    bool dome = sParams.w > 1.5;
    float4 b = dome ? float4(sRect.xy - sRect.z - 1, sRect.xy + sRect.z + 1) : sRect;
    float2 p = lerp(b.xy, b.zw, t) / sTarget.xy;
    return float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
}

struct GBuffer
{
    float4 albedo : SV_Target0;     // linear rgb, coverage
    float4 geo : SV_Target1;        // normal x, y (0..1), height / 16, coverage
    float4 mat : SV_Target2;        // roughness, metalness, powder coat, coverage
};

GBuffer PSShape(float4 pos : SV_Position)
{
    float2 p = pos.xy;
    float3 n = float3(0, 0, 1);
    float z = sParams.x;
    float cover = 1;
    float3 albedo = sAlbedo.rgb;
    float roughness = sMat.x;
    if (sParams.w > 1.5)
    {
        // Dome: a fastener's button head...
        float2 d = (p - sRect.xy) / sRect.z;
        float l = length(d);
        cover = saturate((1 - l) * sRect.z + 0.5);
        float2 dir = l > 1e-4 ? d / l : 0;
        float h, dh;
        ButtonHead(l, h, dh);
        n = normalize(float3(dir * -dh * sParams.z / (sRect.z / sMat.z), 1));
        z += sParams.z * h;

        // ...and a tamper-proof Torx recess in it: a six-lobed star over half
        // the head across, turned however it was driven in, with a pin standing
        // in its middle. Its walls, just inside its edge, face in and catch the
        // light (a bright rim around it, as in photos); its floor is dark.
        float turn = frac(sin(dot(sRect.xy, float2(12.9898, 78.233))) * 43758.5453) * 6.2832;
        float star = 0.33 + 0.08 * cos(6 * (atan2(d.y, d.x) + turn));
        float aa = 1 / sRect.z;                         // a pixel, in radii
        float recess = saturate((star - l) / aa + 0.5);
        float wall = recess * saturate((l - (star - max(0.08, aa))) / aa + 0.5);
        float pin = saturate((0.13 - l) / aa + 0.5);
        float floor_ = saturate(recess - wall) * (1 - pin);
        n = normalize(lerp(n, float3(dir * 0.3, 1), recess - wall));   // the pin's top, and the recess's floor: flat
        n = normalize(lerp(n, float3(-dir, 1), wall));
        z -= sParams.z * 0.5 * (floor_ + wall * 0.5);
        albedo *= 1 - 0.75 * floor_;                    // the floor: deep, and in its own shadow
        roughness = lerp(roughness, 0.7, floor_);
    }
    else if (sParams.w > 0.5)
    {
        // Ramp: a flat slope from z0 to z1
        bool down = sAlbedo.w > 0.5;
        float len = down ? sRect.w - sRect.y : sRect.z - sRect.x;
        float t = saturate(down ? (p.y - sRect.y) / len : (p.x - sRect.x) / len);
        z = lerp(sParams.x, sParams.y, t);
        float slope = (sParams.y - sParams.x) / (len / sMat.z);
        n = normalize(down ? float3(0, -slope, 1) : float3(-slope, 0, 1));
    }
    else if (sParams.z > 0)
    {
        // Box: flat, with its edges rounded over
        float4 d = float4(p.x - sRect.x, p.y - sRect.y, sRect.z - p.x, sRect.w - p.y);
        float m = min(min(d.x, d.y), min(d.z, d.w));
        if (m < sParams.z)
        {
            float2 dir = m == d.x ? float2(-1, 0) : m == d.y ? float2(0, -1) : m == d.z ? float2(1, 0) : float2(0, 1);
            float t = 1 - m / sParams.z;
            n = normalize(float3(dir * t * 1.5, 1));
            z -= t * t * sParams.z / sMat.z;
        }
    }
    GBuffer o;
    o.albedo = float4(albedo, cover);
    o.geo = float4(n.xy * 0.5 + 0.5, saturate(z / 16), cover);
    o.mat = float4(roughness, sMat.y, sMat.w, cover);
    return o;
}

// ---- 1. Cells: their color from the game's frame (t0) ----
float4 PSCells(float4 pos : SV_Position) : SV_Target
{
    int i = (int)pos.x;
    float3 c = t0.SampleLevel(linearClamp, cellUV[i].xy, cellUV[i].z).rgb;
    return float4(SrgbToLinear(saturate(c)) * light.x, 1);
}

// Light from an area source of radiance L and area A at S facing sn, arriving at
// P (not counting the receiver's own angle). w is the direction to the source.
float3 FromSource(float3 P, float3 S, float3 sn, float A, float3 L, out float3 w)
{
    float3 d = S - P;
    float d2 = max(dot(d, d), 1e-4);
    w = d * rsqrt(d2);
    return L * (A * saturate(dot(sn, -w)) / (d2 + A / PI));
}

// Where an output pixel's surface is in 3D (the cabinet is drawn flat, but past
// the corner it continues along the side cabinets' faces, which turn toward the
// player), and its axes: ex along the drawing's x, ez out of the surface
float3 Place(float2 px, float zLocal, out float3 ex, out float3 ez)
{
    float2 f = (px - origin.xy) / view.z;
    ex = float3(1, 0, 0);
    ez = float3(0, 0, 1);
    float along = abs(f.x) - geom.x;
    if (along <= 0) return float3(f, zLocal);
    float sgn = f.x < 0 ? -1 : 1;
    ex = float3(geom.z, 0, sgn * geom.y);
    ez = float3(-sgn * geom.y, 0, geom.z);
    return float3(sgn * geom.x, f.y, 0) + sgn * ex * along + ez * zLocal;
}

// A world direction in a surface's own axes
float3 ToLocal(float3 w, float3 ex, float3 ez)
{
    return float3(dot(w, ex), w.y, dot(w, ez));
}

// ---- 2. Patches: light from the cells (t0) and, after the first pass, from
// each other (t1, the previous pass), reflected ----
float3 PatchDirect(int i)
{
    float3 P = patchPos[i].xyz, N = patchNrm[i].xyz;
    float3 E = 0;
    for (int c = 0; c < (int)counts.x; ++c)
    {
        float3 w;
        float3 e = FromSource(P, cellPos[c].xyz, cellNrm[c].xyz, cellPos[c].w, t0.Load(int3(c, 0, 0)).rgb, w);
        E += e * saturate(dot(N, w));
    }
    return E;
}

float4 PSPatches(float4 pos : SV_Position) : SV_Target
{
    int i = (int)pos.x;
    return float4(patchAlb[i].rgb * PatchDirect(i) / PI, 1);
}

float4 PSBounce(float4 pos : SV_Position) : SV_Target
{
    int i = (int)pos.x;
    float3 P = patchPos[i].xyz, N = patchNrm[i].xyz;
    float3 E = PatchDirect(i);
    for (int j = 0; j < (int)counts.y; ++j)
    {
        if (j == i) continue;
        float3 w;
        float3 e = FromSource(P, patchPos[j].xyz, patchNrm[j].xyz, patchPos[j].w, t1.Load(int3(j, 0, 0)).rgb, w);
        E += e * saturate(dot(N, w));
    }
    return float4(patchAlb[i].rgb * E / PI, 1);
}

// ---- 3. Ambient cube: light from cells (t0) and patches (t1), by direction ----
struct Cube
{
    float4 px : SV_Target0;
    float4 nx : SV_Target1;
    float4 py : SV_Target2;
    float4 ny : SV_Target3;
    float4 pz : SV_Target4;
    float4 nz : SV_Target5;
};

Cube PSCube(float4 pos : SV_Position)
{
    float3 ex, ez;
    float3 P = Place(pos.xy * counts.z, 0.8, ex, ez);
    float3 c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0;
    float3 w, e;
    for (int i = 0; i < (int)counts.x; ++i)
    {
        e = FromSource(P, cellPos[i].xyz, cellNrm[i].xyz, cellPos[i].w, t0.Load(int3(i, 0, 0)).rgb, w);
        w = ToLocal(w, ex, ez);
        c0 += e * max(w.x, 0); c1 += e * max(-w.x, 0);
        c2 += e * max(w.y, 0); c3 += e * max(-w.y, 0);
        c4 += e * max(w.z, 0); c5 += e * max(-w.z, 0);
    }
    for (int j = 0; j < (int)counts.y; ++j)
    {
        e = FromSource(P, patchPos[j].xyz, patchNrm[j].xyz, patchPos[j].w, t1.Load(int3(j, 0, 0)).rgb, w);
        w = ToLocal(w, ex, ez);
        c0 += e * max(w.x, 0); c1 += e * max(-w.x, 0);
        c2 += e * max(w.y, 0); c3 += e * max(-w.y, 0);
        c4 += e * max(w.z, 0); c5 += e * max(-w.z, 0);
    }
    // The arcade's own lighting: mostly from above and in front
    float room = light.y * PI;
    c3 += room * 0.6; c4 += room * 0.85;
    c0 += room * 0.25; c1 += room * 0.25; c2 += room * 0.15;
    Cube o;
    o.px = float4(c0, 1); o.nx = float4(c1, 1);
    o.py = float4(c2, 1); o.ny = float4(c3, 1);
    o.pz = float4(c4, 1); o.nz = float4(c5, 1);
    return o;
}

// ---- 4. Gloss: light seen in each direction, from cells (t0) and patches (t1) ----
struct Gloss
{
    float4 px : SV_Target0;
    float4 nx : SV_Target1;
    float4 py : SV_Target2;
    float4 ny : SV_Target3;
    float4 pz : SV_Target4;
};

Gloss PSGloss(float4 pos : SV_Position)
{
    float3 ex, ez;
    float3 P = Place(pos.xy * counts.z, 0.8, ex, ez);
    float3 g0 = 0, g1 = 0, g2 = 0, g3 = 0, g4 = 0;
    float3 w, e;
    for (int i = 0; i < (int)counts.x + (int)counts.y; ++i)
    {
        if (i < (int)counts.x)
            e = FromSource(P, cellPos[i].xyz, cellNrm[i].xyz, cellPos[i].w, t0.Load(int3(i, 0, 0)).rgb, w);
        else
        {
            int j = i - (int)counts.x;
            e = FromSource(P, patchPos[j].xyz, patchNrm[j].xyz, patchPos[j].w, t1.Load(int3(j, 0, 0)).rgb, w);
        }
        w = ToLocal(w, ex, ez);
        g0 += e * pow(max(w.x, 0), GLOSS_POWER); g1 += e * pow(max(-w.x, 0), GLOSS_POWER);
        g2 += e * pow(max(w.y, 0), GLOSS_POWER); g3 += e * pow(max(-w.y, 0), GLOSS_POWER);
        g4 += e * pow(max(w.z, 0), GLOSS_POWER);
    }
    // Light per solid angle in each lobe, plus the arcade's own lighting. The
    // screens' light carries a gain so their glow holds up against the room
    // lighting on matte surfaces, but a reflection is only as bright as what it
    // reflects: take the gain back out, so a reflected screen is no brighter
    // than the screen itself
    float k = (GLOSS_POWER + 1) / (2 * PI) / max(light.x, 1e-3), room = light.y;
    Gloss o;
    o.px = float4(g0 * k + room * 0.25, 1); o.nx = float4(g1 * k + room * 0.25, 1);
    o.py = float4(g2 * k + room * 0.15, 1); o.ny = float4(g3 * k + room * 0.9, 1);
    o.pz = float4(g4 * k + room * 0.6, 1);
    return o;
}

)"
R"(
// ---- 5. Shading: G-buffer (t0..t2) lit by the ambient cube (t3..t8) and gloss (t9..t13) ----
float3 Irradiance(float3 n, float2 uv)
{
    float3 n2 = n * n;
    float3 x = n.x >= 0 ? cube0.SampleLevel(linearClamp, uv, 0).rgb : cube1.SampleLevel(linearClamp, uv, 0).rgb;
    float3 y = n.y >= 0 ? cube2.SampleLevel(linearClamp, uv, 0).rgb : cube3.SampleLevel(linearClamp, uv, 0).rgb;
    float3 z = n.z >= 0 ? cube4.SampleLevel(linearClamp, uv, 0).rgb : cube5.SampleLevel(linearClamp, uv, 0).rgb;
    return n2.x * x + n2.y * y + n2.z * z;
}

// The light seen looking in direction r (unit), from the gloss lobes (blended
// more softly than they were gathered, so curved surfaces show no seams)
static const float BLEND_POWER = 3;
float3 Glossy(float3 r, float2 uv)
{
    float w0 = pow(max(r.x, 0), BLEND_POWER), w1 = pow(max(-r.x, 0), BLEND_POWER);
    float w2 = pow(max(r.y, 0), BLEND_POWER), w3 = pow(max(-r.y, 0), BLEND_POWER);
    float w4 = pow(max(r.z, 0), BLEND_POWER);
    float3 L = w0 * gloss0.SampleLevel(linearClamp, uv, 0).rgb + w1 * gloss1.SampleLevel(linearClamp, uv, 0).rgb
             + w2 * gloss2.SampleLevel(linearClamp, uv, 0).rgb + w3 * gloss3.SampleLevel(linearClamp, uv, 0).rgb
             + w4 * gloss4.SampleLevel(linearClamp, uv, 0).rgb;
    return L / max(w0 + w1 + w2 + w3 + w4, 1e-4);
}

float Hash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

// Smooth value noise, 0..1
float Noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x),
                lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), f.x), f.y);
}

// Orange peel: the fine dimpling of powder coat, as a slope added to the normal
float2 OrangePeel(float2 px)
{
    float2 q = px / view.z / 0.08;                      // dimples about 0.08" (2 mm) across
    float2 s = float2(Noise(q + float2(0.5, 0)) - Noise(q - float2(0.5, 0)),
                      Noise(q + float2(0, 0.5)) - Noise(q - float2(0, 0.5)));
    float2 q2 = q * 2.7 + 17.0;
    s += 0.5 * float2(Noise(q2 + float2(0.5, 0)) - Noise(q2 - float2(0.5, 0)),
                      Noise(q2 + float2(0, 0.5)) - Noise(q2 - float2(0, 0.5)));
    return s * 0.1;
}

float4 PSShade(float4 pos : SV_Position) : SV_Target
{
    int3 ip = int3(pos.xy, 0);
    float3 albedo = t0.Load(ip).rgb;
    float4 geo = t1.Load(ip);
    float3 mat = t2.Load(ip).xyz;                       // roughness, metalness, powder coat
    float2 nxy = geo.xy * 2 - 1;
    // Orange peel only on flat faces: on the rounded edges and slopes it would
    // break up their thin highlights (and isn't visible on a tight radius anyway)
    float flat = saturate((1 - dot(nxy, nxy) - 0.85) / 0.13);
    nxy -= OrangePeel(pos.xy) * mat.z * flat;
    float3 n = normalize(float3(nxy, sqrt(saturate(1 - dot(nxy, nxy)))));
    float2 uv = pos.xy / view.xy;

    // Seen from the player's eye: the direction to it from this point
    float3 ex, ez;
    float3 P = Place(pos.xy, geo.z * 16, ex, ez);
    float3 v = ToLocal(normalize(EYE - P), ex, ez);
    float nv = saturate(dot(n, v));

    // Fresnel (Schlick)
    float3 F0 = lerp(0.04, albedo, mat.y);
    float3 F = F0 + (1 - F0) * pow(1 - nv, 5);
    // The view, reflected. On steep faces (a screw's far flank, a bracket step)
    // it points back into the wall, where it sees the wall right beside this
    // point: the wall's color, lit by what falls on it here
    float3 r = 2 * nv * n - v;
    float intoWall = saturate(0.5 - r.z / 0.2);
    r = normalize(float3(r.xy, max(r.z, 0.05)));
    float3 wallSeen = wall.rgb * Irradiance(float3(0, 0, 1), uv) / PI;

    // Diffuse, less what's reflected; specular from the gloss lobes, blurring
    // toward the ambient cube as the surface gets rougher
    float3 diffuse = albedo * (1 - mat.y) * (1 - F) * Irradiance(n, uv) / PI;
    float3 env = lerp(Glossy(r, uv), Irradiance(r, uv) / PI, saturate(mat.x * 1.6 - 0.3));
    float3 specular = F * lerp(env, wallSeen, intoWall);

    // The arcade's ceiling lights, overhead and a little toward the player: a
    // highlight, crisp on chrome and broad and faint on satin paint
    float power = exp2(6 * (1 - mat.x) + 1);           // 37 for chrome, 20 for satin
    specular += F * light.y * 2.5 * pow(saturate(dot(r, OVERHEAD)), power) * (1 - intoWall);
    float3 c = (diffuse + specular) * light.z;
    // Leave mid-tones alone; roll bright highlights off smoothly instead of clipping
    c = c < 0.8 ? c : 0.8 + 0.2 * (1 - exp(-(c - 0.8) / 0.2));
    return float4(LinearToSrgb(saturate(c)), 1);
}

)"
// (split in two: the compiler limits how long one string literal can be)
R"(
// ---- 6. Booth map: each booth surface (back, left, right, ceiling, floor) and
// the hood's top and front, as a MAP x MAP grid of what it looks like, lit by
// the cells (t0) and patches (t1) ----
static const int MAP = 32;
static const int MAP_FACES = 7;
static const int HOOD_TOP = 5, HOOD_FRONT = 6;

float3 BoothPoint(int face, float2 uv, out float3 N, out float3 albedo)
{
    float hw = booth.x, bk = booth.y, cy = booth.z, fy = booth.w, fz = booth2.x;
    albedo = wall.rgb;
    if (face == 0) { N = float3(0, 0, -1); return float3(lerp(-hw, hw, uv.x), lerp(cy, fy, uv.y), bk); }
    if (face == 1) { N = float3(1, 0, 0); return float3(-hw, lerp(cy, fy, uv.y), lerp(fz, bk, uv.x)); }
    if (face == 2) { N = float3(-1, 0, 0); return float3(hw, lerp(cy, fy, uv.y), lerp(fz, bk, uv.x)); }
    if (face == 3) { N = float3(0, 1, 0); return float3(lerp(-hw, hw, uv.x), cy, bk * uv.y); }
    if (face == 4) { N = float3(0, -1, 0); albedo = booth2.y; return float3(lerp(-hw, hw, uv.x), fy, bk * uv.y); }
    float x = lerp(-hood.x, hood.x, uv.x);
    albedo = hood2.w;
    if (face == HOOD_TOP) { N = float3(0, -1, 0); return float3(x, hood.y, hood.z * uv.y); }
    float s = hood2.x * uv.y;                          // down the front
    N = normalize(float3(0, -hood.w, 1));
    return float3(x, hood.y + s, hood.z + hood.w * s);
}

float4 PSBoothMap(float4 pos : SV_Position) : SV_Target
{
    int face = (int)(pos.y / MAP);
    float2 uv = float2(pos.x, pos.y - face * MAP) / MAP;
    float3 N, albedo;
    float3 P = BoothPoint(face, uv, N, albedo);
    float3 E = 0, w;
    for (int c = 0; c < (int)counts.x; ++c)
        E += FromSource(P, cellPos[c].xyz, cellNrm[c].xyz, cellPos[c].w, t0.Load(int3(c, 0, 0)).rgb, w) * saturate(dot(N, w));
    for (int j = 0; j < (int)counts.y; ++j)
        E += FromSource(P, patchPos[j].xyz, patchNrm[j].xyz, patchPos[j].w, t1.Load(int3(j, 0, 0)).rgb, w) * saturate(dot(N, w));
    return float4(albedo * (E / PI + light.y), 1);
}

// ---- 7. Acrylic: what's under it (t0, the target as drawn), the booth map (t1),
// and the G-buffer's materials (t2: the chrome screws sit on top of the sheet) ----
float3 Under(float2 px, float lod)
{
    return SrgbToLinear(t0.SampleLevel(linearClamp, px / view.xy, lod).rgb);
}

float3 BoothMap(int face, float2 uv)
{
    uv = clamp(uv, 0.5 / MAP, 1 - 0.5 / MAP);
    return t1.SampleLevel(linearClamp, float2(uv.x, (face + uv.y) / MAP_FACES), 0).rgb;
}

// What's seen from P looking along r (unit), and how far: the nearest of the
// booth's surfaces, the center wall and the side cabinets' faces. A glossy
// surface's reflection spreads by `spread` (radians): the further it goes, the
// blurrier what it shows of the cabinet.
float3 SeenFar(float3 P, float3 r, float spread, out float t)
{
    float hw = booth.x, bk = booth.y, cy = booth.z, fy = booth.w, fz = booth2.x;
    float u;
    int face = 0;
    t = 1e9;
    if (r.z > 1e-4) { u = (bk - P.z) / r.z; if (u > 0 && u < t) { t = u; face = 0; } }
    if (r.x < -1e-4) { u = (-hw - P.x) / r.x; if (u > 0 && u < t) { t = u; face = 1; } }
    if (r.x > 1e-4) { u = (hw - P.x) / r.x; if (u > 0 && u < t) { t = u; face = 2; } }
    if (r.y < -1e-4) { u = (cy - P.y) / r.y; if (u > 0 && u < t) { t = u; face = 3; } }
    if (r.y > 1e-4) { u = (fy - P.y) / r.y; if (u > 0 && u < t) { t = u; face = 4; } }

    // The front: the center wall, then each side cabinet's face
    bool front = false;
    float2 flat = 0;
    if (r.z < -1e-4)
    {
        u = -P.z / r.z;
        float3 X = P + r * u;
        if (u > 1e-3 && u < t && abs(X.x) <= geom.x) { t = u; front = true; flat = X.xy; }
    }
    for (int s = -1; s <= 1; s += 2)
    {
        float3 C = float3(s * geom.x, 0, 0);
        float3 ez = float3(-s * geom.y, 0, geom.z), ex = float3(s * geom.z, 0, geom.y);
        float d = dot(r, ez);
        if (d < -1e-4)
        {
            u = dot(C - P, ez) / d;
            float3 X = P + r * u;
            float along = dot(X - C, ex);
            if (u > 1e-3 && u < t && along >= 0 && along <= booth2.z) { t = u; front = true; flat = float2(s * (geom.x + along), X.y); }
        }
    }
    if (front) return Under(origin.xy + flat * view.z, log2(max(t * spread * view.z, 1)));

    float3 X = P + r * t;
    float2 uv = face == 0 ? float2((X.x + hw) / (2 * hw), (X.y - cy) / (fy - cy))
              : face <= 2 ? float2((X.z - fz) / (bk - fz), (X.y - cy) / (fy - cy))
              : float2((X.x + hw) / (2 * hw), X.z / bk);
    return BoothMap(face, uv);
}

)"
R"(
static const float HOOD_BEND = 0.3;      // the hood's sheet's bend between top and front (inches)

// Where r from P first meets the hood under the center screen, and its
// surface's normal there; 1e9 if it doesn't. It's a box (its top, its front
// leaning back, its outer sides) with the edges between its top and outer
// sides rounded over, front to back, to radius R; and on its top, the center
// plate's flanges, each a thin box (a sheet thick) resting on an end piece.
float HoodHit(float3 P, float3 r, out float3 N)
{
    float hw = hood.x, top = hood.y, D = hood.z, s = hood.w, bottom = hood.y + hood2.x;
    float R = hoodOpening.w;
    float t = 1e9, u;
    float3 X;
    N = float3(0, 0, 1);
    if (r.y > 1e-4)
    {
        u = (top - P.y) / r.y;
        X = P + r * u;
        if (u > 1e-3 && abs(X.x) <= hw - R && X.z >= 0 && X.z <= D) { t = u; N = float3(0, -1, 0); }
    }
    // The rounded edges: a quarter of a cylinder along z at each end
    float a = dot(r.xy, r.xy);
    for (int e = -1; e <= 1; e += 2)
    {
        float2 c = float2(e * (hw - R), top + R);
        float2 d = P.xy - c;
        float b = dot(d, r.xy), q = dot(d, d) - R * R;
        float disc = b * b - a * q;
        if (a < 1e-8 || disc < 0) continue;
        u = (-b - sqrt(disc)) / a;                      // the near side
        X = P + r * u;
        if (u > 1e-3 && u < t && e * (X.x - c.x) >= 0 && X.y <= c.y && X.z >= 0 && X.z <= D + s * (X.y - top))
        {
            t = u;
            N = float3((X.xy - c) / R, 0);
        }
    }
    float3 nf = normalize(float3(0, -s, 1));
    if (dot(r, nf) < -1e-4)
    {
        u = (D - s * top - P.z + s * P.y) / (r.z - s * r.y);
        X = P + r * u;
        float2 c = float2(hw - abs(X.x), X.y - top) - R;   // from the corner's center, when it's past it
        bool corner = c.x < 0 && c.y < 0 && length(c) > R;
        if (u > 1e-3 && u < t && abs(X.x) <= hw && X.y >= top && X.y <= bottom && !corner) { t = u; N = nf; }
    }
    for (int k = -1; k <= 1; k += 2)
    {
        if (k * r.x < -1e-4)
        {
            u = (k * hw - P.x) / r.x;
            X = P + r * u;
            if (u > 1e-3 && u < t && X.y >= top + R && X.y <= bottom && X.z >= 0 && X.z <= D + s * (X.y - top))
            {
                t = u;
                N = float3(k, 0, 0);
            }
        }
    }
    for (int f = -1; f <= 1; f += 2)
    {
        // The flange: its slabs (x, y, z), and where the ray is inside all three
        float3 lo = float3(f < 0 ? -(hoodFlange.x + hoodFlange.y) : hoodFlange.x, top - hoodFlange.z, 0);
        float3 hi = float3(f < 0 ? -hoodFlange.x : hoodFlange.x + hoodFlange.y, top, D - HOOD_BEND);
        float3 inv = 1 / (abs(r) > 1e-6 ? r : 1e-6);
        float3 t0 = (lo - P) * inv, t1 = (hi - P) * inv;
        float3 tn = min(t0, t1), tf = max(t0, t1);
        float enter = max(max(tn.x, tn.y), tn.z), leave = min(min(tf.x, tf.y), tf.z);
        if (enter <= leave && enter > 1e-3 && enter < t)
        {
            t = enter;
            N = enter == tn.x ? float3(-sign(r.x), 0, 0) : enter == tn.y ? float3(0, -sign(r.y), 0) : float3(0, 0, -sign(r.z));
        }
    }
    return t;
}

// Reflectance of a clear dielectric's face (unpolarized), at cos(incidence) c
float Fresnel(float c, float n)
{
    float ct = sqrt(saturate(1 - (1 - c * c) / (n * n)));
    float rs = (c - n * ct) / (c + n * ct), rp = (n * c - ct) / (n * c + ct);
    return 0.5 * (rs * rs + rp * rp);
}

static const float HOOD_SPREAD = 0.02;   // the hood's gloss: its reflections blur by this (radians)

// What the hood looks like at X (hit along r, with normal N): the light on it
// (from the booth map), and a glossy reflection of the rest. Its grilles and
// seams are dull, the control panel's front edge (below what shows of its
// front) is a lighter plastic, and the touch monitor's opening is dark glass.
float3 HoodShade(float3 X, float3 N, float3 r)
{
    float hw = hood.x, D = hood.z;
    bool flange = X.y < hood.y - 1e-4;                  // on a flange (it stands above the top)
    bool isTop = N.y < -0.5, isFront = N.z > 0.5 && !flange;
    float s = X.y - hood.y;                             // down the front
    float in_ = hw - abs(X.x);                          // in from the end
    float3 n = N;
    float diffuseK = 1, glossK = 1;
    // Rounded over where the top meets the front. Across the bend, its
    // reflection sweeps from the screen, over the ceiling, to the booth behind
    // the player, all within a pixel or two: it shows their blend, a soft line.
    // So it's blurred there by how far the bend turns (the rounded ends turn
    // the whole way).
    float bend = flange && !isTop ? 1 : isTop ? saturate((X.z - (D - HOOD_BEND)) / HOOD_BEND) : isFront ? saturate(1 - s / HOOD_BEND) : 1;
    if (isTop) n = normalize(n + float3(0, -hood.w, 1) * bend);
    // A flange's front end: its top carries on over the bend to the front, so
    // it's shaded as the top (blurred, as the bend's reflection is)
    if (flange && N.z > 0.5) n = float3(0, -1, 0);

    // On the top: the center plate's flanges (traced as thin boxes; their
    // sides are blurred like the other bends). Each one's outer edge rolls over
    // (a thin highlight), and just past it, on the end piece, is its shadow.
    // Then the black screws.
    float across = (abs(X.x) - hoodFlange.x) / hoodFlange.y;     // 0 to 1 across a flange
    float past = (across - 1) * hoodFlange.y;                    // past the sheet's edge (inches)
    bool screw = false;
    if (isTop)
    {
        if (flange && past > -hoodFlange.z)
        {
            float roll = 1 + past / hoodFlange.z;
            n = normalize(n + float3(sign(X.x) * 2 * roll, 0, 0));
            bend = max(bend, roll);
        }
        else if (!flange && past >= 0 && past < hoodFlange.z) { diffuseK = 0.4; glossK = 0.35; }
        for (int i = 0; i < (int)hoodFlange.w; ++i)
        {
            float2 d = (X.xz - hoodScrews[i].xy) / hoodScrews[i].z;
            float l = length(d);
            if (l >= 1) continue;
            float h, dh;
            ButtonHead(l, h, dh);
            float2 dir = l > 1e-4 ? d / l : 0;
            n = normalize(float3(dir.x * -dh * 0.3, -1, dir.y * -dh * 0.3));   // (heads 0.3 of their width high)
            screw = true;
            bend = max(bend, l);                        // the reflection blurs over the curve, as at the edges
        }
    }
    if (isFront)
    {
        n = normalize(n + float3(0, -1, 0) * bend);
        if (s > hood2.y) { diffuseK = 1.7; glossK = 0.5; }
        else if (in_ > hoodGrille.x && in_ < hoodGrille.z && s > hoodGrille.y && s < hoodGrille.w) { diffuseK = 0.4; glossK = 0.1; }
        else if (abs(in_ - hood2.z) < 0.03) { diffuseK = 0.3; glossK = 0.2; }
        else if (abs(X.x) < hoodOpening.x && s > hoodOpening.y && s < hoodOpening.z) diffuseK = 0.4;
    }
    // Orange peel: fainter than on the cabinet (at the shallow angle the top is
    // seen from, it glitters), and fading out toward the rounded edges, where it
    // would break up their thin highlights (none on the rounded ends at all)
    float edgeDist = isTop ? min(min(D - X.z, in_ - hoodOpening.w), abs(past)) : isFront ? s : 0;
    if (screw) edgeDist = 0;
    float2 peel = OrangePeel((isTop ? X.xz : X.xy) * view.z) * 0.2 * saturate(edgeDist / (2 * HOOD_BEND) - 0.5);
    n = normalize(n - (isTop ? float3(peel.x, 0, peel.y) : float3(peel, 0)));
    float2 uv = float2((X.x + hw) / (2 * hw), isTop ? X.z / D : s / hood2.x);
    float3 diffuse = BoothMap(isTop ? HOOD_TOP : HOOD_FRONT, uv) * diffuseK;
    float c = saturate(dot(-r, n));
    float3 r2 = r + 2 * c * n;
    float t;
    // The screws: black-finished metal, a little more reflective than the paint
    float F = screw ? 0.12 + 0.88 * pow(1 - c, 5) : Fresnel(max(c, 1e-3), 1.5);
    if (screw) diffuse *= 0.3;
    return diffuse + F * glossK * SeenFar(X + N * 0.02, r2, HOOD_SPREAD + 0.5 * bend, t);
}

// What's seen from P looking along r: as SeenFar, unless the hood is in the way
float3 Seen(float3 P, float3 r)
{
    float tFar;
    float3 L = SeenFar(P, r, 0, tFar);
    float3 N;
    float tHood = HoodHit(P, r, N);
    if (tHood < tFar) L = HoodShade(P + r * tHood, N, r);
    return L;
}

static const float ACRYLIC_N = 1.49;

float4 PSAcrylic(float4 pos : SV_Position) : SV_Target
{
    int3 ip = int3(pos.xy, 0);
    float3 under = SrgbToLinear(t0.Load(ip).rgb);
    float sheet = 1 - t2.Load(ip).y;                    // not where a (metal) screw sits on it
    float3 ex, ez;
    float3 P = Place(pos.xy, sParams.x, ex, ez);
    float3 v = normalize(eye.xyz - P);
    float c = dot(v, ez);
    if (c <= 1e-3 || sheet <= 0) return float4(LinearToSrgb(saturate(under)), 1);

    // Off the front face, and off the back face: that one comes out of the
    // front further from the eye, having crossed the sheet twice, so it's a
    // second, fainter image, shifted
    float3 r = 2 * c * ez - v;
    float F = Fresnel(c, ACRYLIC_N);
    float3 along = v - c * ez;
    float sinT = sqrt(saturate(1 - c * c)) / ACRYLIC_N;
    float shift = 2 * eye.w * sinT / sqrt(1 - sinT * sinT);
    float3 P2 = P - (dot(along, along) > 1e-8 ? normalize(along) : 0) * shift;
    float3 reflected = F * Seen(P, r) + (1 - F) * (1 - F) * F * Seen(P2, r);

    // The screens are as bright as they're set up to be seen, so what's under
    // the sheet isn't dimmed by it: the reflections add to it
    return float4(LinearToSrgb(saturate(under + reflected * sheet)), 1);
}

// ---- 7. (continued) The chrome screw heads: mirrors, curved. What's under
// them (t0), the booth map (t1), and the G-buffer (t2, gAlbedo, gGeo), drawn
// over what's there by how much of each pixel is metal ----
float4 PSChrome(float4 pos : SV_Position) : SV_Target
{
    int3 ip = int3(pos.xy, 0);
    float metal = t2.Load(ip).y;
    if (metal < 0.01) discard;
    float3 albedo = gAlbedo.Load(ip).rgb;
    float4 geo = gGeo.Load(ip);
    float2 nxy = geo.xy * 2 - 1;
    float3 n = float3(nxy, sqrt(saturate(1 - dot(nxy, nxy))));
    float3 ex, ez;
    float3 P = Place(pos.xy, geo.z * 16, ex, ez);
    float3 N = normalize(ex * n.x + float3(0, n.y, 0) + ez * n.z);    // from the surface's axes
    float3 v = normalize(eye.xyz - P);
    float c = saturate(dot(N, v));
    float3 F = albedo + (1 - albedo) * pow(1 - c, 5);                // metal: tinted by its color
    float3 r = 2 * c * N - v;
    // What it mirrors, and the arcade's ceiling lights: a sharp glint
    float3 L = F * (Seen(P + N * 0.05, r) + light.y * 2.5 * pow(saturate(dot(r, OVERHEAD)), 64)) * light.z;
    L = L < 0.8 ? L : 0.8 + 0.2 * (1 - exp(-(L - 0.8) / 0.2));
    return float4(LinearToSrgb(saturate(L)), metal);
}

// ---- 8. The hood: each pixel's view (from the camera, or straight on) traced
// to it; the cabinet as drawn (t0) is what it reflects. Where it misses, and
// where the touch panel is (it sits in the hood's opening), nothing's drawn. ----
float4 PSHood(float4 pos : SV_Position) : SV_Target
{
    float2 cpx = float2(pos.x + sTarget.z, pos.y);      // in the canvas
    float2 f = (cpx - origin.xy) / view.z;              // on the wall
    bool perspective = sTarget.w > 0.5;
    float3 P = perspective ? eye.xyz : float3(f, 1000);
    float3 r = perspective ? normalize(float3(f, 0) - eye.xyz) : float3(0, 0, -1);
    float3 N;
    float t = HoodHit(P, r, N);
    if (t > 1e8) discard;

    float3 c;
    if (panelRect.z > panelRect.x && all(cpx >= panelRect.xy - panelSurround.x) && all(cpx <= panelRect.zw + panelSurround.x))
    {
        if (all(cpx >= panelRect.xy) && all(cpx <= panelRect.zw)) discard;
        c = 0.1 * HoodShade(P + r * t, N, r);           // its black surround: duller
    }
    else c = HoodShade(P + r * t, N, r);
    c *= light.z;
    c = c < 0.8 ? c : 0.8 + 0.2 * (1 - exp(-(c - 0.8) / 0.2));
    return float4(LinearToSrgb(saturate(c)), 1);
}
)";

    struct ShapeConstants
    {
        float rect[4], params[4], albedo[4], mat[4], target[4];
    };

    struct SceneConstants
    {
        float cellPos[kMaxCells][4], cellNrm[kMaxCells][4], cellUV[kMaxCells][4];
        float patchPos[kMaxPatches][4], patchNrm[kMaxPatches][4], patchAlb[kMaxPatches][4];
        float counts[4], view[4], origin[4], light[4], wall[4], geom[4], booth[4], booth2[4], eye[4];
        float hood[4], hood2[4], hoodGrille[4], hoodOpening[4], panelRect[4], panelSurround[4];
        float hoodFlange[4], hoodScrews[kMaxHoodScrews][4];
    };

    struct Target
    {
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;

        bool Create(ID3D11Device* dev, int w, int h, DXGI_FORMAT format, bool mips = false)
        {
            Release();
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = mips ? 0 : 1;
            td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
            td.ArraySize = 1;
            td.Format = format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex))
                && SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv))
                && SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &srv));
        }
        void Release() { SafeRelease(srv); SafeRelease(rtv); SafeRelease(tex); }
    };

    ID3D11Device* g_dev = nullptr;
    ID3D11VertexShader* g_vsFull = nullptr;
    ID3D11VertexShader* g_vsShape = nullptr;
    ID3D11PixelShader* g_psShape = nullptr;
    ID3D11PixelShader* g_psCells = nullptr;
    ID3D11PixelShader* g_psPatches = nullptr;
    ID3D11PixelShader* g_psBounce = nullptr;
    ID3D11PixelShader* g_psCube = nullptr;
    ID3D11PixelShader* g_psGloss = nullptr;
    ID3D11PixelShader* g_psShade = nullptr;
    ID3D11PixelShader* g_psBoothMap = nullptr;
    ID3D11PixelShader* g_psAcrylic = nullptr;
    ID3D11PixelShader* g_psHood = nullptr;
    ID3D11PixelShader* g_psChrome = nullptr;
    ID3D11Buffer* g_shapeCB = nullptr;
    ID3D11Buffer* g_sceneCB = nullptr;
    ID3D11BlendState* g_blendCover = nullptr;
    ID3D11BlendState* g_blendOff = nullptr;
    ID3D11RasterizerState* g_rs = nullptr;
    ID3D11DepthStencilState* g_dss = nullptr;
    ID3D11SamplerState* g_sampler = nullptr;

    Target g_gbuffer[3], g_cells, g_patches, g_patches2, g_cube[6], g_gloss[5];
    Target g_boothMap;                      // 6. the booth's surfaces
    Target g_under;                         // 7. a copy of what's under the acrylic
    constexpr int kMap = 32, kMapFaces = 7; // the booth map (as in the shader)
    std::vector<CabinetAcrylic> g_acrylics;
    std::vector<CabinetAcrylic> g_screws;   // the screw heads (their bounds; z unused)
    float g_hoodTopPx = 0;                  // the hood's top where it meets the wall, in the canvas
    constexpr int kBounces = 2;             // patch-to-patch bounces after the direct light
    int g_outW = 0, g_w = 0, g_h = 0, g_offset = 0;
    bool g_built = false;       // for these sizes
    bool g_enabled = false;     // the cabinet is drawn at this size
    int g_cellCount = 0, g_patchCount = 0;

    bool Compile(const char* entry, const char* profile, ID3DBlob** blob)
    {
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(kShader, strlen(kShader), "cabinetlight", nullptr, nullptr, entry, profile, 0, 0, blob, &err);
        if (FAILED(hr))
            LOG("Cabinet shader %s failed: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        SafeRelease(err);
        return SUCCEEDED(hr);
    }

    bool CreateVS(const char* entry, ID3D11VertexShader** vs)
    {
        ID3DBlob* blob = nullptr;
        const bool ok = Compile(entry, "vs_4_0", &blob)
                     && SUCCEEDED(g_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, vs));
        SafeRelease(blob);
        return ok;
    }

    bool CreatePS(const char* entry, ID3D11PixelShader** ps)
    {
        ID3DBlob* blob = nullptr;
        const bool ok = Compile(entry, "ps_4_0", &blob)
                     && SUCCEEDED(g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ps));
        SafeRelease(blob);
        return ok;
    }

    float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

    void CommonState(ID3D11DeviceContext* ctx)
    {
        ctx->RSSetState(g_rs);
        ctx->OMSetDepthStencilState(g_dss, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &g_sampler);
    }

    void Viewport(ID3D11DeviceContext* ctx, int w, int h)
    {
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
    }

    void UnbindSRVs(ID3D11DeviceContext* ctx)
    {
        ID3D11ShaderResourceView* none[16] = {};
        ctx->PSSetShaderResources(0, 16, none);
    }

    // Builds everything that only depends on the window size: the G-buffer, the
    // scene constants, and the per-frame targets. The scene is laid out for an
    // outW x h window and drawn into a w x h canvas, offset to the right by offset
    // (a canvas wider than the window, for the perspective warp).
    void Build(ID3D11DeviceContext* ctx, int outW, int h, int w, int offset)
    {
        g_built = true;
        g_enabled = false;
        g_outW = outW;
        g_w = w;
        g_h = h;
        g_offset = offset;
        for (Target& t : g_gbuffer) t.Release();
        for (Target& t : g_cube) t.Release();
        for (Target& t : g_gloss) t.Release();
        g_cells.Release();
        g_patches.Release();
        g_patches2.Release();
        g_boothMap.Release();
        g_acrylics.clear();
        g_screws.clear();

        CabinetScene scene;
        if (!GetCabinetScene(outW, h, scene)) return;
        if (offset != 0 || w != outW)
        {
            // Shift into the canvas; the wall (the first shape) covers all of it
            for (CabinetShape& sh : scene.shapes)
            {
                sh.x0 += offset;
                if (sh.kind != CabinetShape::Dome) sh.x1 += offset;
            }
            if (!scene.shapes.empty()) { scene.shapes[0].x0 = 0; scene.shapes[0].x1 = static_cast<float>(w); }
            for (CabinetAcrylic& a : scene.acrylics) { a.x0 += offset; a.x1 += offset; }
            scene.originX += offset;
        }
        g_cellCount = static_cast<int>(scene.cells.size() < kMaxCells ? scene.cells.size() : kMaxCells);
        g_patchCount = static_cast<int>(scene.patches.size() < kMaxPatches ? scene.patches.size() : kMaxPatches);
        if (g_cellCount == 0) return;

        const int cw = (w + kCubeScale - 1) / kCubeScale, ch = (h + kCubeScale - 1) / kCubeScale;
        bool ok = true;
        for (Target& t : g_gbuffer) ok = ok && t.Create(g_dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        for (Target& t : g_cube) ok = ok && t.Create(g_dev, cw, ch, DXGI_FORMAT_R16G16B16A16_FLOAT);
        for (Target& t : g_gloss) ok = ok && t.Create(g_dev, cw, ch, DXGI_FORMAT_R16G16B16A16_FLOAT);
        ok = ok && g_cells.Create(g_dev, g_cellCount, 1, DXGI_FORMAT_R16G16B16A16_FLOAT)
                && g_patches.Create(g_dev, g_patchCount > 0 ? g_patchCount : 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT)
                && g_patches2.Create(g_dev, g_patchCount > 0 ? g_patchCount : 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT)
                && g_boothMap.Create(g_dev, kMap, kMap * kMapFaces, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (!ok)
        {
            LOG("Could not create the cabinet's lighting targets");
            return;
        }

        // Scene constants (they only change with the window size)
        SceneConstants* sc = new SceneConstants();
        for (int i = 0; i < g_cellCount; ++i)
        {
            const CabinetEmitter& c = scene.cells[i];
            float* p = sc->cellPos[i]; p[0] = c.pos[0]; p[1] = c.pos[1]; p[2] = c.pos[2]; p[3] = c.area;
            float* n = sc->cellNrm[i]; n[0] = c.normal[0]; n[1] = c.normal[1]; n[2] = c.normal[2];
            float* t = sc->cellUV[i]; t[0] = c.uv[0]; t[1] = c.uv[1]; t[2] = c.lod;
        }
        for (int i = 0; i < g_patchCount; ++i)
        {
            const CabinetEmitter& c = scene.patches[i];
            float* p = sc->patchPos[i]; p[0] = c.pos[0]; p[1] = c.pos[1]; p[2] = c.pos[2]; p[3] = c.area;
            float* n = sc->patchNrm[i]; n[0] = c.normal[0]; n[1] = c.normal[1]; n[2] = c.normal[2];
            float* a = sc->patchAlb[i]; a[0] = c.albedo[0]; a[1] = c.albedo[1]; a[2] = c.albedo[2];
        }
        sc->counts[0] = static_cast<float>(g_cellCount);
        sc->counts[1] = static_cast<float>(g_patchCount);
        sc->counts[2] = static_cast<float>(kCubeScale);
        sc->view[0] = static_cast<float>(w);
        sc->view[1] = static_cast<float>(h);
        sc->view[2] = scene.pxPerInch;
        sc->origin[0] = scene.originX;
        sc->origin[1] = scene.originY;
        // Room light as the radiance of a white wall it lights, in screen whites
        sc->light[0] = 1.0f;
        sc->light[1] = g_cfg.cabinetRoomLux / (3.14159265f * std::fmax(g_cfg.cabinetScreenNits, 1.0f));
        sc->light[2] = 1.0f;
        for (int i = 0; i < 3; ++i) sc->wall[i] = ToLinear(g_cfg.cabinetColor[i]);
        sc->geom[0] = scene.cornerIn;
        sc->geom[1] = std::sin(scene.faceAngle);
        sc->geom[2] = std::cos(scene.faceAngle);
        sc->booth[0] = scene.boothHalfWidth;
        sc->booth[1] = scene.boothBack;
        sc->booth[2] = scene.boothCeiling;
        sc->booth[3] = scene.boothFloor;
        sc->booth2[0] = scene.faceLenIn * sc->geom[1];      // where the side walls start: the faces' far ends
        sc->booth2[1] = scene.floorAlbedo;
        sc->booth2[2] = scene.faceLenIn;
        for (int i = 0; i < 3; ++i) sc->eye[i] = scene.eye[i];
        sc->eye[3] = scene.acrylicIn;
        sc->hood[0] = scene.cornerIn;
        sc->hood[1] = scene.hoodTop;
        sc->hood[2] = scene.hoodDepth;
        sc->hood[3] = scene.hoodSlope;
        sc->hood2[0] = scene.hoodHeight;
        sc->hood2[1] = scene.hoodFace;
        sc->hood2[2] = scene.hoodSplit;
        sc->hood2[3] = scene.hoodAlbedo;
        for (int i = 0; i < 4; ++i) sc->hoodGrille[i] = scene.hoodGrille[i];
        for (int i = 0; i < 3; ++i) sc->hoodOpening[i] = scene.hoodOpening[i];
        sc->hoodOpening[3] = scene.hoodCorner;
        if (scene.panelRect[2] > scene.panelRect[0])
        {
            sc->panelRect[0] = scene.panelRect[0] + offset; sc->panelRect[1] = scene.panelRect[1];
            sc->panelRect[2] = scene.panelRect[2] + offset; sc->panelRect[3] = scene.panelRect[3];
        }
        sc->panelSurround[0] = scene.panelSurround;
        for (int i = 0; i < 3; ++i) sc->hoodFlange[i] = scene.hoodFlange[i];
        const int screws = static_cast<int>(scene.hoodScrews.size() < kMaxHoodScrews ? scene.hoodScrews.size() : kMaxHoodScrews);
        sc->hoodFlange[3] = static_cast<float>(screws);
        for (int i = 0; i < screws; ++i)
            for (int k = 0; k < 3; ++k) sc->hoodScrews[i][k] = scene.hoodScrews[i][k];
        g_hoodTopPx = scene.originY + scene.hoodTop * scene.pxPerInch;
        g_acrylics = scene.acrylics;
        for (const CabinetShape& sh : scene.shapes)
            if (sh.kind == CabinetShape::Dome)
                g_screws.push_back({ sh.x0 - sh.x1 - 1, sh.y0 - sh.x1 - 1, sh.x0 + sh.x1 + 1, sh.y0 + sh.x1 + 1, 0 });
        ctx->UpdateSubresource(g_sceneCB, 0, nullptr, sc, 0, 0);
        delete sc;

        // G-buffer: the shapes, back to front
        const float clear[4] = { 0, 0, 0, 0 };
        ID3D11RenderTargetView* rtvs[3] = { g_gbuffer[0].rtv, g_gbuffer[1].rtv, g_gbuffer[2].rtv };
        for (ID3D11RenderTargetView* r : rtvs) ctx->ClearRenderTargetView(r, clear);
        CommonState(ctx);
        UnbindSRVs(ctx);
        ctx->OMSetRenderTargets(3, rtvs, nullptr);
        ctx->OMSetBlendState(g_blendCover, nullptr, 0xFFFFFFFF);
        Viewport(ctx, w, h);
        ctx->VSSetShader(g_vsShape, nullptr, 0);
        ctx->PSSetShader(g_psShape, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &g_shapeCB);
        ctx->PSSetConstantBuffers(0, 1, &g_shapeCB);
        for (const CabinetShape& s : scene.shapes)
        {
            ShapeConstants c = {
                { s.x0, s.y0, s.x1, s.y1 },
                { s.z0, s.z1, s.edge, static_cast<float>(s.kind) },
                { ToLinear(s.albedo[0]), ToLinear(s.albedo[1]), ToLinear(s.albedo[2]), static_cast<float>(s.axis) },
                { s.roughness, s.metalness, scene.pxPerInch, s.powderCoat ? 1.0f : 0.0f },
                { static_cast<float>(w), static_cast<float>(h), 0, 0 } };
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(ctx->Map(g_shapeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
            memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(g_shapeCB, 0);
            ctx->Draw(4, 0);
        }
        g_enabled = true;
        LOG("Cabinet built: %zu shapes, %d light cells, %d booth patches, lighting at %dx%d",
            scene.shapes.size(), g_cellCount, g_patchCount, cw, ch);
    }
}

bool CabinetLightInit(ID3D11Device* dev)
{
    g_dev = dev;
    D3D11_BUFFER_DESC sb = {};
    sb.ByteWidth = sizeof(ShapeConstants);
    sb.Usage = D3D11_USAGE_DYNAMIC;
    sb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    sb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(SceneConstants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    // Coverage blending for the G-buffer (anti-aliased fastener edges); color
    // channels only, so the stored alpha stays as the coverage of the base
    D3D11_BLEND_DESC bc = {};
    bc.RenderTarget[0].BlendEnable = TRUE;
    bc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    bc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC bo = {};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    const bool ok = CreateVS("VSFull", &g_vsFull) && CreateVS("VSShape", &g_vsShape)
        && CreatePS("PSShape", &g_psShape) && CreatePS("PSCells", &g_psCells) && CreatePS("PSPatches", &g_psPatches) && CreatePS("PSBounce", &g_psBounce)
        && CreatePS("PSCube", &g_psCube) && CreatePS("PSGloss", &g_psGloss) && CreatePS("PSShade", &g_psShade)
        && CreatePS("PSBoothMap", &g_psBoothMap) && CreatePS("PSAcrylic", &g_psAcrylic) && CreatePS("PSHood", &g_psHood) && CreatePS("PSChrome", &g_psChrome)
        && SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &g_shapeCB))
        && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_sceneCB))
        && SUCCEEDED(dev->CreateBlendState(&bc, &g_blendCover))
        && SUCCEEDED(dev->CreateBlendState(&bo, &g_blendOff))
        && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs))
        && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_dss))
        && SUCCEEDED(dev->CreateSamplerState(&sd, &g_sampler));
    if (!ok)
    {
        LOG("Cabinet lighting unavailable; drawing the screens on black");
        g_psShade = nullptr;   // marks it unavailable (the rest is left for process exit)
    }
    return ok;
}

void CabinetLightInvalidate()
{
    g_built = false;
}

bool CabinetLightRender(ID3D11DeviceContext* ctx, int outWidth, int height, int width, int offset,
                        ID3D11ShaderResourceView* frame, ID3D11RenderTargetView* target)
{
    if (!g_psShade || !frame || !target) return false;
    if (!g_built || outWidth != g_outW || width != g_w || height != g_h || offset != g_offset)
        Build(ctx, outWidth, height, width, offset);
    if (!g_enabled) return false;

    CommonState(ctx);
    ctx->OMSetBlendState(g_blendOff, nullptr, 0xFFFFFFFF);
    ctx->VSSetShader(g_vsFull, nullptr, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_sceneCB);

    // 1. Cells from the game's frame
    UnbindSRVs(ctx);
    ctx->OMSetRenderTargets(1, &g_cells.rtv, nullptr);
    Viewport(ctx, g_cellCount, 1);
    ctx->PSSetShader(g_psCells, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &frame);
    ctx->Draw(4, 0);

    // 2. Booth patches, lit by the cells, then bouncing between each other
    UnbindSRVs(ctx);
    ctx->OMSetRenderTargets(1, &g_patches.rtv, nullptr);
    Viewport(ctx, g_patchCount > 0 ? g_patchCount : 1, 1);
    ctx->PSSetShader(g_psPatches, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &g_cells.srv);
    ctx->Draw(4, 0);
    Target* prev = &g_patches;
    Target* next = &g_patches2;
    ctx->PSSetShader(g_psBounce, nullptr, 0);
    for (int b = 0; b < kBounces; ++b)
    {
        UnbindSRVs(ctx);
        ctx->OMSetRenderTargets(1, &next->rtv, nullptr);
        ID3D11ShaderResourceView* in[2] = { g_cells.srv, prev->srv };
        ctx->PSSetShaderResources(0, 2, in);
        ctx->Draw(4, 0);
        Target* t = prev; prev = next; next = t;
    }

    // 6. (For later) the booth map, from the cells and patches
    UnbindSRVs(ctx);
    ctx->OMSetRenderTargets(1, &g_boothMap.rtv, nullptr);
    Viewport(ctx, kMap, kMap * kMapFaces);
    ctx->PSSetShader(g_psBoothMap, nullptr, 0);
    {
        ID3D11ShaderResourceView* in[2] = { g_cells.srv, prev->srv };
        ctx->PSSetShaderResources(0, 2, in);
    }
    ctx->Draw(4, 0);

    // 3. Ambient cube, from the cells and patches
    UnbindSRVs(ctx);
    ID3D11RenderTargetView* cubes[6];
    for (int i = 0; i < 6; ++i) cubes[i] = g_cube[i].rtv;
    ctx->OMSetRenderTargets(6, cubes, nullptr);
    Viewport(ctx, (width + kCubeScale - 1) / kCubeScale, (height + kCubeScale - 1) / kCubeScale);
    ctx->PSSetShader(g_psCube, nullptr, 0);
    ID3D11ShaderResourceView* sources[2] = { g_cells.srv, prev->srv };
    ctx->PSSetShaderResources(0, 2, sources);
    ctx->Draw(4, 0);

    // 4. Gloss, from the cells and patches (same inputs)
    ID3D11RenderTargetView* glosses[5];
    for (int i = 0; i < 5; ++i) glosses[i] = g_gloss[i].rtv;
    ctx->OMSetRenderTargets(5, glosses, nullptr);
    ctx->PSSetShader(g_psGloss, nullptr, 0);
    ctx->Draw(4, 0);

    // 5. Shade the cabinet into the target
    UnbindSRVs(ctx);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    Viewport(ctx, width, height);
    ctx->PSSetShader(g_psShade, nullptr, 0);
    ID3D11ShaderResourceView* inputs[14] = { g_gbuffer[0].srv, g_gbuffer[1].srv, g_gbuffer[2].srv,
                                             g_cube[0].srv, g_cube[1].srv, g_cube[2].srv,
                                             g_cube[3].srv, g_cube[4].srv, g_cube[5].srv,
                                             g_gloss[0].srv, g_gloss[1].srv, g_gloss[2].srv,
                                             g_gloss[3].srv, g_gloss[4].srv };
    ctx->PSSetShaderResources(0, 14, inputs);
    ctx->Draw(4, 0);
    UnbindSRVs(ctx);
    return true;
}

bool CabinetLightReflect(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target)
{
    if (!g_psAcrylic || !g_enabled || !target || g_acrylics.empty()) return false;

    // A copy of the target to read from (it's what's under the acrylic, and what
    // the front of the cabinet looks like in reflections)
    ID3D11Resource* res = nullptr;
    target->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (!res || FAILED(res->QueryInterface(IID_PPV_ARGS(&tex)))) { SafeRelease(res); return false; }
    SafeRelease(res);
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    if (static_cast<int>(td.Width) != g_w || static_cast<int>(td.Height) != g_h || td.SampleDesc.Count != 1)
    {
        SafeRelease(tex);
        return false;
    }
    D3D11_TEXTURE2D_DESC ud = {};
    if (g_under.tex) g_under.tex->GetDesc(&ud);
    if (!g_under.tex || ud.Width != td.Width || ud.Height != td.Height || ud.Format != td.Format)
    {
        if (!g_under.Create(g_dev, g_w, g_h, td.Format, true))
        {
            LOG("Could not create the acrylic's copy of the screens (%dx%d)", g_w, g_h);
            g_under.Release();
            SafeRelease(tex);
            g_acrylics.clear();     // don't keep trying
            return false;
        }
    }
    ctx->CopySubresourceRegion(g_under.tex, 0, 0, 0, 0, tex, 0, nullptr);
    SafeRelease(tex);
    ctx->GenerateMips(g_under.srv);

    CommonState(ctx);
    UnbindSRVs(ctx);
    ctx->OMSetBlendState(g_blendOff, nullptr, 0xFFFFFFFF);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    Viewport(ctx, g_w, g_h);
    ctx->VSSetShader(g_vsShape, nullptr, 0);
    ctx->PSSetShader(g_psAcrylic, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_shapeCB);
    ctx->PSSetConstantBuffers(0, 1, &g_shapeCB);
    ctx->PSSetConstantBuffers(1, 1, &g_sceneCB);
    ID3D11ShaderResourceView* in[3] = { g_under.srv, g_boothMap.srv, g_gbuffer[2].srv };
    ctx->PSSetShaderResources(0, 3, in);
    ID3D11ShaderResourceView* gbuffer[2] = { g_gbuffer[0].srv, g_gbuffer[1].srv };
    ctx->PSSetShaderResources(14, 2, gbuffer);
    auto draw = [&](const CabinetAcrylic& a)
    {
        const ShapeConstants c = {
            { std::floor(a.x0 + 0.5f), std::floor(a.y0 + 0.5f), std::floor(a.x1 + 0.5f), std::floor(a.y1 + 0.5f) },
            { a.z, a.z, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
            { static_cast<float>(g_w), static_cast<float>(g_h), 0, 0 } };
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(g_shapeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(g_shapeCB, 0);
        ctx->Draw(4, 0);
    };
    for (const CabinetAcrylic& a : g_acrylics) draw(a);

    // The chrome screw heads, over it, blended in by how much of each pixel they cover
    ctx->PSSetShader(g_psChrome, nullptr, 0);
    ctx->OMSetBlendState(g_blendCover, nullptr, 0xFFFFFFFF);
    for (const CabinetAcrylic& a : g_screws) draw(a);
    UnbindSRVs(ctx);
    return true;
}

bool CabinetLightHood(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, int width, int height,
                      int offset, bool perspective)
{
    if (!g_psHood || !g_enabled || !target || !g_under.srv) return false;
    CommonState(ctx);
    UnbindSRVs(ctx);
    ctx->OMSetBlendState(g_blendOff, nullptr, 0xFFFFFFFF);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    Viewport(ctx, width, height);
    ctx->VSSetShader(g_vsShape, nullptr, 0);
    ctx->PSSetShader(g_psHood, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_shapeCB);
    ctx->PSSetConstantBuffers(0, 1, &g_shapeCB);
    ctx->PSSetConstantBuffers(1, 1, &g_sceneCB);
    ID3D11ShaderResourceView* in[2] = { g_under.srv, g_boothMap.srv };
    ctx->PSSetShaderResources(0, 2, in);
    // From the hood's top where it meets the wall (nothing of it shows above
    // that) down, across the whole output
    const ShapeConstants c = {
        { 0, std::floor(std::fmax(g_hoodTopPx - 2, 0.0f)), static_cast<float>(width), static_cast<float>(height) },
        { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        { static_cast<float>(width), static_cast<float>(height), static_cast<float>(offset), perspective ? 1.0f : 0.0f } };
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_shapeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(g_shapeCB, 0);
    ctx->Draw(4, 0);
    UnbindSRVs(ctx);
    return true;
}
