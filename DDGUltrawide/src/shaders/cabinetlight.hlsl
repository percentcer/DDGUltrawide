#define MAX_CELLS 128
#define MAX_PATCHES 176
// The arcade's own lighting reaches into the booth mostly through its windows
// (they're light sources among the booth's patches); this much of it gets in
// everywhere else (over the walls, through the top)
static const float ROOM_LEAK = 0.3;
static const float OUTSIDE = 1.0;         // the arcade through the windows (as kOutside)
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
    float4 patchAlb[MAX_PATCHES];   // linear rgb, emission (screen whites)
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
    float4 grilleCells;             // their holes: across, down; radius, rim (of their spacing across)
    float4 hoodOpening;             // the touch monitor's opening: half width, down its front (from, to); corners' radius
    float4 panelRect;               // the touch panel in the canvas (px; 0 when it isn't there)
    float4 panelSurround;           // its surround (px)
    float4 panelSheet;              // with perspective: the panel's sheet, leaning with the hood's front: half width, top, bottom (inches), how far in front
    float4 panelFrame;              // ...its frame (along the sheet): border at the sides, top, bottom; edges' rounding
    float4 panelFrameScrews;        // ...its screws: in from its sides, down from its top (3)
    float4 panelFrameScrew;         // ...their head radius
    float4 backWindows[3];          // the booth's windows: in its back wall (x from, y from, x to, y to)
    float4 sideWindow;              // ...and in each side wall (z from, y from, z to, y to)
    float4 roofLeds;                // the roof's LED panels: x from, to (either side of the middle); z from, to
    float4 roofLed;                 // ...how bright (screen whites), their soft edge (inches); the hood's gloss (radians its reflections blur)
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
Texture2D<float4> gAO : register(t16);     // 0. contact shadows (1 = open)
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

// A fastener's head at d (from its middle, in radii): the button head's
// surface, and a tamper-proof Torx recess in it: a six-lobed star over half the
// head across, turned however it was driven in (turn), with a pin standing in
// its middle. Its walls, just inside its edge, face in and catch the light (a
// bright rim around it, as in photos); its floor is dark. The normal is in the
// surface's own axes (slope: head height over radius); aa is a pixel in radii.
// Returns the height (of the head's) and how much is recess floor.
float HeadSurface(float2 d, float slope, float turn, float aa, out float3 n)
{
    float l = length(d);
    float2 dir = l > 1e-4 ? d / l : 0;
    float h, dh;
    ButtonHead(l, h, dh);
    n = normalize(float3(dir * -dh * slope, 1));
    float star = 0.33 + 0.08 * cos(6 * (atan2(d.y, d.x) + turn));
    float recess = saturate((star - l) / aa + 0.5);
    float wall = recess * saturate((l - (star - max(0.08, aa))) / aa + 0.5);
    float pin = saturate((0.13 - l) / aa + 0.5);
    float floor_ = saturate(recess - wall) * (1 - pin);
    n = normalize(lerp(n, float3(dir * 0.3, 1), recess - wall));   // the pin's top, and the recess's floor: flat
    n = normalize(lerp(n, float3(-dir, 1), wall));
    return h - 0.5 * (floor_ + wall * 0.5);
}

float HeadFloor(float2 d, float turn, float aa)
{
    float l = length(d);
    float star = 0.33 + 0.08 * cos(6 * (atan2(d.y, d.x) + turn));
    float recess = saturate((star - l) / aa + 0.5);
    float wall = recess * saturate((l - (star - max(0.08, aa))) / aa + 0.5);
    float pin = saturate((0.13 - l) / aa + 0.5);
    return saturate(recess - wall) * (1 - pin);
}

float HeadTurn(float2 canvasCenter)
{
    return frac(sin(dot(canvasCenter, float2(12.9898, 78.233))) * 43758.5453) * 6.2832;
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
        // Dome: a fastener's head (HeadSurface)
        float2 d = (p - sRect.xy) / sRect.z;
        cover = saturate((1 - length(d)) * sRect.z + 0.5);
        float turn = HeadTurn(sRect.xy), aa = 1 / sRect.z;   // (a pixel, in radii)
        z += sParams.z * HeadSurface(d, sParams.z / (sRect.z / sMat.z), turn, aa, n);
        float floor_ = HeadFloor(d, turn, aa);
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
    return float4(patchAlb[i].rgb * PatchDirect(i) / PI + patchAlb[i].w, 1);
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
    return float4(patchAlb[i].rgb * E / PI + patchAlb[i].w, 1);
}

