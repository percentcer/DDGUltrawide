#pragma once
#include <string>

namespace logx
{
    void Init(const std::wstring& path);
    void Write(const char* fmt, ...);
}

#define LOG(...) logx::Write(__VA_ARGS__)
