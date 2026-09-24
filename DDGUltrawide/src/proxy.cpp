#include "proxy.h"
#include "log.h"

#include <windows.h>
#include <string>

// Order must match the THUNK indices in proxy_thunks.asm.
static const char* kExportNames[] = {
    "GetFileVersionInfoA",
    "GetFileVersionInfoByHandle",
    "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA",
    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW",
    "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW",
    "VerFindFileA",
    "VerFindFileW",
    "VerInstallFileA",
    "VerInstallFileW",
    "VerLanguageNameA",
    "VerLanguageNameW",
    "VerQueryValueA",
    "VerQueryValueW"
};
constexpr size_t kExportCount = sizeof(kExportNames) / sizeof(kExportNames[0]);

extern "C"
{
    void* g_origFuncs[kExportCount] = {};
}

bool LoadOriginalVersionDll()
{
    wchar_t sysDir[MAX_PATH];
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    std::wstring path = std::wstring(sysDir) + L"\\version.dll";
    HMODULE real = LoadLibraryW(path.c_str());
    if (!real) return false;

    for (size_t i = 0; i < kExportCount; ++i)
        g_origFuncs[i] = reinterpret_cast<void*>(GetProcAddress(real, kExportNames[i]));
    return true;
}