// Whether the hood is between P and a light at S (0 if so). Most paths can't
// reach it: both ends above its top
float HoodHit(float3 P, float3 r, out float3 N);
float HoodShadow(float3 P, float3 S)
{
    if (P.y < hood.y && S.y < hood.y) return 1;
    float3 d = S - P;
    float len = length(d), t;
    float3 N;
    t = HoodHit(P, d / len, N);
    return t < len - 0.1 ? 0 : 1;
}

// ---- 0. Contact shadows, from the G-buffer's heights (t1) ----
static const float AO_RADIUS = 1.5;      // inches around
static const float AO_STRENGTH = 0.85;
float AOHeight(float2 px)
{
    float4 g = t1.SampleLevel(linearClamp, px / view.xy, 0);
    return g.z * 16 * g.w;
}

float4 PSAO(float4 pos : SV_Position) : SV_Target
{
    float h0 = AOHeight(pos.xy);
    float occluded = 0;
    [unroll] for (int d = 0; d < 8; ++d)
    {
        float a = d * PI / 4 + 0.39;
        float2 dir = float2(cos(a), sin(a));
        float best = 0;
        [unroll] for (int s = 1; s <= 6; ++s)
        {
            float dist = AO_RADIUS * s / 6;                       // inches
            float dh = AOHeight(pos.xy + dir * dist * view.z) - h0;
            best = max(best, dh / sqrt(dh * dh + dist * dist) * (1 - dist / (AO_RADIUS * 1.15)));
        }
        occluded += best;
    }
    return float4(saturate(1 - AO_STRENGTH * occluded / 8).xxx, 1);
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
        e = FromSource(P, cellPos[i].xyz, cellNrm[i].xyz, cellPos[i].w, t0.Load(int3(i, 0, 0)).rgb, w)
          * HoodShadow(P, cellPos[i].xyz);
        w = ToLocal(w, ex, ez);
        c0 += e * max(w.x, 0); c1 += e * max(-w.x, 0);
        c2 += e * max(w.y, 0); c3 += e * max(-w.y, 0);
        c4 += e * max(w.z, 0); c5 += e * max(-w.z, 0);
    }
    for (int j = 0; j < (int)counts.y; ++j)
    {
        e = FromSource(P, patchPos[j].xyz, patchNrm[j].xyz, patchPos[j].w, t1.Load(int3(j, 0, 0)).rgb, w)
          * HoodShadow(P, patchPos[j].xyz);
        w = ToLocal(w, ex, ez);
        c0 += e * max(w.x, 0); c1 += e * max(-w.x, 0);
        c2 += e * max(w.y, 0); c3 += e * max(-w.y, 0);
        c4 += e * max(w.z, 0); c5 += e * max(-w.z, 0);
    }
    // The arcade's own lighting: mostly from above and in front
    float room = light.y * PI * ROOM_LEAK;
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
            e = FromSource(P, cellPos[i].xyz, cellNrm[i].xyz, cellPos[i].w, t0.Load(int3(i, 0, 0)).rgb, w)
              * HoodShadow(P, cellPos[i].xyz);
        else
        {
            int j = i - (int)counts.x;
            e = FromSource(P, patchPos[j].xyz, patchNrm[j].xyz, patchPos[j].w, t1.Load(int3(j, 0, 0)).rgb, w)
              * HoodShadow(P, patchPos[j].xyz);
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
    float k = (GLOSS_POWER + 1) / (2 * PI) / max(light.x, 1e-3), room = light.y * ROOM_LEAK;
    Gloss o;
    o.px = float4(g0 * k + room * 0.25, 1); o.nx = float4(g1 * k + room * 0.25, 1);
    o.py = float4(g2 * k + room * 0.15, 1); o.ny = float4(g3 * k + room * 0.9, 1);
    o.pz = float4(g4 * k + room * 0.6, 1);
    return o;
}

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
    float ao = gAO.Load(ip).x;                          // contact shadows
    float3 diffuse = albedo * (1 - mat.y) * (1 - F) * Irradiance(n, uv) / PI * ao;
    float3 env = lerp(Glossy(r, uv), Irradiance(r, uv) / PI, saturate(mat.x * 1.6 - 0.3));
    float3 specular = F * lerp(env, wallSeen, intoWall) * lerp(1, ao, 0.6);

    // The arcade's ceiling lights, overhead and a little toward the player: a
    // highlight, crisp on chrome and broad and faint on satin paint
    float power = exp2(6 * (1 - mat.x) + 1);           // 37 for chrome, 20 for satin
    specular += F * light.y * 2.5 * pow(saturate(dot(r, OVERHEAD)), power) * (1 - intoWall);
    float3 c = (diffuse + specular) * light.z;
    // Leave mid-tones alone; roll bright highlights off smoothly instead of clipping
    c = c < 0.8 ? c : 0.8 + 0.2 * (1 - exp(-(c - 0.8) / 0.2));
    return float4(LinearToSrgb(saturate(c)), 1);
}

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
    return float4(albedo * (E / PI + light.y * ROOM_LEAK), 1);
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

    // The booth's windows: the arcade outside, as its own lighting lights it
    // (mid grey)
    bool window = false;
    if (face == 0)
        for (int w = 0; w < 3; ++w)
            window = window || all(X.xy >= backWindows[w].xy) && all(X.xy <= backWindows[w].zw);
    else if (face <= 2)
        window = all(X.zy >= sideWindow.xy) && all(X.zy <= sideWindow.zw);
    if (window) return OUTSIDE * light.y;

    // The roof's LED panels: diffusers in a recess, so their edges are soft,
    // and softer still as seen in a glossy surface (by how far it spreads its
    // reflection, over how far away they are), so they don't glint off in
    // single pixels
    float led = 0;
    if (face == 3)
    {
        float soft = roofLed.y + t * spread;           // inches, either side of an edge (the glow, and the blur)
        float2 c = float2((roofLeds.x + roofLeds.y) / 2, (roofLeds.z + roofLeds.w) / 2);
        float2 h = float2((roofLeds.y - roofLeds.x) / 2, (roofLeds.w - roofLeds.z) / 2);
        float2 d = abs(float2(abs(X.x), X.z) - c) - h;  // past the edges (negative: inside)
        float2 k = saturate(0.5 - d / (2 * soft));
        led = k.x * k.y * min(1, 2 * h.x / (2 * soft)) * min(1, 2 * h.y / (2 * soft));   // (spread thin as it blurs)
    }

    float2 uv = face == 0 ? float2((X.x + hw) / (2 * hw), (X.y - cy) / (fy - cy))
              : face <= 2 ? float2((X.z - fz) / (bk - fz), (X.y - cy) / (fy - cy))
              : float2((X.x + hw) / (2 * hw), X.z / bk);
    return lerp(BoothMap(face, uv), roofLed.x, led);
}

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
    // A flange's front end: rounded over, from its top (facing up) down to
    // facing the player, so it shows a gradient (what's above, down to the
    // booth behind) rather than one flat color. Blurred a little, as the other
    // bends are, since it's only a few pixels tall.
    if (flange && N.z > 0.5)
    {
        float down = saturate((X.y - (hood.y - hoodFlange.z)) / hoodFlange.z);   // 0 at its top, 1 at its bottom
        float a = down * down * 1.5708;                 // (a quarter round, seen side on: most of its height is near the top)
        n = normalize(float3(0, -cos(a), sin(a)));
        bend = 0.6;                                     // (not the flat sides' full blur, which would wash it out)
    }

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
        else if (in_ > hoodGrille.x && in_ < hoodGrille.z && s > hoodGrille.y && s < hoodGrille.w)
        {
            // A speaker grille: a honeycomb of round holes, black inside, their
            // rims rounded over (catching the light). Smoothed over a pixel's
            // footprint, and toward the grille's overall look where the holes
            // get too small to see.
            float2 cell = float2((hoodGrille.z - hoodGrille.x) / (grilleCells.x + 0.5), (hoodGrille.w - hoodGrille.y) / grilleCells.y);
            float2 g = float2(in_ - hoodGrille.x, s - hoodGrille.y) / cell;     // in cells
            float2 best = 0;
            float d = 1e9;
            // The nearest hole, of the grille's own (whole ones: no cut-off
            // holes at its edges)
            for (float row = floor(g.y) - 1; row <= floor(g.y) + 1; ++row)
            {
                if (row < 0 || row >= grilleCells.y) continue;
                float stagger = 0.5 * fmod(row, 2);
                float2 c = float2(clamp(floor(g.x - stagger), 0, grilleCells.x - 1) + 0.5 + stagger, row + 0.5);
                // A regular honeycomb on the panel (in spacings): it leans back,
                // so seen from the front (as the drawing is), its rows crowd
                float dc = length((g - c) * float2(1, 0.866));
                if (dc < d) { d = dc; best = c; }
            }
            float px = (sTarget.w > 0.5 ? max(eye.z - X.z, 1) / eye.z : 1) / view.z / cell.x;   // a pixel, in spacings
            float R = grilleCells.z, W = grilleCells.w, a = max(px, 1e-3);
            float hole = 1 - smoothstep(R - a, R + a, d);
            float rim = (1 - hole) * (1 - smoothstep(R + W - a, R + W + a, d));
            float seen = saturate(1.5 - px * 3);                                // 1 while a hole spans a few pixels
            float2 toward = (best - g) * cell;                                  // toward the hole's middle (in, down)
            float3 inward = toward.x * float3(-sign(X.x), 0, 0) + toward.y * normalize(float3(0, 1, hood.w));
            n = normalize(n + normalize(inward + 1e-6) * 1.5 * rim * seen);
            bend = max(bend, rim);
            float open = lerp(3.14159 * R * R / 0.866, hole, seen);             // how much is hole (on average, far off)
            diffuseK = 1 - open;
            glossK = 1 - open;
        }
        else if (abs(in_ - hood2.z) < 0.03) { diffuseK = 0.3; glossK = 0.2; }
        else if (abs(X.x) < hoodOpening.x && s > hoodOpening.y && s < hoodOpening.z) diffuseK = 0.4;
    }
    // Orange peel: fainter than on the cabinet (at the shallow angle the top is
    // seen from, it glitters), and fading out toward the rounded edges, where it
    // would break up their thin highlights (none on the rounded ends at all)
    float edgeDist = isTop ? min(min(D - X.z, in_ - hoodOpening.w), abs(past)) : isFront ? s : 0;
    if (isFront && in_ > hoodGrille.x && in_ < hoodGrille.z && s > hoodGrille.y && s < hoodGrille.w) edgeDist = 0;
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
    return diffuse + F * glossK * SeenFar(X + N * 0.02, r2, roofLed.z + 0.5 * bend, t);
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

