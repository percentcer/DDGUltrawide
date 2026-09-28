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
    constexpr int kCubeScale = 8;           // ambient cube resolution: 1 texel per 8x8 pixels
    // Light levels are physical, in units of the screens' white: a screen emits
    // exactly what it shows (1 = white = ArcadeCabinetScreenNits), and the output
    // shows the screens' white as display white, so the pictures look as drawn.
    // The arcade's own lighting (ArcadeCabinetRoomLux) is converted to match.

    const char* kShader = R"(
#define MAX_CELLS 128
#define MAX_PATCHES 160
static const float PI = 3.14159265;

cbuffer Shape : register(b0)
{
    float4 sRect;       // left, top, right, bottom (px). Dome: center x, y, radius
    float4 sParams;     // z0, z1, edge, kind (0 box, 1 ramp, 2 dome)
    float4 sAlbedo;     // linear rgb, ramp axis
    float4 sMat;        // roughness, metalness, pixels per inch, powder coat
    float4 sTarget;     // target width, height
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
    if (sParams.w > 1.5)
    {
        // Dome: a low, rounded fastener head
        float2 d = (p - sRect.xy) / sRect.z;
        float l = length(d);
        cover = saturate((1 - l) * sRect.z + 0.5);
        float h = sqrt(saturate(1 - l * l));
        n = normalize(float3(d, h * 1.3));
        z += sParams.z * h;
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
    o.albedo = float4(sAlbedo.rgb, cover);
    o.geo = float4(n.xy * 0.5 + 0.5, saturate(z / 16), cover);
    o.mat = float4(sMat.x, sMat.y, sMat.w, cover);
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
)";

    struct ShapeConstants
    {
        float rect[4], params[4], albedo[4], mat[4], target[4];
    };

    struct SceneConstants
    {
        float cellPos[kMaxCells][4], cellNrm[kMaxCells][4], cellUV[kMaxCells][4];
        float patchPos[kMaxPatches][4], patchNrm[kMaxPatches][4], patchAlb[kMaxPatches][4];
        float counts[4], view[4], origin[4], light[4], wall[4], geom[4];
    };

    struct Target
    {
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;

        bool Create(ID3D11Device* dev, int w, int h, DXGI_FORMAT format)
        {
            Release();
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
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
    ID3D11Buffer* g_shapeCB = nullptr;
    ID3D11Buffer* g_sceneCB = nullptr;
    ID3D11BlendState* g_blendCover = nullptr;
    ID3D11BlendState* g_blendOff = nullptr;
    ID3D11RasterizerState* g_rs = nullptr;
    ID3D11DepthStencilState* g_dss = nullptr;
    ID3D11SamplerState* g_sampler = nullptr;

    Target g_gbuffer[3], g_cells, g_patches, g_patches2, g_cube[6], g_gloss[5];
    constexpr int kBounces = 2;             // patch-to-patch bounces after the direct light
    int g_w = 0, g_h = 0;
    bool g_built = false;       // for g_w x g_h
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
        ID3D11ShaderResourceView* none[14] = {};
        ctx->PSSetShaderResources(0, 14, none);
    }

    // Builds everything that only depends on the window size: the G-buffer, the
    // scene constants, and the per-frame targets.
    void Build(ID3D11DeviceContext* ctx, int w, int h)
    {
        g_built = true;
        g_enabled = false;
        g_w = w;
        g_h = h;
        for (Target& t : g_gbuffer) t.Release();
        for (Target& t : g_cube) t.Release();
        for (Target& t : g_gloss) t.Release();
        g_cells.Release();
        g_patches.Release();
        g_patches2.Release();

        CabinetScene scene;
        if (!GetCabinetScene(w, h, scene)) return;
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
                && g_patches2.Create(g_dev, g_patchCount > 0 ? g_patchCount : 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
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

bool CabinetLightRender(ID3D11DeviceContext* ctx, int width, int height,
                        ID3D11ShaderResourceView* frame, ID3D11RenderTargetView* target)
{
    if (!g_psShade || !frame || !target) return false;
    if (!g_built || width != g_w || height != g_h) Build(ctx, width, height);
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
