#pragma once
#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

inline std::wstring utf8to16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    if (n <= 0) return {};
    std::wstring out;
    out.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

inline std::string utf16to8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out;
    out.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

inline void toLowerW(std::wstring& s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
}

inline std::wstring lowerW(std::wstring s) {
    toLowerW(s);
    return s;
}

inline bool startsWithW(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// True when `path` is `prefix` itself or lives underneath it. Unlike a plain
// startsWith this respects component boundaries, so "D:\ProjectsX" is not
// treated as a child of "D:\Projects". Both sides must already be lowercased.
inline bool isPathPrefixW(const std::wstring& path, const std::wstring& prefix) {
    if (prefix.empty()) return true;
    if (!startsWithW(path, prefix)) return false;
    return path.size() == prefix.size() || path[prefix.size()] == L'\\';
}

// Splits on `sep`, dropping empty segments so trailing or doubled separators
// do not produce blank path components.
inline std::vector<std::wstring> splitW(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(sep, pos);
        if (next == std::wstring::npos) {
            if (pos < s.size()) out.push_back(s.substr(pos));
            break;
        }
        if (next > pos) out.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

// Directory holding the running executable, with a trailing backslash.
inline std::wstring exeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    size_t slash = buf.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring{} : buf.substr(0, slash + 1);
}
