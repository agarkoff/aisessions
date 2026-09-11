#include "Settings.h"
#include "Json.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path settingsDir() {
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    std::wstring base;
    if (n > 0) {
        base.resize(n);
        base.resize(GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), n));
    }
    if (base.empty()) {
        wchar_t buf[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
            base = buf;
        else
            base = L".";
    }
    return fs::path(base) / L"AISessions";
}

fs::path settingsPath() {
    return settingsDir() / L"settings.json";
}

// Guards against a corrupt or hand-edited settings file resizing the window
// into something unusable.
int clampWidth(long long v, int fallback, int lo, int hi) {
    if (v < lo || v > hi) return fallback;
    return static_cast<int>(v);
}

} // namespace

Settings Settings::load() {
    Settings s;
    std::ifstream in(settingsPath(), std::ios::binary);
    if (!in) return s;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) return s;

    mini::JValue root;
    if (!mini::parse(text, root) || !root.isObject()) return s;
    if (root.get("leftWidth")) s.leftWidth = clampWidth(root.intOf("leftWidth"), s.leftWidth, 60, 4000);
    if (root.get("col0")) s.col0 = clampWidth(root.intOf("col0"), s.col0, 20, 2000);
    if (root.get("col1")) s.col1 = clampWidth(root.intOf("col1"), s.col1, 20, 2000);
    if (root.get("col3")) s.col3 = clampWidth(root.intOf("col3"), s.col3, 20, 2000);
    if (root.get("col4")) s.col4 = clampWidth(root.intOf("col4"), s.col4, 20, 2000);
    if (root.get("sortColumn")) {
        long long c = root.intOf("sortColumn");
        if (c >= 0 && c < 5) s.sortColumn = static_cast<int>(c);
    }
    if (const mini::JValue* v = root.get("sortDescending"); v && v->isBool())
        s.sortDescending = v->b;
    return s;
}

void Settings::save() const {
    std::string text =
        "{\n"
        "  \"leftWidth\": " + std::to_string(leftWidth) + ",\n"
        "  \"col0\": " + std::to_string(col0) + ",\n"
        "  \"col1\": " + std::to_string(col1) + ",\n"
        "  \"col3\": " + std::to_string(col3) + ",\n"
        "  \"col4\": " + std::to_string(col4) + ",\n"
        "  \"sortColumn\": " + std::to_string(sortColumn) + ",\n"
        "  \"sortDescending\": " + (sortDescending ? "true" : "false") + "\n"
        "}\n";

    std::error_code ec;
    fs::create_directories(settingsDir(), ec);
    std::ofstream out(settingsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << text;
}
