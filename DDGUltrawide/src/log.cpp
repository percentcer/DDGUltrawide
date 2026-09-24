#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
    FILE* g_file = nullptr;
    std::mutex g_mutex;
}

void logx::Init(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) g_file = _wfopen(path.c_str(), L"w");
}

void logx::Write(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);

    fputc('\n', g_file);
    fflush(g_file);
}
