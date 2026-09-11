#pragma once
#include <windows.h>
#include <string>

class MainWindow;

class ToastWindow {
public:
    static ToastWindow& instance();

    ToastWindow(const ToastWindow&) = delete;
    ToastWindow& operator=(const ToastWindow&) = delete;

    // Lazily registers the class and creates the hidden popup window.
    void setup(MainWindow* owner);
    void show(MainWindow* owner, const std::wstring& text);

private:
    ToastWindow() = default;

    static constexpr wchar_t kClassName[] = L"AISessions_ToastWindow";
    static constexpr UINT_PTR kToastTimerId = 99;
    static constexpr UINT kFrameMs = 15;

    enum class Phase { FadeIn, Hold, FadeOut };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void onTimer();
    void onPaint();
    void createFont(UINT dpi);

    HWND hwnd_ = nullptr;
    HFONT hFont_ = nullptr;
    std::wstring text_;
    UINT dpi_ = 96;
    int alpha_ = 0;
    int holdTicks_ = 0;
    Phase phase_ = Phase::FadeIn;
};
