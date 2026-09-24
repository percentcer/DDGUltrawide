#include "uiscale.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace
{
    // float UUserInterfaceSettings::GetDPIScaleBasedOnSize(FIntPoint Size) const
    // FIntPoint (two int32s) arrives packed in one 64-bit register: X low, Y high.
    using GetDPIScaleFn = float(__fastcall*)(void* settings, uint64_t packedSize);
    GetDPIScaleFn g_orig = nullptr;

    std::atomic<uint64_t> g_lastLoggedSize{ 0 };

    float __fastcall Hook_GetDPIScaleBasedOnSize(void* settings, uint64_t packedSize)
    {
        const int32_t w = static_cast<int32_t>(packedSize & 0xFFFFFFFFull);
        const int32_t h = static_cast<int32_t>(packedSize >> 32);

        const size_t ref = static_cast<size_t>(g_cfg.uiReferencePlayer);
        if (w <= 0 || h <= 0 || ref >= g_cfg.players.size() || g_cfg.uiDesignHeight <= 0)
            return g_orig(settings, packedSize);

        // Reference tile's height in whole pixels (same rounding as the layout's pixel snap)
        const Rect& r = g_cfg.players[ref];
        const double top = std::floor(r.originY * h + 0.5);
        const double bottom = std::floor((r.originY + r.sizeY) * h + 0.5);
        const float scale = static_cast<float>((bottom - top) / g_cfg.uiDesignHeight);

        // Log whenever the viewport size changes (not every call)
        if (g_lastLoggedSize.exchange(packedSize) != packedSize)
        {
            const float engine = g_orig(settings, packedSize);
            LOG("UI scale for %dx%d: %.4f (engine would use %.4f)", w, h, scale, engine);
        }
        return scale;
    }
}

bool InstallUIScaleHook()
{
    if (!g_cfg.uiScaleEnabled)
    {
        LOG("UI scale hook disabled in the ini");
        return false;
    }
    if (g_cfg.dpiScaleRva == 0)
    {
        LOG("DPIScaleRVA not set in the ini; UI scale hook disabled");
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    void* target = base + g_cfg.dpiScaleRva;
    LOG("Hooking GetDPIScaleBasedOnSize at %p (base %p + 0x%llX)", target, base,
        static_cast<unsigned long long>(g_cfg.dpiScaleRva));

    if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_GetDPIScaleBasedOnSize),
                      reinterpret_cast<void**>(&g_orig)) != MH_OK)
    {
        LOG("MH_CreateHook failed (UI scale)");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK)
    {
        LOG("MH_EnableHook failed (UI scale)");
        return false;
    }
    LOG("UI scale hook installed (reference player P%d, design height %.0f)",
        g_cfg.uiReferencePlayer, g_cfg.uiDesignHeight);
    return true;
}
