#include "layout.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <vector>

namespace
{
    // Unreal's TArray memory layout: pointer, count, capacity.
    struct TArrayRaw
    {
        void* data;
        int32_t num;
        int32_t max;
    };

    using LayoutPlayersFn = void(__fastcall*)(void* viewportClient);
    LayoutPlayersFn g_origLayoutPlayers = nullptr;

    std::vector<Rect> g_snapped;
    bool g_loggedApplied = false;
    int g_lastProblem = -1;

    void Problem(int code, const char* what)
    {
        // Log each distinct problem once, not every frame.
        if (g_lastProblem != code)
        {
            g_lastProblem = code;
            LOG("Layout not applied: %s", what);
        }
    }

    // Writes our rects into the split-screen table entry the engine is about to use.
    // UGameViewportClient (UE 4.15):
    //   SplitscreenInfo: TArray<FSplitscreenData>, each entry a TArray<FPerPlayerSplitscreenData>
    //   FPerPlayerSplitscreenData = { float SizeX, SizeY, OriginX, OriginY }
    void ApplyLayout(uint8_t* vc)
    {
        auto* info = reinterpret_cast<TArrayRaw*>(vc + g_cfg.offSplitscreenInfo);
        const uint8_t active = *(vc + g_cfg.offActiveType);

        if (!info->data || info->num <= 0 || info->num > 16) { Problem(1, "split-screen table missing"); return; }
        if (active >= info->num) { Problem(2, "active split type out of range"); return; }

        auto* entry = reinterpret_cast<TArrayRaw*>(static_cast<uint8_t*>(info->data) + sizeof(TArrayRaw) * active);
        if (!entry->data || entry->num != static_cast<int32_t>(g_snapped.size()))
        {
            Problem(3, "active entry has a different player count than [Layout]");
            return;
        }

        auto* rects = static_cast<float*>(entry->data);
        for (size_t i = 0; i < g_snapped.size(); ++i)
        {
            rects[i * 4 + 0] = g_snapped[i].sizeX;
            rects[i * 4 + 1] = g_snapped[i].sizeY;
            rects[i * 4 + 2] = g_snapped[i].originX;
            rects[i * 4 + 3] = g_snapped[i].originY;
        }

        g_lastProblem = -1;
        if (!g_loggedApplied)
        {
            g_loggedApplied = true;
            LOG("Layout applied (split type %u, %zu players)", active, g_snapped.size());
        }
    }

    // If the RVA points at the wrong function, the reads above could fault.
    // Catch that, log it once, and stop touching memory instead of crashing.
    bool g_disabled = false;

    bool SafeApply(uint8_t* vc)
    {
        __try
        {
            ApplyLayout(vc);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void __fastcall Hook_LayoutPlayers(void* vc)
    {
        if (vc && !g_disabled && !SafeApply(static_cast<uint8_t*>(vc)))
        {
            g_disabled = true;
            LOG("Memory fault while applying layout; LayoutPlayersRVA probably points at the wrong function. Layout disabled.");
        }
        g_origLayoutPlayers(vc);
    }
}

bool InstallLayoutHook()
{
    if (g_cfg.layoutPlayersRva == 0)
    {
        LOG("LayoutPlayersRVA not set in the ini; layout hook disabled");
        return false;
    }
    if (g_cfg.players.empty())
    {
        LOG("No [Layout] entries; layout hook disabled");
        return false;
    }

    g_snapped = SnappedLayout();

    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    void* target = base + g_cfg.layoutPlayersRva;
    LOG("Hooking LayoutPlayers at %p (base %p + 0x%llX)", target, base,
        static_cast<unsigned long long>(g_cfg.layoutPlayersRva));

    if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_LayoutPlayers),
                      reinterpret_cast<void**>(&g_origLayoutPlayers)) != MH_OK)
    {
        LOG("MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK)
    {
        LOG("MH_EnableHook failed");
        return false;
    }
    LOG("LayoutPlayers hook installed");
    return true;
}
