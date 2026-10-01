#include "cabinetlight.h"
#include "config.h"
#include "log.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

// The shaders (shaders\cabinetlight.hlsl), compiled with the DLL (shaders\compile.cmd)
#include "cabinetlight_VSFull.h"
#include "cabinetlight_VSShape.h"
#include "cabinetlight_PSShape.h"
#include "cabinetlight_PSCells.h"
#include "cabinetlight_PSPatches.h"
#include "cabinetlight_PSBounce.h"
#include "cabinetlight_PSCube.h"
#include "cabinetlight_PSGloss.h"
#include "cabinetlight_PSShade.h"
#include "cabinetlight_PSBoothMap.h"
#include "cabinetlight_PSAcrylic.h"
#include "cabinetlight_PSHood.h"
#include "cabinetlight_PSChrome.h"
#include "cabinetlight_PSAO.h"
#include "cabinetlight_PSScrew.h"

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
//   0. (Once, with the G-buffer) contact shadows: how much of the sky above
//      each point of the cabinet nearby raised parts (frames, brackets, screws)
//      block, from its heights. It darkens the light the point gets.
//   (The hood also shadows the cabinet from each light, in 3 and 4.)
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
    constexpr int kMaxPatches = 176;
    constexpr int kMaxHoodScrews = 16;
    constexpr int kCubeScale = 8;           // ambient cube resolution: 1 texel per 8x8 pixels
    // Light levels are physical, in units of the screens' white: a screen emits
    // exactly what it shows (1 = white = ArcadeCabinetScreenNits), and the output
    // shows the screens' white as display white, so the pictures look as drawn.
    // The arcade's own lighting (ArcadeCabinetRoomLux) is converted to match.


    struct ShapeConstants
    {
        float rect[4], params[4], albedo[4], mat[4], target[4];
    };

    struct SceneConstants
    {
        float cellPos[kMaxCells][4], cellNrm[kMaxCells][4], cellUV[kMaxCells][4];
        float patchPos[kMaxPatches][4], patchNrm[kMaxPatches][4], patchAlb[kMaxPatches][4];
        float counts[4], view[4], origin[4], light[4], wall[4], geom[4], booth[4], booth2[4], eye[4];
        float hood[4], hood2[4], hoodGrille[4], grilleCells[4], hoodOpening[4], panelRect[4], panelSurround[4], panelSheet[4];
        float panelFrame[4], panelFrameScrews[4], panelFrameScrew[4], backWindows[3][4], sideWindow[4], roofLeds[4], roofLed[4];
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
    ID3D11PixelShader* g_psAO = nullptr;
    ID3D11PixelShader* g_psScrew = nullptr;
    ID3D11Buffer* g_shapeCB = nullptr;
    ID3D11Buffer* g_sceneCB = nullptr;
    ID3D11BlendState* g_blendCover = nullptr;
    ID3D11BlendState* g_blendOff = nullptr;
    ID3D11RasterizerState* g_rs = nullptr;
    ID3D11DepthStencilState* g_dss = nullptr;
    ID3D11SamplerState* g_sampler = nullptr;

    Target g_gbuffer[3], g_cells, g_patches, g_patches2, g_cube[6], g_gloss[5];
    Target g_ao;                            // 0. contact shadows
    Target g_boothMap;                      // 6. the booth's surfaces
    Target g_under;                         // 7. a copy of what's under the acrylic
    Target g_reflections;                   // 7. the acrylic's reflections alone
    bool g_sheetsDrawn = false;             // ...if the sheets were drawn
    constexpr int kMap = 32, kMapFaces = 7; // the booth map (as in the shader)
    std::vector<CabinetAcrylic> g_acrylics;
    std::vector<CabinetAcrylic> g_screws;   // the screw heads (their bounds; z unused)
    struct ScrewHead { float x, y, r, z, height, albedo[3]; };   // canvas px, px; inches; linear
    std::vector<ScrewHead> g_heads;         // ...and as they are, for drawing in perspective
    float g_originX = 0, g_originY = 0, g_pxPerInch = 1, g_cornerIn = 0, g_faceAngle = 0, g_cameraIn = 0;
    float g_hoodTopPx = 0;                  // the hood's top where it meets the wall, in the canvas
    constexpr int kBounces = 2;             // patch-to-patch bounces after the direct light
    int g_outW = 0, g_w = 0, g_h = 0, g_offset = 0;
    bool g_built = false;       // for these sizes
    bool g_enabled = false;     // the cabinet is drawn at this size
    int g_cellCount = 0, g_patchCount = 0;

    // The shaders' bytecode, by entry point
    struct Bytecode { const char* entry; const BYTE* data; size_t size; };
    const Bytecode kBytecode[] = {
        { "VSFull", g_cabinetlight_VSFull, sizeof(g_cabinetlight_VSFull) },
        { "VSShape", g_cabinetlight_VSShape, sizeof(g_cabinetlight_VSShape) },
        { "PSShape", g_cabinetlight_PSShape, sizeof(g_cabinetlight_PSShape) },
        { "PSCells", g_cabinetlight_PSCells, sizeof(g_cabinetlight_PSCells) },
        { "PSPatches", g_cabinetlight_PSPatches, sizeof(g_cabinetlight_PSPatches) },
        { "PSBounce", g_cabinetlight_PSBounce, sizeof(g_cabinetlight_PSBounce) },
        { "PSCube", g_cabinetlight_PSCube, sizeof(g_cabinetlight_PSCube) },
        { "PSGloss", g_cabinetlight_PSGloss, sizeof(g_cabinetlight_PSGloss) },
        { "PSShade", g_cabinetlight_PSShade, sizeof(g_cabinetlight_PSShade) },
        { "PSBoothMap", g_cabinetlight_PSBoothMap, sizeof(g_cabinetlight_PSBoothMap) },
        { "PSAcrylic", g_cabinetlight_PSAcrylic, sizeof(g_cabinetlight_PSAcrylic) },
        { "PSHood", g_cabinetlight_PSHood, sizeof(g_cabinetlight_PSHood) },
        { "PSChrome", g_cabinetlight_PSChrome, sizeof(g_cabinetlight_PSChrome) },
        { "PSAO", g_cabinetlight_PSAO, sizeof(g_cabinetlight_PSAO) },
        { "PSScrew", g_cabinetlight_PSScrew, sizeof(g_cabinetlight_PSScrew) },
    };

    const Bytecode* FindBytecode(const char* entry)
    {
        for (const Bytecode& b : kBytecode)
            if (strcmp(b.entry, entry) == 0) return &b;
        LOG("Cabinet shader %s is missing", entry);
        return nullptr;
    }

    bool CreateVS(const char* entry, ID3D11VertexShader** vs)
    {
        const Bytecode* b = FindBytecode(entry);
        return b && SUCCEEDED(g_dev->CreateVertexShader(b->data, b->size, nullptr, vs));
    }

    bool CreatePS(const char* entry, ID3D11PixelShader** ps)
    {
        const Bytecode* b = FindBytecode(entry);
        return b && SUCCEEDED(g_dev->CreatePixelShader(b->data, b->size, nullptr, ps));
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
        ID3D11ShaderResourceView* none[17] = {};
        ctx->PSSetShaderResources(0, 17, none);
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
        g_ao.Release();
        g_cells.Release();
        g_patches.Release();
        g_patches2.Release();
        g_boothMap.Release();
        g_acrylics.clear();
        g_screws.clear();
        g_heads.clear();

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
            float* a = sc->patchAlb[i]; a[0] = c.albedo[0]; a[1] = c.albedo[1]; a[2] = c.albedo[2]; a[3] = c.emission;
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
        for (int i = 0; i < 4; ++i) sc->grilleCells[i] = scene.grilleCells[i];
        for (int i = 0; i < 3; ++i) sc->hoodOpening[i] = scene.hoodOpening[i];
        sc->hoodOpening[3] = scene.hoodCorner;
        if (scene.panelRect[2] > scene.panelRect[0])
        {
            sc->panelRect[0] = scene.panelRect[0] + offset; sc->panelRect[1] = scene.panelRect[1];
            sc->panelRect[2] = scene.panelRect[2] + offset; sc->panelRect[3] = scene.panelRect[3];
        }
        sc->panelSurround[0] = scene.panelSurround;
        for (int i = 0; i < 3; ++i) sc->panelSheet[i] = scene.panelSheet[i];
        sc->panelSheet[3] = 10.0f / 25.4f;                  // (as kPanelForwardMm)
        for (int i = 0; i < 4; ++i) sc->panelFrame[i] = scene.panelFrame[i];
        for (int i = 0; i < 4; ++i) sc->panelFrameScrews[i] = scene.panelFrameScrews[i];
        sc->panelFrameScrew[0] = scene.panelFrameScrews[4];
        memcpy(sc->backWindows, scene.backWindows, sizeof(sc->backWindows));
        memcpy(sc->sideWindow, scene.sideWindow, sizeof(sc->sideWindow));
        memcpy(sc->roofLeds, scene.roofLeds, sizeof(sc->roofLeds));
        sc->roofLed[0] = scene.roofLedRadiance;
        sc->roofLed[1] = g_cfg.roofLightGlowMm / 25.4f;
        sc->roofLed[2] = g_cfg.hoodGlossDeg * 3.14159265f / 180;
        for (int i = 0; i < 3; ++i) sc->hoodFlange[i] = scene.hoodFlange[i];
        const int screws = static_cast<int>(scene.hoodScrews.size() < kMaxHoodScrews ? scene.hoodScrews.size() : kMaxHoodScrews);
        sc->hoodFlange[3] = static_cast<float>(screws);
        for (int i = 0; i < screws; ++i)
            for (int k = 0; k < 3; ++k) sc->hoodScrews[i][k] = scene.hoodScrews[i][k];
        g_hoodTopPx = scene.originY + scene.hoodTop * scene.pxPerInch;
        g_acrylics = scene.acrylics;
        for (const CabinetShape& sh : scene.shapes)
            if (sh.kind == CabinetShape::Dome)
            {
                g_screws.push_back({ sh.x0 - sh.x1 - 1, sh.y0 - sh.x1 - 1, sh.x0 + sh.x1 + 1, sh.y0 + sh.x1 + 1, 0 });
                g_heads.push_back({ sh.x0, sh.y0, sh.x1, sh.z0, sh.edge,
                                    { ToLinear(sh.albedo[0]), ToLinear(sh.albedo[1]), ToLinear(sh.albedo[2]) } });
            }
        g_originX = scene.originX; g_originY = scene.originY; g_pxPerInch = scene.pxPerInch;
        g_cornerIn = scene.cornerIn; g_faceAngle = scene.faceAngle; g_cameraIn = scene.eye[2];
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
        // Contact shadows, from it
        if (g_psAO && g_ao.Create(g_dev, w, h, DXGI_FORMAT_R8_UNORM))
        {
            UnbindSRVs(ctx);
            ctx->OMSetRenderTargets(1, &g_ao.rtv, nullptr);
            ctx->OMSetBlendState(g_blendOff, nullptr, 0xFFFFFFFF);
            ctx->VSSetShader(g_vsFull, nullptr, 0);
            ctx->PSSetShader(g_psAO, nullptr, 0);
            ctx->PSSetConstantBuffers(1, 1, &g_sceneCB);
            ctx->PSSetShaderResources(1, 1, &g_gbuffer[1].srv);
            ctx->Draw(4, 0);
            UnbindSRVs(ctx);
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
        && CreatePS("PSBoothMap", &g_psBoothMap) && CreatePS("PSAcrylic", &g_psAcrylic) && CreatePS("PSHood", &g_psHood) && CreatePS("PSChrome", &g_psChrome) && CreatePS("PSAO", &g_psAO) && CreatePS("PSScrew", &g_psScrew)
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
    ctx->PSSetShaderResources(16, 1, &g_ao.srv);
    ctx->Draw(4, 0);
    UnbindSRVs(ctx);
    return true;
}

bool CabinetLightReflect(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, bool sheets)
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
    D3D11_TEXTURE2D_DESC rd = {};
    if (g_reflections.tex) g_reflections.tex->GetDesc(&rd);
    if ((!g_reflections.tex || rd.Width != td.Width || rd.Height != td.Height)
        && !g_reflections.Create(g_dev, g_w, g_h, DXGI_FORMAT_R16G16B16A16_FLOAT))
        g_reflections.Release();
    ctx->CopySubresourceRegion(g_under.tex, 0, 0, 0, 0, tex, 0, nullptr);
    SafeRelease(tex);
    ctx->GenerateMips(g_under.srv);

    CommonState(ctx);
    UnbindSRVs(ctx);
    ctx->OMSetBlendState(g_blendOff, nullptr, 0xFFFFFFFF);
    const float none[4] = { 0, 0, 0, 0 };
    if (g_reflections.rtv) ctx->ClearRenderTargetView(g_reflections.rtv, none);
    ID3D11RenderTargetView* targets[2] = { target, g_reflections.rtv };
    ctx->OMSetRenderTargets(g_reflections.rtv ? 2 : 1, targets, nullptr);
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
    if (sheets)
        for (const CabinetAcrylic& a : g_acrylics) draw(a);
    g_sheetsDrawn = sheets;

    // The chrome screw heads, over it, blended in by how much of each pixel they
    // cover. Only in the flat view: with perspective (where the sheets are
    // drawn), CabinetLightScrews draws them over the warped view instead, and
    // they'd be covered.
    if (!sheets)
    {
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->PSSetShader(g_psChrome, nullptr, 0);
        ctx->OMSetBlendState(g_blendCover, nullptr, 0xFFFFFFFF);
        for (const CabinetAcrylic& a : g_screws) draw(a);
    }
    UnbindSRVs(ctx);
    return true;
}

bool CabinetLightHood(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, int width, int height,
                      int offset, bool perspective)
{
    if (!g_psHood || !g_enabled || !target || !g_under.srv) return false;
    CommonState(ctx);
    UnbindSRVs(ctx);
    ctx->OMSetBlendState(g_blendCover, nullptr, 0xFFFFFFFF);   // (its outline blends by coverage)
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

ID3D11ShaderResourceView* CabinetLightReflections()
{
    return g_enabled && g_sheetsDrawn && !g_acrylics.empty() ? g_reflections.srv : nullptr;
}

bool CabinetLightScrews(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, int width, int height, int offset)
{
    if (!g_psScrew || !g_enabled || !target || !g_under.srv || g_heads.empty() || g_cameraIn <= 0) return false;
    CommonState(ctx);
    UnbindSRVs(ctx);
    ctx->OMSetBlendState(g_blendCover, nullptr, 0xFFFFFFFF);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    Viewport(ctx, width, height);
    ctx->VSSetShader(g_vsShape, nullptr, 0);
    ctx->PSSetShader(g_psScrew, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_shapeCB);
    ctx->PSSetConstantBuffers(0, 1, &g_shapeCB);
    ctx->PSSetConstantBuffers(1, 1, &g_sceneCB);
    ID3D11ShaderResourceView* in[2] = { g_under.srv, g_boothMap.srv };
    ctx->PSSetShaderResources(0, 2, in);
    const float sa = std::sin(g_faceAngle), ca = std::cos(g_faceAngle), Z = g_cameraIn;
    for (const ScrewHead& hd : g_heads)
    {
        // Where it is in 3D (as the shader's Place), and where that shows
        const float fx = (hd.x - g_originX) / g_pxPerInch, fy = (hd.y - g_originY) / g_pxPerInch;
        float qx = fx, qz = 0;                      // (its base on the face's surface, as in the shader)
        const float along = std::fabs(fx) - g_cornerIn;
        if (along > 0)
        {
            const float sgn = fx < 0 ? -1.0f : 1.0f;
            qx = sgn * g_cornerIn + sgn * ca * along;
            qz = sa * along;
        }
        const float k = Z / std::fmax(Z - qz, 1.0f);
        const float cx = g_originX - offset + qx * k * g_pxPerInch, cy = g_originY + fy * k * g_pxPerInch;
        const float r = hd.r * k * 1.3f + 2;
        const ShapeConstants c = {
            { std::floor(cx - r), std::floor(cy - r), std::ceil(cx + r), std::ceil(cy + r) },
            { hd.x, hd.y, hd.r, hd.z }, { hd.albedo[0], hd.albedo[1], hd.albedo[2], hd.height }, { 0, 0, 0, 0 },
            { static_cast<float>(width), static_cast<float>(height), static_cast<float>(offset), 0 } };
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(g_shapeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(g_shapeCB, 0);
        ctx->Draw(4, 0);
    }
    UnbindSRVs(ctx);
    return true;
}
