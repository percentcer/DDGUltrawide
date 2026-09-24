#include "compositor.h"
#include "config.h"
#include "log.h"
#include "touch.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    template <typename T>
    void SafeRelease(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    PresentFn g_origPresent = nullptr;

    // ---- Output window (created and pumped on its own thread) ----
    std::atomic<HWND> g_outWnd{ nullptr };
    bool g_shown = false;
    HWND g_gameWnd = nullptr;   // the game's window, from its swap chain
    constexpr UINT WM_APP_ATTACH = WM_APP + 1;   // wParam = game window

    // ---- D3D objects (only touched on the game's presenting thread) ----
    ID3D11Device* g_dev = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;
    IDXGISwapChain* g_outChain = nullptr;
    ID3D11RenderTargetView* g_outRTV = nullptr;
    ID3D11Texture2D* g_srcTex = nullptr;
    ID3D11ShaderResourceView* g_srcSRV = nullptr;
    bool g_srcMips = false;
    UINT g_srcW = 0, g_srcH = 0;
    DXGI_FORMAT g_srcFmt = DXGI_FORMAT_UNKNOWN;
    ID3D11VertexShader* g_vs = nullptr;
    ID3D11PixelShader* g_ps = nullptr;
    ID3D11Buffer* g_cb = nullptr;
    ID3D11SamplerState* g_samp = nullptr;
    ID3D11BlendState* g_blend = nullptr;
    ID3D11RasterizerState* g_rs = nullptr;
    ID3D11DepthStencilState* g_dss = nullptr;
    bool g_failed = false;
    bool g_loggedFirst = false;

    // Per-draw constants: where to draw (fractions of the output) and what to sample
    // (UVs of the game image), plus a clamp rect so sampling never bleeds into a
    // neighboring quadrant.
    struct DrawConstants
    {
        float dst[4];    // x0, y0, x1, y1 in output fractions (y down)
        float src[4];    // u0, v0, u1, v1
        float clamp[4];  // u0, v0, u1, v1
    };

    const char* kShader = R"(
cbuffer CB : register(b0)
{
    float4 dstRect;
    float4 srcRect;
    float4 clampRect;
};
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID)
{
    float2 t = float2(id & 1, id >> 1);
    float2 p = lerp(dstRect.xy, dstRect.zw, t);
    VSOut o;
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = lerp(srcRect.xy, srcRect.zw, t);
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = clamp(i.uv, clampRect.xy, clampRect.zw);
    return float4(tex.Sample(smp, uv).rgb, 1);
}
)";

    // ---------------------------------------------------------------------
    // Output window
    // ---------------------------------------------------------------------
    LRESULT CALLBACK OutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        LRESULT touchResult = 0;
        if (TouchHandleMessage(hwnd, msg, wp, lp, touchResult)) return touchResult;

        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;   // never take focus from the game
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            return 0;               // only closes with the game
        case WM_APP_ATTACH:
        {
            HWND game = reinterpret_cast<HWND>(wp);
            TouchSetGameWindow(game);
            if (g_cfg.gameWindowMode == 0)
            {
                // Owned windows always stay above their owner in the z-order.
                SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(game));
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                LOG("Output window now owned by the game window %p", game);
            }
            else
            {
                // Move the game window completely off the desktop.
                SetWindowPos(game, nullptr, -32000, -32000, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                LOG("Moved the game window %p off-screen", game);
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    DWORD WINAPI WindowThread(LPVOID)
    {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OutWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))  /* IDC_ARROW */;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"DDGUltrawideOutput";
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                    wc.lpszClassName, L"DDGUltrawide", WS_POPUP,
                                    g_cfg.outX, g_cfg.outY, g_cfg.outW, g_cfg.outH,
                                    nullptr, nullptr, inst, nullptr);
        if (!hwnd)
        {
            LOG("Could not create the output window (error %lu)", GetLastError());
            return 0;
        }
        LOG("Output window %dx%d at %d,%d", g_cfg.outW, g_cfg.outH, g_cfg.outX, g_cfg.outY);
        TouchSetOutputWindow(hwnd);
        g_outWnd = hwnd;   // shown after the first composited frame

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }

    // ---------------------------------------------------------------------
    // D3D setup
    // ---------------------------------------------------------------------
    DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
        }
    }

    bool InitPipeline()
    {
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* err = nullptr;
        const size_t len = strlen(kShader);

        if (FAILED(D3DCompile(kShader, len, "compositor", nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &err)))
        {
            LOG("VS compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            SafeRelease(err);
            return false;
        }
        if (FAILED(D3DCompile(kShader, len, "compositor", nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &err)))
        {
            LOG("PS compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            SafeRelease(err);
            SafeRelease(vsBlob);
            return false;
        }

        bool ok = SUCCEEDED(g_dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs))
               && SUCCEEDED(g_dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps));
        SafeRelease(vsBlob);
        SafeRelease(psBlob);
        if (!ok) { LOG("Shader creation failed"); return false; }

        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(DrawConstants);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;

        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = FALSE;
        dd.StencilEnable = FALSE;

        ok = SUCCEEDED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb))
          && SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_samp))
          && SUCCEEDED(g_dev->CreateBlendState(&bd, &g_blend))
          && SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_rs))
          && SUCCEEDED(g_dev->CreateDepthStencilState(&dd, &g_dss));
        if (!ok) LOG("Pipeline state creation failed");
        return ok;
    }

    // Our copy of the game's frame, with a mip chain for good-quality downscaling.
    bool EnsureSource(const D3D11_TEXTURE2D_DESC& bb)
    {
        if (g_srcTex && g_srcW == bb.Width && g_srcH == bb.Height && g_srcFmt == bb.Format) return true;
        SafeRelease(g_srcSRV);
        SafeRelease(g_srcTex);

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = bb.Width;
        td.Height = bb.Height;
        td.ArraySize = 1;
        td.Format = bb.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;

        // Preferred: full mip chain, generated on the GPU each frame
        td.MipLevels = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        g_srcMips = SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_srcTex));
        if (!g_srcMips)
        {
            td.MipLevels = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = 0;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_srcTex)))
            {
                LOG("Could not create the source texture (format %d)", static_cast<int>(bb.Format));
                return false;
            }
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = ViewFormat(bb.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = static_cast<UINT>(-1);
        if (FAILED(g_dev->CreateShaderResourceView(g_srcTex, &sv, &g_srcSRV)))
        {
            LOG("Could not create the source view");
            SafeRelease(g_srcTex);
            return false;
        }

        g_srcW = bb.Width;
        g_srcH = bb.Height;
        g_srcFmt = bb.Format;
        LOG("Game frame: %ux%u, format %d, samples %u, mips %s", bb.Width, bb.Height,
            static_cast<int>(bb.Format), bb.SampleDesc.Count, g_srcMips ? "yes" : "no");
        return true;
    }

    bool EnsureOutput()
    {
        if (g_outChain) return true;
        HWND hwnd = g_outWnd.load();
        if (!hwnd) return false;

        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory* factory = nullptr;
        bool ok = SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev)))
               && SUCCEEDED(dxgiDev->GetAdapter(&adapter))
               && SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)));
        if (ok)
        {
            DXGI_SWAP_CHAIN_DESC sd = {};
            sd.BufferDesc.Width = g_cfg.outW;
            sd.BufferDesc.Height = g_cfg.outH;
            sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.Windowed = TRUE;

            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (FAILED(factory->CreateSwapChain(g_dev, &sd, &g_outChain)))
            {
                sd.BufferCount = 1;
                sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                if (FAILED(factory->CreateSwapChain(g_dev, &sd, &g_outChain))) g_outChain = nullptr;
            }
            if (g_outChain)
            {
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                LOG("Output swap chain created (%s)", sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip" : "blit");
            }
        }
        SafeRelease(factory);
        SafeRelease(adapter);
        SafeRelease(dxgiDev);
        if (!g_outChain) { LOG("Could not create the output swap chain"); return false; }

        ID3D11Texture2D* buf = nullptr;
        if (FAILED(g_outChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buf)))
            || FAILED(g_dev->CreateRenderTargetView(buf, nullptr, &g_outRTV)))
        {
            SafeRelease(buf);
            LOG("Could not create the output render target");
            SafeRelease(g_outChain);
            return false;
        }
        SafeRelease(buf);
        return true;
    }

    // ---------------------------------------------------------------------
    // Saving and restoring the game's pipeline state around our drawing
    // ---------------------------------------------------------------------
    struct StateBackup
    {
        UINT numScissor, numVP;
        D3D11_RECT scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11RasterizerState* rs;
        ID3D11BlendState* blend;
        FLOAT blendFactor[4];
        UINT sampleMask;
        ID3D11DepthStencilState* dss;
        UINT stencilRef;
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        ID3D11DepthStencilView* dsv;
        ID3D11ShaderResourceView* psSRV;
        ID3D11SamplerState* psSampler;
        ID3D11PixelShader* ps;
        ID3D11VertexShader* vs;
        ID3D11GeometryShader* gs;
        ID3D11HullShader* hs;
        ID3D11DomainShader* ds;
        ID3D11ClassInstance* psInst[256];
        ID3D11ClassInstance* vsInst[256];
        ID3D11ClassInstance* gsInst[256];
        UINT psInstN, vsInstN, gsInstN;
        ID3D11Buffer* vsCB;
        ID3D11Buffer* psCB;
        D3D11_PRIMITIVE_TOPOLOGY topo;
        ID3D11Buffer* ib;
        DXGI_FORMAT ibFmt;
        UINT ibOff;
        ID3D11Buffer* vb;
        UINT vbStride, vbOff;
        ID3D11InputLayout* layout;
    };
    StateBackup g_backup;   // render thread only; too big for the stack

    void Backup(StateBackup& b)
    {
        b.numScissor = b.numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        g_ctx->RSGetScissorRects(&b.numScissor, b.scissor);
        g_ctx->RSGetViewports(&b.numVP, b.vp);
        g_ctx->RSGetState(&b.rs);
        g_ctx->OMGetBlendState(&b.blend, b.blendFactor, &b.sampleMask);
        g_ctx->OMGetDepthStencilState(&b.dss, &b.stencilRef);
        g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtv, &b.dsv);
        g_ctx->PSGetShaderResources(0, 1, &b.psSRV);
        g_ctx->PSGetSamplers(0, 1, &b.psSampler);
        b.psInstN = b.vsInstN = b.gsInstN = 256;
        g_ctx->PSGetShader(&b.ps, b.psInst, &b.psInstN);
        g_ctx->VSGetShader(&b.vs, b.vsInst, &b.vsInstN);
        g_ctx->GSGetShader(&b.gs, b.gsInst, &b.gsInstN);
        g_ctx->HSGetShader(&b.hs, nullptr, nullptr);
        g_ctx->DSGetShader(&b.ds, nullptr, nullptr);
        g_ctx->VSGetConstantBuffers(0, 1, &b.vsCB);
        g_ctx->PSGetConstantBuffers(0, 1, &b.psCB);
        g_ctx->IAGetPrimitiveTopology(&b.topo);
        g_ctx->IAGetIndexBuffer(&b.ib, &b.ibFmt, &b.ibOff);
        g_ctx->IAGetVertexBuffers(0, 1, &b.vb, &b.vbStride, &b.vbOff);
        g_ctx->IAGetInputLayout(&b.layout);
    }

    void Restore(StateBackup& b)
    {
        g_ctx->RSSetScissorRects(b.numScissor, b.scissor);
        g_ctx->RSSetViewports(b.numVP, b.vp);
        g_ctx->RSSetState(b.rs); SafeRelease(b.rs);
        g_ctx->OMSetBlendState(b.blend, b.blendFactor, b.sampleMask); SafeRelease(b.blend);
        g_ctx->OMSetDepthStencilState(b.dss, b.stencilRef); SafeRelease(b.dss);
        g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtv, b.dsv);
        for (auto*& r : b.rtv) SafeRelease(r);
        SafeRelease(b.dsv);
        g_ctx->PSSetShaderResources(0, 1, &b.psSRV); SafeRelease(b.psSRV);
        g_ctx->PSSetSamplers(0, 1, &b.psSampler); SafeRelease(b.psSampler);
        g_ctx->PSSetShader(b.ps, b.psInst, b.psInstN); SafeRelease(b.ps);
        for (UINT i = 0; i < b.psInstN; ++i) SafeRelease(b.psInst[i]);
        g_ctx->VSSetShader(b.vs, b.vsInst, b.vsInstN); SafeRelease(b.vs);
        for (UINT i = 0; i < b.vsInstN; ++i) SafeRelease(b.vsInst[i]);
        g_ctx->GSSetShader(b.gs, b.gsInst, b.gsInstN); SafeRelease(b.gs);
        for (UINT i = 0; i < b.gsInstN; ++i) SafeRelease(b.gsInst[i]);
        g_ctx->HSSetShader(b.hs, nullptr, 0); SafeRelease(b.hs);
        g_ctx->DSSetShader(b.ds, nullptr, 0); SafeRelease(b.ds);
        g_ctx->VSSetConstantBuffers(0, 1, &b.vsCB); SafeRelease(b.vsCB);
        g_ctx->PSSetConstantBuffers(0, 1, &b.psCB); SafeRelease(b.psCB);
        g_ctx->IASetPrimitiveTopology(b.topo);
        g_ctx->IASetIndexBuffer(b.ib, b.ibFmt, b.ibOff); SafeRelease(b.ib);
        g_ctx->IASetVertexBuffers(0, 1, &b.vb, &b.vbStride, &b.vbOff); SafeRelease(b.vb);
        g_ctx->IASetInputLayout(b.layout); SafeRelease(b.layout);
    }

    // ---------------------------------------------------------------------
    // Per-frame compositing
    // ---------------------------------------------------------------------
    void DrawRegions()
    {
        const std::vector<Rect> dst = SnappedDests();
        const size_t n = g_cfg.ScreenCount();

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(g_cfg.outW);
        vp.Height = static_cast<float>(g_cfg.outH);
        vp.MaxDepth = 1.0f;

        const float black[4] = { 0, 0, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &g_outRTV, nullptr);
        g_ctx->ClearRenderTargetView(g_outRTV, black);
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(g_rs);
        g_ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(g_dss, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_ps, nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->HSSetShader(nullptr, nullptr, 0);
        g_ctx->DSSetShader(nullptr, nullptr, 0);
        g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->PSSetShaderResources(0, 1, &g_srcSRV);
        g_ctx->PSSetSamplers(0, 1, &g_samp);

        const float tu = 1.0f / g_srcW, tv = 1.0f / g_srcH;
        for (size_t i = 0; i < n; ++i)
        {
            const Rect& d = dst[i];
            const Rect& s = g_cfg.sources[i];
            DrawConstants c;
            c.dst[0] = d.originX; c.dst[1] = d.originY;
            c.dst[2] = d.originX + d.sizeX; c.dst[3] = d.originY + d.sizeY;
            c.src[0] = s.originX; c.src[1] = s.originY;
            c.src[2] = s.originX + s.sizeX; c.src[3] = s.originY + s.sizeY;
            // Keep the sample footprint inside the region
            c.clamp[0] = c.src[0] + tu; c.clamp[1] = c.src[1] + tv;
            c.clamp[2] = c.src[2] - tu; c.clamp[3] = c.src[3] - tv;

            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
            memcpy(m.pData, &c, sizeof(c));
            g_ctx->Unmap(g_cb, 0);
            g_ctx->Draw(4, 0);
        }

        // Don't leave our view bound where the game might try to write to the texture
        ID3D11ShaderResourceView* nullSRV = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    void Composite(IDXGISwapChain* gameChain)
    {
        if (g_failed || !g_outWnd.load()) return;

        if (!g_dev)
        {
            if (FAILED(gameChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))))
            {
                LOG("The game's swap chain isn't Direct3D 11; compositor disabled");
                g_failed = true;
                return;
            }
            g_dev->GetImmediateContext(&g_ctx);
            if (!InitPipeline()) { g_failed = true; return; }
        }
        if (!EnsureOutput()) { g_failed = true; return; }

        // Tie our window to the game's window (once per game window)
        DXGI_SWAP_CHAIN_DESC scd;
        if (SUCCEEDED(gameChain->GetDesc(&scd)) && scd.OutputWindow && scd.OutputWindow != g_gameWnd)
        {
            g_gameWnd = scd.OutputWindow;
            PostMessageW(g_outWnd.load(), WM_APP_ATTACH, reinterpret_cast<WPARAM>(g_gameWnd), 0);
        }

        ID3D11Texture2D* bb = nullptr;
        if (FAILED(gameChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
        D3D11_TEXTURE2D_DESC desc;
        bb->GetDesc(&desc);
        if (!EnsureSource(desc)) { SafeRelease(bb); g_failed = true; return; }

        if (desc.SampleDesc.Count > 1)
            g_ctx->ResolveSubresource(g_srcTex, 0, bb, 0, ViewFormat(desc.Format));
        else
            g_ctx->CopySubresourceRegion(g_srcTex, 0, 0, 0, 0, bb, 0, nullptr);
        SafeRelease(bb);
        if (g_srcMips) g_ctx->GenerateMips(g_srcSRV);

        Backup(g_backup);
        DrawRegions();
        Restore(g_backup);

        g_outChain->Present(g_cfg.vsync ? 1 : 0, 0);

        if (!g_shown)
        {
            g_shown = true;
            ShowWindowAsync(g_outWnd.load(), SW_SHOWNOACTIVATE);
        }
        if (!g_loggedFirst)
        {
            g_loggedFirst = true;
            LOG("First frame composited");
        }
    }

    bool SafeComposite(IDXGISwapChain* chain)
    {
        __try
        {
            Composite(chain);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* chain, UINT sync, UINT flags)
    {
        // Our own Present comes through here too; only composite the game's frames.
        if (chain != g_outChain && !(flags & DXGI_PRESENT_TEST) && !g_failed)
        {
            if (!SafeComposite(chain))
            {
                g_failed = true;
                LOG("Fault while compositing; compositor disabled");
            }
        }
        return g_origPresent(chain, sync, flags);
    }

    // Finds IDXGISwapChain::Present by creating a throwaway device and swap chain.
    void* FindPresent()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DDGUltrawideDummy";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return nullptr;

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = 64;
        sd.BufferDesc.Height = 64;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        IDXGISwapChain* chain = nullptr;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        void* present = nullptr;
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                    D3D11_SDK_VERSION, &sd, &chain, &dev, nullptr, &ctx)))
        {
            void** vtable = *reinterpret_cast<void***>(chain);
            present = vtable[8];   // IDXGISwapChain::Present
        }
        SafeRelease(chain);
        SafeRelease(ctx);
        SafeRelease(dev);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return present;
    }
}

bool InstallCompositor()
{
    HANDLE t = CreateThread(nullptr, 0, WindowThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);

    void* present = FindPresent();
    if (!present)
    {
        LOG("Could not locate IDXGISwapChain::Present; compositor disabled");
        return false;
    }
    if (MH_CreateHook(present, reinterpret_cast<void*>(&Hook_Present), reinterpret_cast<void**>(&g_origPresent)) != MH_OK
        || MH_EnableHook(present) != MH_OK)
    {
        LOG("Could not hook Present; compositor disabled");
        return false;
    }
    LOG("Compositor installed (Present at %p), %zu screens", present, g_cfg.ScreenCount());
    return true;
}
