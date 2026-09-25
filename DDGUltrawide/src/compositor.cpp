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
#include <cwchar>
#include <vector>

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

    // ---- Output windows: the main output, plus the optional touch panel window ----
    struct Output
    {
        const char* name = "";
        int x = 0, y = 0, w = 0, h = 0;
        bool vsync = true;
        std::atomic<HWND> hwnd{ nullptr };   // created and pumped on the window thread
        std::atomic<bool> ready{ false };    // window thread finished (hwnd may still be null)

        // Only touched on the game's presenting thread
        IDXGISwapChain* chain = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        bool shown = false;
        bool failed = false;
    };
    Output g_outs[kWindowCount];

    HWND g_gameWnd = nullptr;   // the game's window, from its swap chain
    constexpr UINT WM_APP_ATTACH = WM_APP + 1;   // wParam = game window
    bool g_loggedSizeMismatch = false;

    // ---- D3D objects (only touched on the game's presenting thread) ----
    ID3D11Device* g_dev = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;
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
    // Output windows
    // ---------------------------------------------------------------------
    const char* OutputName(HWND hwnd)
    {
        for (const Output& o : g_outs)
            if (o.hwnd.load() == hwnd) return o.name;
        return "Output";
    }

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
                LOG("%s window now owned by the game window %p", OutputName(hwnd), game);
            }
            else if (hwnd == g_outs[kMainWindow].hwnd.load())
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

    BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp)
    {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi))
            reinterpret_cast<std::vector<MONITORINFOEXW>*>(lp)->push_back(mi);
        return TRUE;
    }

    // "\\.\DISPLAY2" -> 2, the number Windows shows in its display settings.
    int DisplayNumber(const wchar_t* device)
    {
        const wchar_t* p = wcsstr(device, L"DISPLAY");
        return p ? _wtoi(p + 7) : 0;
    }

    // Where the touch panel window goes: the explicit rect from the ini, or the
    // whole of the chosen monitor.
    bool ResolvePanelRect(RECT& r)
    {
        if (g_cfg.panelW > 0 && g_cfg.panelH > 0)
        {
            r = { g_cfg.panelX, g_cfg.panelY, g_cfg.panelX + g_cfg.panelW, g_cfg.panelY + g_cfg.panelH };
            return true;
        }

        std::vector<MONITORINFOEXW> mons;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&mons));
        for (const MONITORINFOEXW& m : mons)
        {
            const RECT& mr = m.rcMonitor;
            LOG("Monitor %d: %ldx%ld at %ld,%ld%s", DisplayNumber(m.szDevice), mr.right - mr.left,
                mr.bottom - mr.top, mr.left, mr.top, (m.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
        }

        const POINT mainCenter = { g_cfg.outX + g_cfg.outW / 2, g_cfg.outY + g_cfg.outH / 2 };
        for (const MONITORINFOEXW& m : mons)
        {
            const bool match = g_cfg.panelMonitor > 0
                ? DisplayNumber(m.szDevice) == g_cfg.panelMonitor
                : !PtInRect(&m.rcMonitor, mainCenter);
            if (match) { r = m.rcMonitor; return true; }
        }

        if (g_cfg.panelMonitor > 0)
            LOG("Monitor %d not found; touch panel window disabled", g_cfg.panelMonitor);
        else
            LOG("No monitor other than the main output's found; touch panel window disabled");
        return false;
    }

    HWND CreateOutputWindow(Output& o, const wchar_t* className, const wchar_t* title)
    {
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                    className, title, WS_POPUP, o.x, o.y, o.w, o.h,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd)
            LOG("%s window %dx%d at %d,%d", o.name, o.w, o.h, o.x, o.y);
        else
            LOG("Could not create the %s window (error %lu)", o.name, GetLastError());
        return hwnd;
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

        // Windows are shown after their first composited frame.
        Output& main = g_outs[kMainWindow];
        HWND hwnd = CreateOutputWindow(main, wc.lpszClassName, L"DDGUltrawide");
        TouchSetOutputWindow(kMainWindow, hwnd);
        main.hwnd = hwnd;
        main.ready = true;

        Output& panel = g_outs[kPanelWindow];
        RECT pr;
        if (g_cfg.panelWindow && ResolvePanelRect(pr))
        {
            panel.x = pr.left;
            panel.y = pr.top;
            panel.w = pr.right - pr.left;
            panel.h = pr.bottom - pr.top;
            HWND ph = CreateOutputWindow(panel, wc.lpszClassName, L"DDGUltrawide Touch Panel");
            TouchSetOutputWindow(kPanelWindow, ph);
            panel.hwnd = ph;
        }
        panel.ready = true;

        if (!hwnd && !panel.hwnd.load()) return 0;

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

    bool EnsureOutput(Output& o)
    {
        if (o.chain) return true;
        HWND hwnd = o.hwnd.load();
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
            sd.BufferDesc.Width = o.w;
            sd.BufferDesc.Height = o.h;
            sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.Windowed = TRUE;

            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (FAILED(factory->CreateSwapChain(g_dev, &sd, &o.chain)))
            {
                sd.BufferCount = 1;
                sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                if (FAILED(factory->CreateSwapChain(g_dev, &sd, &o.chain))) o.chain = nullptr;
            }
            if (o.chain)
            {
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                LOG("%s swap chain created (%s)", o.name,
                    sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip" : "blit");
            }
        }
        SafeRelease(factory);
        SafeRelease(adapter);
        SafeRelease(dxgiDev);
        if (!o.chain) { LOG("Could not create the %s swap chain", o.name); return false; }

        ID3D11Texture2D* buf = nullptr;
        if (FAILED(o.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buf)))
            || FAILED(g_dev->CreateRenderTargetView(buf, nullptr, &o.rtv)))
        {
            SafeRelease(buf);
            LOG("Could not create the %s render target", o.name);
            SafeRelease(o.chain);
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
    void DrawRegions(int window)
    {
        Output& o = g_outs[window];
        const std::vector<Placement> placements = Placements(window, o.w, o.h);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(o.w);
        vp.Height = static_cast<float>(o.h);
        vp.MaxDepth = 1.0f;

        const float black[4] = { 0, 0, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &o.rtv, nullptr);
        g_ctx->ClearRenderTargetView(o.rtv, black);
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
        for (const Placement& p : placements)
        {
            const Rect& d = p.dest;
            const Rect& s = p.source;
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

    bool IsOurChain(IDXGISwapChain* chain)
    {
        for (const Output& o : g_outs)
            if (o.chain && o.chain == chain) return true;
        return false;
    }

    void Composite(IDXGISwapChain* gameChain)
    {
        if (g_failed) return;
        for (const Output& o : g_outs)
            if (!o.ready.load()) return;   // window thread still setting up

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

        bool any = false;
        for (Output& o : g_outs)
        {
            if (o.failed || !o.hwnd.load()) continue;
            if (EnsureOutput(o)) any = true;
            else o.failed = true;
        }
        if (!any) { LOG("No output windows; compositor disabled"); g_failed = true; return; }

        // Tie our windows to the game's window (once per game window)
        DXGI_SWAP_CHAIN_DESC scd;
        if (SUCCEEDED(gameChain->GetDesc(&scd)) && scd.OutputWindow && scd.OutputWindow != g_gameWnd)
        {
            g_gameWnd = scd.OutputWindow;
            for (const Output& o : g_outs)
                if (HWND h = o.hwnd.load())
                    PostMessageW(h, WM_APP_ATTACH, reinterpret_cast<WPARAM>(g_gameWnd), 0);
        }

        ID3D11Texture2D* bb = nullptr;
        if (FAILED(gameChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
        D3D11_TEXTURE2D_DESC desc;
        bb->GetDesc(&desc);

        if (!g_loggedSizeMismatch && g_cfg.renderW > 0 && g_cfg.renderH > 0 &&
            (desc.Width != static_cast<UINT>(g_cfg.renderW) || desc.Height != static_cast<UINT>(g_cfg.renderH)))
        {
            g_loggedSizeMismatch = true;
            LOG("Game is rendering at %ux%u, not the requested %dx%d",
                desc.Width, desc.Height, g_cfg.renderW, g_cfg.renderH);
        }

        if (!EnsureSource(desc)) { SafeRelease(bb); g_failed = true; return; }

        if (desc.SampleDesc.Count > 1)
            g_ctx->ResolveSubresource(g_srcTex, 0, bb, 0, ViewFormat(desc.Format));
        else
            g_ctx->CopySubresourceRegion(g_srcTex, 0, 0, 0, 0, bb, 0, nullptr);
        SafeRelease(bb);
        if (g_srcMips) g_ctx->GenerateMips(g_srcSRV);

        Backup(g_backup);
        for (int w = 0; w < kWindowCount; ++w)
            if (g_outs[w].chain) DrawRegions(w);
        Restore(g_backup);

        // The panel first: it doesn't wait for vsync, so the main window's wait
        // doesn't hold it back.
        for (int w = kWindowCount - 1; w >= 0; --w)
        {
            Output& o = g_outs[w];
            if (!o.chain) continue;
            o.chain->Present(o.vsync ? 1 : 0, 0);
            if (!o.shown)
            {
                o.shown = true;
                ShowWindowAsync(o.hwnd.load(), SW_SHOWNOACTIVATE);
            }
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
        if (!IsOurChain(chain) && !(flags & DXGI_PRESENT_TEST) && !g_failed)
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
    Output& main = g_outs[kMainWindow];
    main.name = "Output";
    main.x = g_cfg.outX;
    main.y = g_cfg.outY;
    main.w = g_cfg.outW;
    main.h = g_cfg.outH;
    main.vsync = g_cfg.vsync;
    Output& panel = g_outs[kPanelWindow];
    panel.name = "Touch panel";
    panel.vsync = g_cfg.panelVsync;   // position and size are found on the window thread

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
