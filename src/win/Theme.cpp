#include "Theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <cstring>

namespace {

// Documented since Windows 10 20H1; harmless (returns an error) on older builds.
constexpr DWORD kUseImmersiveDarkMode = 20;

// uxtheme exports these by ordinal only. They are what makes comctl32 draw dark
// scrollbars and menus; without them the rest of the palette still works, the
// scrollbars just stay light.
enum class PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();

struct UxThemeHooks {
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;

    UxThemeHooks() {
        HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
        if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ux) return;
        setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(ux, MAKEINTRESOURCEA(135)));
        flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
            GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    }
};

const UxThemeHooks& hooks() {
    static UxThemeHooks h;
    return h;
}

// AISESSIONS_THEME=dark|light overrides the system setting. Handy for checking
// both palettes without switching the whole desktop.
bool readOverride(bool& dark) {
    wchar_t buf[16]{};
    DWORD n = GetEnvironmentVariableW(L"AISESSIONS_THEME", buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return false;
    if (_wcsicmp(buf, L"dark") == 0) { dark = true; return true; }
    if (_wcsicmp(buf, L"light") == 0) { dark = false; return true; }
    return false;
}

bool readSystemDarkMode() {
    bool forced = false;
    if (readOverride(forced)) return forced;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 1, size = sizeof(value), type = 0;
    LSTATUS st = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_DWORD) return false;
    return value == 0;
}

} // namespace

Theme& Theme::instance() {
    static Theme t;
    return t;
}

bool Theme::refresh() {
    bool now = readSystemDarkMode();
    if (now == dark_ && (bgBrush_ || surfaceBrush_ || fieldBrush_)) return false;
    bool changed = now != dark_;
    dark_ = now;
    releaseBrushes();
    return changed;
}

void Theme::releaseBrushes() {
    if (bgBrush_) { DeleteObject(bgBrush_); bgBrush_ = nullptr; }
    if (surfaceBrush_) { DeleteObject(surfaceBrush_); surfaceBrush_ = nullptr; }
    if (fieldBrush_) { DeleteObject(fieldBrush_); fieldBrush_ = nullptr; }
}

COLORREF Theme::background() const {
    return dark_ ? RGB(0x20, 0x20, 0x20) : RGB(0xf3, 0xf3, 0xf3);
}
COLORREF Theme::surface() const {
    return dark_ ? RGB(0x2b, 0x2b, 0x2b) : RGB(0xff, 0xff, 0xff);
}
COLORREF Theme::text() const {
    return dark_ ? RGB(0xf0, 0xf0, 0xf0) : RGB(0x1a, 0x1a, 0x1a);
}
COLORREF Theme::dimText() const {
    return dark_ ? RGB(0x9a, 0x9a, 0x9a) : RGB(0x5f, 0x5f, 0x5f);
}
COLORREF Theme::border() const {
    return dark_ ? RGB(0x3d, 0x3d, 0x3d) : RGB(0xdc, 0xdc, 0xdc);
}
COLORREF Theme::field() const {
    return dark_ ? RGB(0x33, 0x33, 0x33) : RGB(0xff, 0xff, 0xff);
}
COLORREF Theme::fieldBorder() const {
    return dark_ ? RGB(0x4a, 0x4a, 0x4a) : RGB(0xc4, 0xc4, 0xc4);
}
COLORREF Theme::fieldHover() const {
    return dark_ ? RGB(0x3d, 0x3d, 0x3d) : RGB(0xef, 0xef, 0xef);
}
COLORREF Theme::fieldPressed() const {
    return dark_ ? RGB(0x4a, 0x4a, 0x4a) : RGB(0xe0, 0xe0, 0xe0);
}

COLORREF Theme::accent() const {
    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
        return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    return dark_ ? RGB(0x2f, 0x5d, 0x8f) : RGB(0x00, 0x5a, 0x9e);
}

HBRUSH Theme::backgroundBrush() {
    if (!bgBrush_) bgBrush_ = CreateSolidBrush(background());
    return bgBrush_;
}

HBRUSH Theme::surfaceBrush() {
    if (!surfaceBrush_) surfaceBrush_ = CreateSolidBrush(surface());
    return surfaceBrush_;
}

HBRUSH Theme::fieldBrush() {
    if (!fieldBrush_) fieldBrush_ = CreateSolidBrush(field());
    return fieldBrush_;
}

const wchar_t* Theme::explorerThemeName() const {
    return dark_ ? L"DarkMode_Explorer" : L"Explorer";
}

const wchar_t* Theme::itemsViewThemeName() const {
    return dark_ ? L"DarkMode_ItemsView" : L"ItemsView";
}

void Theme::applyProcessMode(bool dark) {
    if (auto fn = hooks().setPreferredAppMode)
        fn(dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
    if (auto fn = hooks().flushMenuThemes) fn();
}

void Theme::applyWindowFrame(HWND hwnd, bool dark) {
    if (!hwnd) return;
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof(value));
}

bool Theme::isColorSchemeChange(LPARAM lParam) {
    auto text = reinterpret_cast<const wchar_t*>(lParam);
    return text && wcscmp(text, L"ImmersiveColorSet") == 0;
}
