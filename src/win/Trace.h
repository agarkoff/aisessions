#pragma once
#include <windows.h>

#include <cstdio>
#include <string>

// Diagnostic log, off unless AISESSIONS_TRACE names a file. Shell controls do
// most of their work behind COM callbacks, and this is the only way to see
// which of them actually fire for a given gesture.
inline void traceW(const wchar_t* format, ...) {
    static const std::wstring path = [] {
        wchar_t buf[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableW(L"AISESSIONS_TRACE", buf, MAX_PATH);
        return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
    }();
    if (path.empty()) return;

    wchar_t line[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, line);
        fclose(f);
    }
}
