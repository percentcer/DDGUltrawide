#include "cmdline.h"
#include "log.h"

#include <windows.h>
#include <cwchar>
#include <list>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    int g_renderW = 0, g_renderH = 0;
    std::wstring g_extra;

    using GetCommandLineWFn = LPWSTR(WINAPI*)();
    using GetCommandLineAFn = LPSTR(WINAPI*)();
    GetCommandLineWFn g_realW = nullptr;
    GetCommandLineAFn g_realA = nullptr;

    // Every command line we hand out stays alive for the life of the process.
    std::mutex g_mutex;
    std::wstring g_lastSourceW;
    std::list<std::wstring> g_keepW;
    std::list<std::string> g_keepA;

    // Splits a command line into arguments, keeping quotes as written.
    std::vector<std::wstring> SplitArgs(const std::wstring& cmd)
    {
        std::vector<std::wstring> args;
        std::wstring cur;
        bool quoted = false, any = false;
        for (wchar_t c : cmd)
        {
            if (c == L'"') { quoted = !quoted; cur += c; any = true; continue; }
            if (!quoted && (c == L' ' || c == L'\t'))
            {
                if (any) { args.push_back(cur); cur.clear(); any = false; }
                continue;
            }
            cur += c;
            any = true;
        }
        if (any) args.push_back(cur);
        return args;
    }

    bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
    {
        const size_t n = wcslen(prefix);
        return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
    }

    // Arguments we replace when forcing the render size. Unreal matches "ResX="
    // anywhere in the command line and uses the first match, so any existing
    // one has to go, with or without a leading - or /.
    bool IsReplacedArg(std::wstring a)
    {
        if (!a.empty() && (a[0] == L'-' || a[0] == L'/')) a.erase(0, 1);
        return StartsWithNoCase(a, L"ResX=") || StartsWithNoCase(a, L"ResY=") ||
               _wcsicmp(a.c_str(), L"windowed") == 0 || _wcsicmp(a.c_str(), L"fullscreen") == 0 ||
               _wcsicmp(a.c_str(), L"ForceRes") == 0;
    }

    std::wstring EditCommandLine(const std::wstring& source)
    {
        const std::vector<std::wstring> args = SplitArgs(source);
        std::wstring out;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (i > 0 && g_renderW > 0 && g_renderH > 0 && IsReplacedArg(args[i])) continue;   // [0] is the exe
            if (!out.empty()) out += L' ';
            out += args[i];
        }
        if (g_renderW > 0 && g_renderH > 0)
            out += L" -ResX=" + std::to_wstring(g_renderW) + L" -ResY=" + std::to_wstring(g_renderH) +
                   L" -windowed -ForceRes";
        if (!g_extra.empty()) out += L" " + g_extra;
        return out;
    }

    LPWSTR WINAPI Hook_GetCommandLineW()
    {
        const LPWSTR real = g_realW();
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::wstring source = real ? real : L"";
        if (g_keepW.empty() || source != g_lastSourceW)
        {
            g_lastSourceW = source;
            g_keepW.push_back(EditCommandLine(source));
            LOG("Command line as received: %ls", source.c_str());
            LOG("Command line given to the game: %ls", g_keepW.back().c_str());
        }
        return g_keepW.back().data();
    }

    LPSTR WINAPI Hook_GetCommandLineA()
    {
        const std::wstring w = Hook_GetCommandLineW();
        std::lock_guard<std::mutex> lock(g_mutex);
        const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string a(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, a.data(), n, nullptr, nullptr);
        g_keepA.push_back(a);
        return g_keepA.back().data();
    }

    // Points the main executable's import-table entry for `name` at `hook`.
    // Returns the number of entries patched.
    int PatchImport(const char* name, void* hook)
    {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return 0;

        int patched = 0;
        for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
        {
            if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
            auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addrs)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(byName->Name), name) != 0) continue;

                DWORD old;
                if (VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                {
                    addrs->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
                    VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
                    ++patched;
                }
            }
        }
        return patched;
    }
}

bool InstallCommandLineHook(int renderW, int renderH, const std::wstring& extra)
{
    g_renderW = renderW;
    g_renderH = renderH;
    g_extra = extra;
    if ((renderW <= 0 || renderH <= 0) && extra.empty())
    {
        LOG("Command line left unchanged");
        return false;
    }

    // Calls through kernel32 go through any launcher hooks on it.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    g_realW = reinterpret_cast<GetCommandLineWFn>(GetProcAddress(k32, "GetCommandLineW"));
    g_realA = reinterpret_cast<GetCommandLineAFn>(GetProcAddress(k32, "GetCommandLineA"));
    if (!g_realW)
    {
        LOG("Could not find GetCommandLineW; command line unchanged");
        return false;
    }

    const int w = PatchImport("GetCommandLineW", reinterpret_cast<void*>(&Hook_GetCommandLineW));
    const int a = PatchImport("GetCommandLineA", reinterpret_cast<void*>(&Hook_GetCommandLineA));
    LOG("Command line hook installed in the game's import table (%d W, %d A entries)", w, a);
    return w + a > 0;
}
