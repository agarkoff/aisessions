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

    auto width = [&](const char* key, int& into, int lo, int hi) {
        if (root.get(key)) into = clampWidth(root.intOf(key), into, lo, hi);
    };
    width("leftWidth", s.leftWidth, 60, 4000);
    width("agentWidth", s.agentWidth, 20, 2000);
    width("sessionIdWidth", s.sessionIdWidth, 20, 2000);
    width("modelWidth", s.modelWidth, 20, 2000);
    width("sizeWidth", s.sizeWidth, 20, 2000);
    width("updatedWidth", s.updatedWidth, 20, 2000);

    std::string sortBy = root.strOf("sortBy");
    if (!sortBy.empty()) s.sortBy = sortBy;  // an unknown name falls back in the window
    if (const mini::JValue* v = root.get("sortDescending"); v && v->isBool())
        s.sortDescending = v->b;
    return s;
}

void Settings::save() const {
    std::string text =
        "{\n"
        "  \"leftWidth\": " + std::to_string(leftWidth) + ",\n"
        "  \"agentWidth\": " + std::to_string(agentWidth) + ",\n"
        "  \"sessionIdWidth\": " + std::to_string(sessionIdWidth) + ",\n"
        "  \"modelWidth\": " + std::to_string(modelWidth) + ",\n"
        "  \"sizeWidth\": " + std::to_string(sizeWidth) + ",\n"
        "  \"updatedWidth\": " + std::to_string(updatedWidth) + ",\n"
        "  \"sortBy\": \"" + sortBy + "\",\n"
        "  \"sortDescending\": " + (sortDescending ? "true" : "false") + "\n"
        "}\n";

    std::error_code ec;
    fs::create_directories(settingsDir(), ec);
    std::ofstream out(settingsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << text;
}