// Drawn with the acrylic over it; and the reflections alone (linear), so the
// screens can be drawn again under them
struct AcrylicOut
{
    float4 color : SV_Target0;
    float4 reflected : SV_Target1;
};

AcrylicOut Acrylic(float3 under, float3 reflected)
{
    AcrylicOut o;
    o.color = float4(LinearToSrgb(saturate(under + reflected)), 1);
    o.reflected = float4(reflected, 1);
    return o;
}

AcrylicOut PSAcrylic(float4 pos : SV_Position)
{
    int3 ip = int3(pos.xy, 0);
    float3 under = SrgbToLinear(t0.Load(ip).rgb);
    float sheet = 1 - t2.Load(ip).y;                    // not where a (metal) screw sits on it
    float3 ex, ez;
    float3 P = Place(pos.xy, sParams.x, ex, ez);
    float3 v = normalize(eye.xyz - P);
    float c = dot(v, ez);
    if (c <= 1e-3 || sheet <= 0) return Acrylic(under, 0);

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
    return Acrylic(under, reflected * sheet);
}

// ---- 7. (continued) The chrome screw heads: mirrors, curved. What's under
// them (t0), the booth map (t1), and the G-buffer (t2, gAlbedo, gGeo), drawn
// over what's there by how much of each pixel is metal ----
// One sharp reflection off a screw head, at canvas point px (its surface
// interpolated from the G-buffer between pixels)
float3 ChromeAt(float2 px, float3 albedo)
{
    float4 geo = gGeo.SampleLevel(linearClamp, px / view.xy, 0);
    float2 nxy = geo.xy * 2 - 1;
    float3 n = float3(nxy, sqrt(saturate(1 - dot(nxy, nxy))));
    float3 ex, ez;
    float3 P = Place(px, geo.z * 16, ex, ez);
    float3 N = normalize(ex * n.x + float3(0, n.y, 0) + ez * n.z);    // from the surface's axes
    float3 v = normalize(eye.xyz - P);
    float c = saturate(dot(N, v));
    float3 F = albedo + (1 - albedo) * pow(1 - c, 5);                // metal: tinted by its color
    float3 r = 2 * c * N - v;
    // What it mirrors, and the arcade's ceiling lights: a sharp glint
    return F * (Seen(P + N * 0.05, r) + light.y * 2.5 * pow(saturate(dot(r, OVERHEAD)), 64));
}

