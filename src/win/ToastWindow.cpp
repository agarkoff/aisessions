#include "ToastWindow.h"
#include "MainWindow.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr COLORREF kToastText = RGB(0xff, 0xff, 0xff);

int scaleFor(UINT dpi, double v) {
    return static_cast<int>(std::lround(v * dpi / 96.0));
}

} // namespace

ToastWindow& ToastWindow::instance() {
    static ToastWindow win;
    return win;
}

LRESULT CALLBACK ToastWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ToastWindow* self = reinterpret_cast<ToastWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ToastWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (self && self->hwnd_ == hwnd) {
        if (msg == WM_NCDESTROY) {
            LRESULT r = self->handleMessage(msg, wParam, lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            return r;
        }
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ToastWindow::setup(MainWindow* owner) {
    if (hwnd_ || !owner) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ToastWindow::wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    dpi_ = owner->dpi() ? owner->dpi() : 96;

    // WS_EX_LAYERED is what makes SetLayeredWindowAttributes (the fade) work.
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClassName, L"", WS_POPUP,
        0, 0, scaleFor(dpi_, 200), scaleFor(dpi_, 42),
        nullptr, nullptr, wc.hInstance, this);
}

void ToastWindow::createFont(UINT dpi) {
    if (hFont_) DeleteObject(hFont_);
    int height = -MulDiv(12, static_cast<int>(dpi), 72);
    hFont_ = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void ToastWindow::show(MainWindow* owner, const std::wstring& text) {
    if (!owner || !owner->handle()) return;
    if (!hwnd_) setup(owner);
    if (!hwnd_) return;

    text_ = text;
    dpi_ = owner->dpi() ? owner->dpi() : 96;
    createFont(dpi_);

    int padX = scaleFor(dpi_, 20);
    int h = scaleFor(dpi_, 42);

    // Size the bubble to the text, capped so a long id cannot run off-screen.
    int textW = scaleFor(dpi_, 120);
    if (HDC dc = GetDC(hwnd_)) {
        HGDIOBJ old = SelectObject(dc, hFont_);
        RECT calc{};
        DrawTextW(dc, text_.c_str(), static_cast<int>(text_.size()), &calc,
                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        textW = calc.right - calc.left;
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
    }

    RECT ownerRc{};
    GetWindowRect(owner->handle(), &ownerRc);
    int ownerW = static_cast<int>(ownerRc.right - ownerRc.left);
    int maxW = std::max(scaleFor(dpi_, 200), ownerW - scaleFor(dpi_, 64));
    int w = std::min(textW + padX * 2, maxW);

    int x = ownerRc.left + (ownerRc.right - ownerRc.left - w) / 2;
    int y = ownerRc.bottom - scaleFor(dpi_, 48) - h;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

    int radius = scaleFor(dpi_, 8);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius, radius);
    SetWindowRgn(hwnd_, rgn, TRUE);  // the window owns rgn from here on

    alpha_ = 0;
    holdTicks_ = 0;
    phase_ = Phase::FadeIn;
    SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
    SetTimer(hwnd_, kToastTimerId, kFrameMs, nullptr);
}

void ToastWindow::onTimer() {
    switch (phase_) {
    case Phase::FadeIn:
        alpha_ += 32;
        if (alpha_ >= 255) {
            alpha_ = 255;
            phase_ = Phase::Hold;
            holdTicks_ = 0;
        }
        break;

    case Phase::Hold:
        // ~1.5s at 15ms per frame.
        if (++holdTicks_ >= 100) phase_ = Phase::FadeOut;
        break;

    case Phase::FadeOut:
        // alpha_ is a signed int precisely so this can go below zero.
        alpha_ -= 24;
        if (alpha_ <= 0) {
            alpha_ = 0;
            KillTimer(hwnd_, kToastTimerId);
            ShowWindow(hwnd_, SW_HIDE);
            return;
        }
        break;
    }
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(alpha_), LWA_ALPHA);
}

void ToastWindow::onPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    // Draw off-screen so the rounded fill and the text land in one blit.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(Theme::instance().accent());
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    HGDIOBJ oldFont = SelectObject(mem, hFont_);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, kToastText);
    RECT textRc = {scaleFor(dpi_, 12), 0, w - scaleFor(dpi_, 12), h};
    DrawTextW(mem, text_.c_str(), static_cast<int>(text_.size()), &textRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(mem, oldFont);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(hwnd_, &ps);
}

LRESULT ToastWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        onPaint();
        return 0;

    case WM_TIMER:
        if (wParam == kToastTimerId) onTimer();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_NCDESTROY: {
        LRESULT r = DefWindowProcW(hwnd_, msg, wParam, lParam);
        if (hFont_) { DeleteObject(hFont_); hFont_ = nullptr; }
        return r;
    }

    default:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}
