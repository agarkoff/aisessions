#pragma once
#include <windows.h>

// Light/dark palette that follows the Windows "app mode" setting.
//
// Only the window frame has a documented dark-mode switch (DWMWA_USE_IMMERSIVE
// DARK_MODE). The common controls are brought along with a mix of documented
// theme class names ("DarkMode_Explorer") and one undocumented uxtheme export,
// all resolved at run time so an OS without them simply stays light.
class Theme {
public:
    static Theme& instance();

    // Re-reads the system setting. Returns true when the mode actually changed.
    bool refresh();

    bool dark() const { return dark_; }

    COLORREF background() const;   // window / control backdrop
    COLORREF surface() const;      // list and tree backdrop
    COLORREF text() const;
    COLORREF dimText() const;      // status line
    COLORREF border() const;
    COLORREF accent() const;       // toast fill

    HBRUSH backgroundBrush();
    HBRUSH surfaceBrush();

    // Name to hand SetWindowTheme / INameSpaceTreeControl::SetTheme.
    const wchar_t* explorerThemeName() const;
    const wchar_t* itemsViewThemeName() const;

    // Process-wide opt-in; safe to call repeatedly. Must run before the first
    // themed control is created for scrollbars and menus to follow suit.
    static void applyProcessMode(bool dark);

    // Dark title bar and border for one top-level window.
    static void applyWindowFrame(HWND hwnd, bool dark);

    // True when lParam of WM_SETTINGCHANGE announces a colour-scheme change.
    static bool isColorSchemeChange(LPARAM lParam);

private:
    Theme() { refresh(); }
    void releaseBrushes();

    bool dark_ = false;
    HBRUSH bgBrush_ = nullptr;
    HBRUSH surfaceBrush_ = nullptr;
};