float4 PSChrome(float4 pos : SV_Position) : SV_Target
{
    int3 ip = int3(pos.xy, 0);
    float metal = t2.Load(ip).y;
    if (metal < 0.01) discard;
    float3 albedo = gAlbedo.Load(ip).rgb;

    // A head is only a few pixels across, and its surface turns a lot across
    // each one: its reflections are taken sharp, at 3 x 3 points across the
    // pixel, and averaged, so a glint covering part of a pixel shows as part
    // of it (rather than flickering on and off)
    float3 L = 0;
    [loop] for (int k = 0; k < 9; ++k)                                  // (a loop: unrolled, it takes long to compile)
        L += ChromeAt(pos.xy + (float2(k % 3, k / 3) - 1) / 3, albedo);
    L *= light.z / 9;
    L = L < 0.8 ? L : 0.8 + 0.2 * (1 - exp(-(L - 0.8) / 0.2));
    return float4(LinearToSrgb(saturate(L)), metal);
}

// ---- 7. (with perspective) The screw heads again, over the warped view, in
// it: each pixel's camera ray, at 3 x 3 points across it, traced to the head
// (on its surface's plane, then onto the head's curve), and the head shaded
// there as in PSChrome. Drawn at its true size, so it's as sharp at the
// window's edges (where the warp magnifies the flat drawing) as in the middle.
// sRect: its square in the output; sParams: its middle in the canvas (px),
// radius (px, in the canvas), base height (inches); sAlbedo: color, head height
// (inches); sTarget.z: the canvas's offset. ----
float4 PSScrew(float4 pos : SV_Position) : SV_Target
{
    // Its base on the face's surface, as the warp sees the whole flat drawing
    // (the frames it sits on too), so it lines up with them; only the head's
    // own height stands off it
    float3 ex, ez;
    float3 C = Place(sParams.xy, 0, ex, ez);
    float3 ey = float3(0, 1, 0);
    float R = sParams.z / view.z, H = sAlbedo.w;                         // inches
    float turn = HeadTurn(sParams.xy);
    float aa = 0.5 / max(sParams.z * eye.z / max(eye.z - C.z, 1), 1);   // a sample's width, in radii (about)
    float3 sum = 0;
    float hits = 0;
    [loop] for (int k = 0; k < 9; ++k)
    {
        float2 px = pos.xy + (float2(k % 3, k / 3) - 1) / 3;
        float2 f = (float2(px.x + sTarget.z, px.y) - origin.xy) / view.z;
        float3 r = normalize(float3(f, 0) - eye.xyz);
        float denom = dot(r, ez);
        if (denom > -1e-4) continue;
        // Onto the plane at the head's middle height, then onto its curve
        float hz = 0.7 * H, l = 2;
        float2 d = 0;
        float3 X = 0;
        [unroll] for (int it = 0; it < 3; ++it)
        {
            float t = dot(C + ez * hz - eye.xyz, ez) / denom;
            X = eye.xyz + r * t;
            d = float2(dot(X - C, ex), dot(X - C, ey)) / R;
            l = length(d);
            float h, dh;
            ButtonHead(min(l, 1), h, dh);
            hz = H * h;
        }
        if (l >= 1) continue;
        float3 n;
        HeadSurface(d, H / R, turn, aa, n);
        float floor_ = HeadFloor(d, turn, aa);
        float3 albedo = sAlbedo.rgb * (1 - 0.75 * floor_);
        float3 N = normalize(ex * n.x + ey * n.y + ez * n.z);
        float3 v = -r;
        float c = saturate(dot(N, v));
        float3 F = albedo + (1 - albedo) * pow(1 - c, 5);
        float3 rr = 2 * c * N - v;
        sum += F * (Seen(X + N * 0.05, rr) + light.y * 2.5 * pow(saturate(dot(rr, OVERHEAD)), 64));
        hits += 1;
    }
    if (hits <= 0) discard;
    float3 L = sum / hits * light.z;
    L = L < 0.8 ? L : 0.8 + 0.2 * (1 - exp(-(L - 0.8) / 0.2));
    return float4(LinearToSrgb(saturate(L)), hits / 9);
}

