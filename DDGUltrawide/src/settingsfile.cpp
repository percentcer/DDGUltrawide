#include "settingsfile.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
    using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    CreateFileWFn g_origCreateFileW = nullptr;

    int g_width = 0, g_height = 0;
    thread_local bool t_inHook = false;   // our own file access mustn't re-enter

    bool EndsWithNoCase(const wchar_t* s, const wchar_t* suffix)
    {
        const size_t n = wcslen(s), m = wcslen(suffix);
        return n >= m && _wcsicmp(s + n - m, suffix) == 0;
    }

    bool ReadAll(const wchar_t* path, std::vector<char>& data)
    {
        HANDLE h = g_origCreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size;
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20);
        if (ok)
        {
            data.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = data.empty() || (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)
                                  && read == data.size());
        }
        CloseHandle(h);
        return ok;
    }

    bool WriteAll(const wchar_t* path, const std::vector<char>& data)
    {
        HANDLE h = g_origCreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
                        && written == data.size();
        CloseHandle(h);
        return ok;
    }

    // Sets Key=Value inside [Section], adding the key (or section) if missing.
    void SetIniValue(std::wstring& text, const std::wstring& section, const std::wstring& key, const std::wstring& value)
    {
        const std::wstring header = L"[" + section + L"]";
        size_t secStart = text.find(header);
        if (secStart == std::wstring::npos)
        {
            if (!text.empty() && text.back() != L'\n') text += L"\r\n";
            text += header + L"\r\n" + key + L"=" + value + L"\r\n";
            return;
        }
        size_t bodyStart = text.find(L'\n', secStart);
        bodyStart = (bodyStart == std::wstring::npos) ? text.size() : bodyStart + 1;
        size_t secEnd = text.find(L"\n[", bodyStart);
        secEnd = (secEnd == std::wstring::npos) ? text.size() : secEnd + 1;

        size_t line = bodyStart;
        while (line < secEnd)
        {
            size_t lineEnd = text.find(L'\n', line);
            if (lineEnd == std::wstring::npos || lineEnd > secEnd) lineEnd = secEnd;
            if (text.compare(line, key.size() + 1, key + L"=") == 0)
            {
                size_t valueEnd = lineEnd;
                while (valueEnd > line && (text[valueEnd - 1] == L'\n' || text[valueEnd - 1] == L'\r')) --valueEnd;
                const size_t valueStart = line + key.size() + 1;
                text.replace(valueStart, valueEnd - valueStart, value);
                return;
            }
            line = lineEnd + (lineEnd < text.size() && text[lineEnd] == L'\n' ? 1 : 0);
            if (lineEnd == secEnd) break;
        }
        text.insert(bodyStart, key + L"=" + value + L"\r\n");
    }

    void PatchSettingsFile(const wchar_t* path)
    {
        std::vector<char> raw;
        if (!ReadAll(path, raw))
        {
            LOG("Could not read %ls", path);
            return;
        }

        // UE4 writes these files as plain ASCII, or UTF-16 LE with a BOM.
        const bool utf16 = raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF
                           && static_cast<unsigned char>(raw[1]) == 0xFE;
        std::wstring text;
        if (utf16)
            text.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
        else
            text.assign(raw.begin(), raw.end());   // ASCII

        const std::wstring before = text;
        const std::wstring section = L"/Script/Engine.GameUserSettings";
        const std::wstring w = std::to_wstring(g_width), h = std::to_wstring(g_height);
        SetIniValue(text, section, L"ResolutionSizeX", w);
        SetIniValue(text, section, L"ResolutionSizeY", h);
        SetIniValue(text, section, L"LastUserConfirmedResolutionSizeX", w);
        SetIniValue(text, section, L"LastUserConfirmedResolutionSizeY", h);
        SetIniValue(text, section, L"FullscreenMode", L"2");               // windowed
        SetIniValue(text, section, L"LastConfirmedFullscreenMode", L"2");

        if (text == before)
        {
            LOG("GameUserSettings.ini already set to %dx%d windowed", g_width, g_height);
            return;
        }

        std::vector<char> out;
        if (utf16)
        {
            out = { static_cast<char>(0xFF), static_cast<char>(0xFE) };
            const char* p = reinterpret_cast<const char*>(text.data());
            out.insert(out.end(), p, p + text.size() * sizeof(wchar_t));
        }
        else
        {
            for (wchar_t c : text) out.push_back(static_cast<char>(c));
        }
        if (WriteAll(path, out))
            LOG("Set GameUserSettings.ini to %dx%d windowed (%ls)", g_width, g_height, path);
        else
            LOG("Could not write %ls (error %lu)", path, GetLastError());
    }

    HANDLE WINAPI Hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disposition, DWORD flags, HANDLE templ)
    {
        if (!t_inHook && path && (access & GENERIC_READ) && !(access & GENERIC_WRITE)
            && EndsWithNoCase(path, L"\\GameUserSettings.ini"))
        {
            t_inHook = true;
            PatchSettingsFile(path);
            t_inHook = false;
        }
        return g_origCreateFileW(path, access, share, sa, disposition, flags, templ);
    }
}

bool InstallSettingsFileHook(int width, int height)
{
    g_width = width;
    g_height = height;
    const bool ok =
        MH_CreateHookApi(L"kernelbase", "CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW),
                         reinterpret_cast<void**>(&g_origCreateFileW)) == MH_OK &&
        MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    LOG(ok ? "Settings file hook installed" : "Settings file hook FAILED");
    return ok;
}