// ---- 8. The hood: each pixel's view (from the camera, or straight on) traced
// to it; the cabinet as drawn (t0) is what it reflects. Where it misses, and
// where the touch panel is (it sits in the hood's opening), nothing's drawn. ----
// The touch panel's frame, at Q on its sheet (leaning with the hood's front;
// q: across from the middle and down from the panel's top, inches along it),
// seen along r: glossy black acrylic, its edges rounded over inside and out
// (the reflection blurring over them, as on the hood's bends), with black
// screws. outer: how far inside its outside edge; inner: how far outside the
// panel's opening.
float3 FrameShade(float3 Q, float2 q, float3 r, float outer, float inner, float panelH)
{
    float3 across = float3(sign(q.x), 0, 0), down = normalize(float3(0, 1, hood.w));
    float3 nS = normalize(float3(0, -hood.w, 1));
    float R = panelFrame.w;

    // Toward the nearest outside edge, and away from the nearest opening edge
    float hw = panelSheet.x;
    float3 outward = hw + panelFrame.x - abs(q.x) <= min(q.y + panelFrame.y, panelH + panelFrame.z - q.y) ? across
                   : q.y + panelFrame.y < panelH + panelFrame.z - q.y ? -down : down;
    float3 inward = abs(q.x) - hw >= max(-q.y, q.y - panelH) ? -across : -q.y > q.y - panelH ? down : -down;
    float bendOut = saturate(1 - outer / R), bendIn = saturate(1 - inner / R);
    float3 n = normalize(nS + outward * 1.5 * bendOut + inward * 1.5 * bendIn);
    float bend = max(bendOut, bendIn);

    // The screws: 3 down each side
    bool screw = false;
    float side = hw + panelFrame.x - panelFrameScrews.x;
    for (int i = 0; i < 3; ++i)
    {
        float2 d = (float2(abs(q.x), q.y + panelFrame.y) - float2(side, panelFrameScrews[1 + i])) / panelFrameScrew.x;
        float l = length(d);
        if (l >= 1) continue;
        float h, dh;
        ButtonHead(l, h, dh);
        float2 dir = l > 1e-4 ? d / l : 0;
        n = normalize(nS + (across * dir.x + down * dir.y) * -dh * 0.3);
        screw = true;
        bend = l;
    }

    float c = saturate(dot(-r, n));
    float F = screw ? 0.12 + 0.88 * pow(1 - c, 5) : Fresnel(max(c, 1e-3), 1.49);
    float t;
    float3 diffuse = BoothMap(HOOD_FRONT, float2((Q.x + hood.x) / (2 * hood.x), (Q.y - hood.y) / hood2.x)) * 0.4;
    return diffuse + F * SeenFar(Q + n * 0.02, r + 2 * c * n, 0.01 + 0.5 * bend, t);
}

// The hood as seen at canvas point cpx: its color (linear, before exposure),
// with alpha 1, or alpha 0 where it isn't (or the touch panel shows through,
// in the flat view). key: what's there (a face, a flange, a screw, the touch
// panel's frame), to tell an edge, when only that's wanted.
float4 HoodSample(float2 cpx, bool perspective, bool shade, out float key)
{
    float2 f = (cpx - origin.xy) / view.z;              // on the wall
    float3 P = perspective ? eye.xyz : float3(f, 1000);
    float3 r = perspective ? normalize(float3(f, 0) - eye.xyz) : float3(0, 0, -1);
    float3 N;
    float t = HoodHit(P, r, N);
    key = -1000;                                        // (nothing: below any face's key)
    if (t > 1e8) return 0;
    float3 X = P + r * t;
    key = dot(round(N * 2), float3(1, 5, 25)) + (X.y < hood.y - 1e-4 ? 100 : 0);
    if (N.y < -0.5)
    {
        for (int i = 0; i < (int)hoodFlange.w; ++i)
            if (length(X.xz - hoodScrews[i].xy) < hoodScrews[i].z) key += 200 + i;
        if (abs(abs(X.x) - (hoodFlange.x + hoodFlange.y)) < hoodFlange.z) key += 50;   // a flange's rolled edge, and its shadow
    }

    // The touch panel sits in the hood's opening (it's drawn there, not by us),
    // in a black surround (duller). With perspective, it's a sheet leaning with
    // the hood's front, a little in front of it, drawn over the hood afterward
    // (blending its edges), in its glossy frame; under it, dark (its edges
    // blend with that)
    float3 c = 0;
    if (shade) c = HoodShade(X, N, r);
    if (perspective && panelSheet.x > 0)
    {
        float s = hood.w, D = hood.z + panelSheet.w, k = sqrt(1 + s * s);
        float u = (D - s * hood.y - P.z + s * P.y) / (r.z - s * r.y);
        float3 Q = P + r * u;
        float2 q = float2(Q.x, (Q.y - panelSheet.y) * k);                // on the sheet: across, down from the panel's top
        float panelH = (panelSheet.z - panelSheet.y) * k, hw = panelSheet.x;
        float outer = min(hw + panelFrame.x - abs(q.x), min(q.y + panelFrame.y, panelH + panelFrame.z - q.y));
        float inner = max(abs(q.x) - hw, max(-q.y, q.y - panelH));
        float px = (eye.z - Q.z) / eye.z / view.z;       // a pixel, in inches, at Q
        float cover = u > 0 && u <= t + 1 ? saturate(outer / px + 0.5) : 0;   // (its outside edge blends)
        if (cover > 0)
        {
            key = 1000 + (inner <= 0 ? 1 : 0);
            float side = hw + panelFrame.x - panelFrameScrews.x;
            for (int i = 0; i < 3; ++i)
                if (length(float2(abs(q.x), q.y + panelFrame.y) - float2(side, panelFrameScrews[1 + i])) < panelFrameScrew.x) key += 10 + i;
            if (shade) c = lerp(c, inner <= 0 ? 0.1 * c : FrameShade(Q, q, r, outer, inner, panelH), cover);
        }
    }
    else if (panelRect.z > panelRect.x && all(cpx >= panelRect.xy - panelSurround.x) && all(cpx <= panelRect.zw + panelSurround.x))
    {
        if (all(cpx >= panelRect.xy) && all(cpx <= panelRect.zw)) { key = -2000; return 0; }
        c *= 0.1;
        key = 2000;
    }
    return float4(c, 1);
}

// Each pixel's view traced to the hood. Where its corners all see the same
// thing, it's shaded once; at an edge (of a flange, a screw, the hood itself),
// at 3 x 3 points across it and averaged, its outline blending with what's
// behind (so edges don't stair-step).
float4 PSHood(float4 pos : SV_Position) : SV_Target
{
    float2 cpx = float2(pos.x + sTarget.z, pos.y);      // in the canvas
    bool perspective = sTarget.w > 0.5;
    float k0, k1, k2, k3, k4;
    HoodSample(cpx + float2(-0.4, -0.4), perspective, false, k1);
    HoodSample(cpx + float2(0.4, -0.4), perspective, false, k2);
    HoodSample(cpx + float2(-0.4, 0.4), perspective, false, k3);
    HoodSample(cpx + float2(0.4, 0.4), perspective, false, k4);
    float4 c;
    if (k1 == k2 && k1 == k3 && k1 == k4)
    {
        if (k1 <= -1000) discard;                       // (nothing there)
        c = HoodSample(cpx, perspective, true, k0);
        if (c.a <= 0) discard;
    }
    else
    {
        c = 0;
        [loop] for (int i = 0; i < 9; ++i)
            c += HoodSample(cpx + (float2(i % 3, i / 3) - 1) / 3, perspective, true, k0);
        if (c.a <= 0) discard;
        c.rgb /= c.a;
        c.a /= 9;
    }
    float3 L = c.rgb * light.z;
    L = L < 0.8 ? L : 0.8 + 0.2 * (1 - exp(-(L - 0.8) / 0.2));
    return float4(LinearToSrgb(saturate(L)), c.a);
}
