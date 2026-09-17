#include "MainWindow.h"
#include "Theme.h"
#include "StrUtil.h"
#include "Trace.h"
#include "core/SessionActions.h"
#include "core/SessionLoader.h"
#include "resource.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <unordered_set>

LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // CreateWindowExW has not returned yet, so hwnd_ has to be published
        // here: WM_CREATE already needs it as the parent for every child
        // control, and GetDpiForWindow needs it to pick the font size.
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

MainWindow::MainWindow(const Settings& settings)
    : settings_(settings), leftWidth_(settings.leftWidth) {
    colWidth_[kAgent] = settings.agentWidth;
    colWidth_[kSessionId] = settings.sessionIdWidth;
    colWidth_[kModel] = settings.modelWidth;
    colWidth_[kSize] = settings.sizeWidth;
    colWidth_[kUpdated] = settings.updatedWidth;
    int sortBy = columnByName(settings.sortBy);
    sortColumn_ = sortBy >= 0 ? sortBy : kUpdated;
    sortDescending_ = settings.sortDescending;

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    Theme& theme = Theme::instance();
    // Must precede the first themed control so scrollbars follow the mode too.
    Theme::applyProcessMode(theme.dark());

    // The icon is embedded in the executable. Both sizes are requested
    // explicitly: LoadIcon would hand back a single 32x32 image that the title
    // bar then has to squash, which looks like a broken placeholder.
    auto loadIcon = [&](int cx, int cy) -> HICON {
        if (HICON fromResource = static_cast<HICON>(LoadImageW(
                hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR)))
            return fromResource;
        // Fallback for a build whose resources were stripped.
        std::wstring iconPath = exeDir() + L"assets\\AppIcon.ico";
        return static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                             cx, cy, LR_LOADFROMFILE));
    };
    hIconBig_ = loadIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    hIconSmall_ = loadIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = &MainWindow::wndProc;
    wc.hInstance = hInst;
    wc.hIcon = hIconBig_;
    wc.hIconSm = hIconSmall_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // painted from the theme in WM_ERASEBKGND
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    double s = GetDpiForSystem() / 96.0;
    int w = static_cast<int>(1200 * s);
    int h = static_cast<int>(700 * s);
    int sx = GetSystemMetrics(SM_CXSCREEN) / 2 - w / 2;
    int sy = static_cast<int>((GetSystemMetrics(SM_CYSCREEN) / 2 - h / 2) * 0.8);

    hwnd_ = CreateWindowExW(0, kClassName, L"AI Sessions",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            sx, sy, w, h, nullptr, nullptr, hInst, this);
    if (!hwnd_) return;

    Theme::applyWindowFrame(hwnd_, theme.dark());
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetFocus(hSearch_);

    loadSessionsAsync();
}

MainWindow::~MainWindow() {
    if (hIconBig_) DestroyIcon(hIconBig_);
    if (hIconSmall_) DestroyIcon(hIconSmall_);
}

bool MainWindow::translateAccelerator(MSG& msg) {
    if (!hwnd_) return false;
    if (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd)) return false;

    // Enter never reaches the list: IsDialogMessage claims it as the dialog's
    // default action. So the resume shortcut is taken here, ahead of it.
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
        msg.hwnd == hList_ && GetKeyState(VK_CONTROL) < 0) {
        resumeSessions(selectedRows());
        return true;
    }

    // Gives the controls their WS_TABSTOP behaviour; without this Tab does
    // nothing, because the main window is not a dialog.
    return IsDialogMessageW(hwnd_, &msg) != FALSE;
}

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    Theme& theme = Theme::instance();

    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        if (dpi_ == 0) dpi_ = 96;
        // Settings arrive in 96-dpi units; everything below works in pixels.
        rescaleGeometry(96, dpi_);
        createFont();
        createChildren();
        setupColumns();
        applyTheme();
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_SETFOCUS:
        SetFocus(hSearch_);
        return 0;

    case WM_COMMAND:
        onCommand(wParam, lParam);
        return 0;

    case WM_NOTIFY:
        return onNotify(lParam);

    case WM_LBUTTONDOWN:
        onSplitterDown(lParam);
        return 0;

    case WM_MOUSEMOVE:
        onSplitterMove(lParam);
        return 0;

    case WM_LBUTTONUP:
        onSplitterUp();
        return 0;

    case WM_CAPTURECHANGED:
        dragSplitter_ = false;
        return 0;

    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == hList_) {
            showListMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wParam) == hwnd_ && cursorOverSplitter()) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return 1;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = scale(560);
        mmi->ptMinTrackSize.y = scale(320);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (Theme::isColorSchemeChange(lParam) && theme.refresh()) {
            Theme::applyProcessMode(theme.dark());
            Theme::applyWindowFrame(hwnd_, theme.dark());
            applyTheme();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return 0;

    case WM_DPICHANGED: {
        UINT oldDpi = dpi_;
        dpi_ = HIWORD(wParam);
        if (dpi_ == 0) dpi_ = 96;
        rescaleGeometry(oldDpi, dpi_);
        createFont();
        applyRowHeight();
        for (int i = 0; i < kColumns; i++)
            SendMessageW(hList_, LVM_SETCOLUMNWIDTH, i, colWidth_[i]);
        if (lParam) {
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout();
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        FillRect(hdc, &rc, theme.backgroundBrush());
        paintSearchFrame(hdc);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, theme.dimText());
        SetBkColor(hdc, theme.background());
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(theme.backgroundBrush());
    }

    case WM_CTLCOLOREDIT: {
        // The search field sits inside a frame the parent fills with field(),
        // so it has to use the same colour or a seam shows around the text.
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, theme.text());
        SetBkColor(hdc, theme.field());
        return reinterpret_cast<LRESULT>(theme.fieldBrush());
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, theme.text());
        SetBkColor(hdc, theme.surface());
        return reinterpret_cast<LRESULT>(theme.surfaceBrush());
    }

    case WM_APP_LOAD_DONE:
        onLoadDone(lParam);
        return 0;

    case WM_CLOSE:
        saveSettings();
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        tree_.destroy();
        PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY: {
        LRESULT r = DefWindowProcW(hwnd_, msg, wParam, lParam);
        if (hFont_) { DeleteObject(hFont_); hFont_ = nullptr; }
        if (hRowSpacer_) { ImageList_Destroy(hRowSpacer_); hRowSpacer_ = nullptr; }
        return r;
    }

    default:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

//--------------------------------------------------------------------
// Children, fonts, theme, layout
//--------------------------------------------------------------------

void MainWindow::createFont() {
    HFONT old = hFont_;
    int height = -MulDiv(10, static_cast<int>(dpi_), 72);
    hFont_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    applyFonts();
    // Deleted only once the controls have taken the replacement.
    if (old) DeleteObject(old);
}

void MainWindow::createChildren() {
    // Borderless: the parent draws the frame, so the search box, the agent list
    // and the button all share one border colour instead of three system looks.
    hSearch_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH)),
        nullptr, nullptr);
    SendMessageW(hSearch_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Search title, directory or id..."));
    // Breathing room between the border and the caret.
    SendMessageW(hSearch_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(scale(8), scale(8)));

    hAgent_ = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AGENT)),
        nullptr, nullptr);

    hRefresh_ = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH)),
        nullptr, nullptr);

    // Bulk removal of the visible sessions whose transcript is gone. Disabled
    // until the current view actually contains some.
    hPrune_ = CreateWindowExW(0, L"BUTTON", L"Delete stale",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRUNE)),
        nullptr, nullptr);

    // The real Explorer navigation pane: drives, folders, shell icons, lazy
    // expansion - all handled by the shell rather than reimplemented here.
    RECT treeRc{0, 0, 0, 0};
    tree_.create(hwnd_, treeRc);
    tree_.onSelectionChanged = [this](const std::wstring& path) { onFolderSelected(path); };
    tree_.folderSessionCount = [this](const std::wstring& path) -> int {
        auto it = dirCounts_.find(path);
        return it != dirCounts_.end() ? it->second : 0;
    };
    tree_.onNewSessionRequested = [this](const std::string& agent, const std::wstring& directory) {
        startNewSessionInDirectory(agent, directory);
    };

    hList_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP
        | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
        nullptr, nullptr);

    SendMessageW(hList_, LVM_SETUNICODEFORMAT, TRUE, 0);
    // No grid lines: the Explorer theme carries hover and selection on its own,
    // and gridlines are what make a list view look like a 1998 dialog.
    SendMessageW(hList_, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    hHeader_ = reinterpret_cast<HWND>(SendMessageW(hList_, LVM_GETHEADER, 0, 0));
    applyRowHeight();

    hStatus_ = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
        nullptr, nullptr);

    refreshChrome_ = {this, false, false, false};
    pruneChrome_ = {this, false, false, false};
    agentChrome_ = {this, true, false, false};
    SetWindowSubclass(hRefresh_, &MainWindow::flatChromeProc, 1,
                      reinterpret_cast<DWORD_PTR>(&refreshChrome_));
    SetWindowSubclass(hPrune_, &MainWindow::flatChromeProc, 1,
                      reinterpret_cast<DWORD_PTR>(&pruneChrome_));
    SetWindowSubclass(hAgent_, &MainWindow::flatChromeProc, 1,
                      reinterpret_cast<DWORD_PTR>(&agentChrome_));

    applyFonts();
}

LRESULT CALLBACK MainWindow::flatChromeProc(HWND hwnd, UINT msg, WPARAM wParam,
                                            LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    auto* chrome = reinterpret_cast<FlatChrome*>(data);
    if (!chrome || !chrome->owner) return DefSubclassProc(hwnd, msg, wParam, lParam);

    auto repaint = [&] { InvalidateRect(hwnd, nullptr, FALSE); };

    switch (msg) {
    case WM_MOUSEMOVE:
        if (!chrome->hot) {
            chrome->hot = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            repaint();
        }
        break;

    case WM_MOUSELEAVE:
        chrome->hot = false;
        repaint();
        break;

    case WM_LBUTTONDOWN:
        chrome->pressed = true;
        repaint();
        break;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        chrome->pressed = false;
        repaint();
        break;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        repaint();
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        chrome->owner->paintFlatChrome(hwnd, *chrome);
        return 0;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, &MainWindow::flatChromeProc, id);
        break;

    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void MainWindow::paintFlatChrome(HWND hwnd, FlatChrome& chrome) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    Theme& theme = Theme::instance();

    bool enabled = IsWindowEnabled(hwnd) != FALSE;
    bool dropped = chrome.isCombo &&
                   SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;
    COLORREF fill = !enabled                    ? theme.background()
                  : (chrome.pressed || dropped) ? theme.fieldPressed()
                  : chrome.hot                  ? theme.fieldHover()
                                                : theme.field();

    HBRUSH bg = CreateSolidBrush(fill);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    HBRUSH frame = CreateSolidBrush(GetFocus() == hwnd ? theme.accent()
                                                       : theme.fieldBorder());
    FrameRect(hdc, &rc, frame);
    DeleteObject(frame);

    HGDIOBJ oldFont = SelectObject(hdc, hFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, enabled ? theme.text() : theme.dimText());

    wchar_t text[160]{};
    if (chrome.isCombo) {
        int sel = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && SendMessageW(hwnd, CB_GETLBTEXTLEN, sel, 0) < 160)
            SendMessageW(hwnd, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text));

        int chevronW = scale(22);
        RECT textRc{rc.left + scale(10), rc.top, rc.right - chevronW, rc.bottom};
        DrawTextW(hdc, text, -1, &textRc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

        // Chevron, drawn rather than taken from a font so it lines up at any DPI.
        int cx = rc.right - chevronW / 2 - scale(4);
        int cy = (rc.top + rc.bottom) / 2 - scale(1);
        int arm = scale(4);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(1)), theme.dimText());
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        POINT chevron[3] = {{cx - arm, cy - arm / 2},
                            {cx, cy + arm / 2},
                            {cx + arm, cy - arm / 2}};
        Polyline(hdc, chevron, 3);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    } else {
        GetWindowTextW(hwnd, text, ARRAYSIZE(text));
        DrawTextW(hdc, text, -1, &rc,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
}

void MainWindow::paintSearchFrame(HDC hdc) {
    if (IsRectEmpty(&searchFrame_)) return;
    Theme& theme = Theme::instance();

    HBRUSH fill = CreateSolidBrush(theme.field());
    FillRect(hdc, &searchFrame_, fill);
    DeleteObject(fill);

    // Same border as the two painted controls, so the row reads as one piece.
    HBRUSH frame = CreateSolidBrush(GetFocus() == hSearch_ ? theme.accent()
                                                           : theme.fieldBorder());
    FrameRect(hdc, &searchFrame_, frame);
    DeleteObject(frame);
}

void MainWindow::applyRowHeight() {
    // A list view takes its row height from the small image list; an empty
    // image list of the desired height is the standard way to add padding.
    if (hRowSpacer_) {
        SendMessageW(hList_, LVM_SETIMAGELIST, LVSIL_SMALL, 0);
        ImageList_Destroy(hRowSpacer_);
    }
    hRowSpacer_ = ImageList_Create(1, scale(26), ILC_COLOR32, 1, 1);
    SendMessageW(hList_, LVM_SETIMAGELIST, LVSIL_SMALL,
                 reinterpret_cast<LPARAM>(hRowSpacer_));
}

void MainWindow::applyTheme() {
    Theme& theme = Theme::instance();
    const wchar_t* explorer = theme.explorerThemeName();

    // "Explorer" / "DarkMode_Explorer" is what gives the list its hover
    // highlight, soft selection fill and thin scrollbars.
    SetWindowTheme(hList_, explorer, nullptr);
    if (hHeader_) SetWindowTheme(hHeader_, theme.itemsViewThemeName(), nullptr);
    // The button and the agent list draw their own chrome, so they need no
    // theme class; only their drop-down list is still system-drawn.
    SetWindowTheme(hAgent_, explorer, nullptr);
    tree_.applyTheme(explorer, theme.surface(), theme.text());

    ListView_SetBkColor(hList_, theme.surface());
    ListView_SetTextBkColor(hList_, CLR_NONE);
    ListView_SetTextColor(hList_, theme.text());

    if (tree_.handle()) InvalidateRect(tree_.handle(), nullptr, TRUE);
    InvalidateRect(hList_, nullptr, TRUE);
}

void MainWindow::applyFonts() {
    HWND children[] = {hSearch_, hAgent_, hRefresh_, hPrune_, hList_, hStatus_, tree_.handle()};
    for (HWND h : children) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hFont_), TRUE);
    }
}

void MainWindow::setupColumns() {
    if (columnsCreated_ || !hList_) return;
    columnsCreated_ = true;
    static const wchar_t* const headers[kColumns] = {
        L"Agent", L"Session ID", L"Title", L"Model", L"Size", L"Updated"};
    for (int i = 0; i < kColumns; i++) {
        LVCOLUMNW col{};
        col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
        col.fmt = (i == kSize) ? LVCFMT_RIGHT : LVCFMT_LEFT;  // sizes align on the unit
        col.cx = colWidth_[i];
        col.pszText = const_cast<LPWSTR>(headers[i]);
        SendMessageW(hList_, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&col));
    }
}

int MainWindow::textHeight() const {
    int height = scale(16);
    if (HDC dc = GetDC(hwnd_)) {
        HGDIOBJ old = SelectObject(dc, hFont_);
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) height = tm.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
    }
    return height;
}

int MainWindow::scale(int v) const {
    return static_cast<int>(std::lround(v * dpi_ / 96.0));
}

int MainWindow::unscale(int v) const {
    return static_cast<int>(std::lround(v * 96.0 / (dpi_ ? dpi_ : 96)));
}

void MainWindow::rescaleGeometry(UINT fromDpi, UINT toDpi) {
    if (fromDpi == 0 || toDpi == 0 || fromDpi == toDpi) return;
    double f = static_cast<double>(toDpi) / fromDpi;
    leftWidth_ = static_cast<int>(std::lround(leftWidth_ * f));
    for (int& w : colWidth_) w = static_cast<int>(std::lround(w * f));
}

void MainWindow::layout() {
    if (!hList_) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int cx = rc.right, cy = rc.bottom;
    // A minimized window reports a 0x0 client area; laying out against that
    // would clamp the splitter to its minimum and lose the user's position.
    if (cx <= 0 || cy <= 0) return;

    int m = scale(12);
    int gap = scale(8);
    int statusH = scale(20);

    int refreshW = scale(88);
    int pruneW = scale(108);
    int agentW = scale(150);

    int y = m;
    int searchW = cx - m * 2 - refreshW - pruneW - agentW - gap * 3;
    if (searchW < scale(120)) searchW = scale(120);

    // A drop-down list ignores the height it is given and shrinks itself to the
    // one it derives from its item height, which is why the three controls used
    // to end up different sizes. Size the list first, then let the edit and the
    // button adopt whatever height it settled on.
    int fieldH = textHeight() + scale(8);
    SendMessageW(hAgent_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), fieldH);
    SendMessageW(hAgent_, CB_SETITEMHEIGHT, 0, textHeight() + scale(6));
    MoveWindow(hAgent_, m + searchW + gap, y, agentW, fieldH + scale(220), TRUE);

    RECT agentRc{};
    GetWindowRect(hAgent_, &agentRc);
    int toolH = std::max(scale(20), static_cast<int>(agentRc.bottom - agentRc.top));

    MoveWindow(hPrune_, cx - m - pruneW, y, pruneW, toolH, TRUE);
    MoveWindow(hRefresh_, cx - m - pruneW - gap - refreshW, y, refreshW, toolH, TRUE);

    // The edit itself is borderless and sits inside a frame the parent paints,
    // which is what lets all three controls share one border.
    searchFrame_ = RECT{m, y, m + searchW, y + toolH};
    int inset = std::max(1, scale(1));
    MoveWindow(hSearch_, searchFrame_.left + inset + scale(7),
               searchFrame_.top + (toolH - textHeight()) / 2,
               searchW - inset * 2 - scale(14), textHeight(), TRUE);

    int top = y + toolH + gap + scale(4);
    int statusY = cy - m / 2 - statusH;
    int bottom = statusY - gap;

    int splitterW = scale(8);
    int minLeft = scale(140);
    int maxLeft = std::max(minLeft, cx - m * 2 - splitterW - scale(240));
    leftWidth_ = std::clamp(leftWidth_, minLeft, maxLeft);

    int treeX = m;
    int treeW = leftWidth_;
    int splitterX = treeX + treeW;
    int listX = splitterX + splitterW;
    int listW = std::max(scale(80), cx - m - listX);

    splitterX_ = splitterX;

    int contentH = std::max(scale(60), bottom - top);
    RECT treeRc{treeX, top, treeX + treeW, top + contentH};
    tree_.move(treeRc);
    MoveWindow(hList_, listX, top, listW, contentH, TRUE);
    MoveWindow(hStatus_, m, statusY, cx - m * 2, statusH, TRUE);

    // Title absorbs whatever the fixed-width columns leave over.
    int others = 0;
    for (int i = 0; i < kColumns; i++)
        if (i != kTitle) others += colWidth_[i];
    int titleW = std::max(scale(60),
                          listW - others - GetSystemMetrics(SM_CXVSCROLL) - scale(6));
    colWidth_[kTitle] = titleW;
    SendMessageW(hList_, LVM_SETCOLUMNWIDTH, kTitle, titleW);
}

//--------------------------------------------------------------------
// Commands / notifications
//--------------------------------------------------------------------

void MainWindow::onCommand(WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);
    switch (id) {
    case IDC_SEARCH:
        if (code == EN_CHANGE) applyFilter();
        // The frame is drawn by the parent, so it has to be repainted when the
        // field gains or loses focus.
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS)
            InvalidateRect(hwnd_, &searchFrame_, TRUE);
        break;
    case IDC_AGENT:
        if (code == CBN_SELCHANGE) applyFilter();
        break;
    case IDC_REFRESH:
        if (code == BN_CLICKED) loadSessionsAsync();
        break;
    case IDC_PRUNE:
        if (code == BN_CLICKED) deleteStaleSessions();
        break;
    }
}

void MainWindow::updatePruneButton() {
    bool anyStale = false;
    for (int index : filtered_) {
        if (!all_[index].resumable) { anyStale = true; break; }
    }
    if ((IsWindowEnabled(hPrune_) != FALSE) != anyStale) {
        EnableWindow(hPrune_, anyStale);
        InvalidateRect(hPrune_, nullptr, FALSE);
    }
}

void MainWindow::deleteStaleSessions() {
    // "Stale" is the same thing the list dims: a Claude session whose
    // transcript has been pruned. Scoped to the current view so the folder
    // tree and the search box narrow what gets cleaned up.
    std::vector<Session> stale;
    for (int index : filtered_) {
        const Session& s = all_[index];
        if (s.agent == "Claude" && !s.resumable) stale.push_back(s);
    }
    if (stale.empty()) return;

    std::wstring prompt =
        L"Delete " + std::to_wstring(stale.size()) + L" stale session" +
        (stale.size() == 1 ? L"" : L"s") + L" shown in the list?\n\n"
        L"These have no transcript left, so they cannot be resumed; only their "
        L"lines in history.jsonl remain. The file is rewritten once, with the "
        L"previous version kept as history.jsonl.bak.";
    if (MessageBoxW(hwnd_, prompt.c_str(), L"Delete stale sessions",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;

    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    long long freed = 0;
    std::wstring error;
    bool ok = SessionActions::deleteClaudeSessions(stale, freed, error);
    SetCursor(previous);

    if (!ok) {
        MessageBoxW(hwnd_, error.c_str(), L"Delete stale sessions", MB_ICONERROR | MB_OK);
        return;
    }

    std::wstring freedText = utf8to16(Session::formatSize(freed));
    if (freed <= 0) freedText = L"0 B";
    std::wstring done = L"Deleted " + std::to_wstring(stale.size()) + L" stale session" +
                        (stale.size() == 1 ? L"" : L"s") + L".\n\nFreed " + freedText +
                        L" in history.jsonl.";
    setStatus(L"Deleted " + std::to_wstring(stale.size()) + L" stale sessions, freed " + freedText);
    loadSessionsAsync();
    MessageBoxW(hwnd_, done.c_str(), L"Delete stale sessions", MB_ICONINFORMATION | MB_OK);
}

//--------------------------------------------------------------------
// Context menu
//--------------------------------------------------------------------

int MainWindow::rowUnderCursor(int screenX, int screenY) const {
    LVHITTESTINFO hit{};
    hit.pt = POINT{screenX, screenY};
    ScreenToClient(hList_, &hit.pt);
    int row = static_cast<int>(SendMessageW(hList_, LVM_HITTEST, 0,
                                            reinterpret_cast<LPARAM>(&hit)));
    return (row >= 0 && row < static_cast<int>(filtered_.size())) ? row : -1;
}

void MainWindow::showListMenu(int x, int y) {
    // Keyboard menu key: aim at the focused row and place the menu on it.
    if (x == -1 && y == -1) {
        int focused = static_cast<int>(
            SendMessageW(hList_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
        if (focused < 0) return;
        RECT itemRc{};
        itemRc.left = LVIR_BOUNDS;
        if (!SendMessageW(hList_, LVM_GETITEMRECT, static_cast<WPARAM>(focused),
                          reinterpret_cast<LPARAM>(&itemRc)))
            return;
        POINT pt{itemRc.left + scale(24), itemRc.bottom};
        ClientToScreen(hList_, &pt);
        x = pt.x;
        y = pt.y;
    }

    int row = rowUnderCursor(x, y);
    traceW(L"showListMenu at %d,%d -> row=%d foreground=%d", x, y, row,
           GetForegroundWindow() == hwnd_ ? 1 : 0);
    if (row < 0) return;

    // The list has already handled the right click the Explorer way: on an
    // unselected row it becomes the sole selection, on a selected one the
    // selection is kept. Either way the menu acts on the whole selection.
    std::vector<int> rows = selectedRows();
    if (rows.empty()) rows.push_back(row);
    size_t n = rows.size();
    std::wstring count = n > 1 ? L" (" + std::to_wstring(n) + L")" : L"";

    // Resume is offered only when at least one selected session still has
    // something to resume; it then applies to those alone.
    size_t resumable = 0;
    for (int r : rows) resumable += all_[filtered_[r]].resumable ? 1 : 0;
    std::wstring resumeCount = resumable > 1 ? L" (" + std::to_wstring(resumable) + L")" : L"";

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_COPY,
                (L"Copy session ID" + std::wstring(n > 1 ? L"s" : L"") + count + L"\tCtrl+C").c_str());
    AppendMenuW(menu, MF_STRING | (resumable ? MF_ENABLED : MF_GRAYED), IDM_RESUME,
                (resumable ? L"Resume in terminal" + resumeCount + L"\tCtrl+Enter"
                           : std::wstring(L"Resume in terminal — transcript is gone")).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_DELETE,
                (L"Delete session" + std::wstring(n > 1 ? L"s" : L"") + count + L"\tDel").c_str());

    int choice = static_cast<int>(TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
        x, y, hwnd_, nullptr));
    DestroyMenu(menu);

    if (choice == IDM_COPY) copySessionIds(rows);
    else if (choice == IDM_RESUME) resumeSessions(rows);
    else if (choice == IDM_DELETE) deleteSessions(rows);
}

std::vector<int> MainWindow::selectedRows() const {
    std::vector<int> rows;
    int row = -1;
    while ((row = static_cast<int>(SendMessageW(hList_, LVM_GETNEXTITEM,
                                                static_cast<WPARAM>(row),
                                                LVNI_SELECTED))) >= 0) {
        if (row < static_cast<int>(filtered_.size())) rows.push_back(row);
    }
    return rows;
}

void MainWindow::resumeSessions(const std::vector<int>& selected) {
    // Sessions with no transcript left are skipped rather than launched into
    // an error; the menu already greys them, this covers the shortcut.
    std::vector<int> rows;
    for (int row : selected)
        if (all_[filtered_[row]].resumable) rows.push_back(row);
    if (rows.empty()) {
        if (!selected.empty()) setStatus(L"Nothing to resume: the transcript is gone");
        return;
    }

    // Opening many terminals by accident is easy with Ctrl+A; ask past a few.
    if (rows.size() > 3) {
        std::wstring prompt = L"Open " + std::to_wstring(rows.size()) +
                              L" terminal tabs, one per selected session?";
        if (MessageBoxW(hwnd_, prompt.c_str(), L"Resume in terminal",
                        MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            return;
    }

    std::wstring errors;
    size_t started = 0;
    for (int row : rows) {
        std::wstring error;
        if (SessionActions::resumeInTerminal(all_[filtered_[row]], error)) ++started;
        else errors += views_[filtered_[row]].sessionId + L": " + error + L"\n";
    }

    if (started == 1 && rows.size() == 1)
        setStatus(L"Resuming " + views_[filtered_[rows[0]]].sessionId + L"...");
    else
        setStatus(L"Resuming " + std::to_wstring(started) + L" sessions...");
    if (!errors.empty())
        MessageBoxW(hwnd_, errors.c_str(), L"Resume in terminal", MB_ICONWARNING | MB_OK);
}

void MainWindow::startNewSessionInDirectory(const std::string& agent,
                                            const std::wstring& directory) {
    std::wstring error;
    if (SessionActions::startNewSession(agent, directory, error)) {
        setStatus(L"Starting a new " + utf8to16(agent) + L" session...");
        return;
    }
    MessageBoxW(hwnd_, error.c_str(), L"New session", MB_ICONWARNING | MB_OK);
}

void MainWindow::deleteSessions(const std::vector<int>& rows) {
    if (rows.empty()) return;

    std::vector<Session> claudeSessions, openCodeSessions;
    std::unordered_set<std::string> openCodeSelected;
    for (int row : rows) {
        const Session& s = all_[filtered_[row]];
        if (s.agent == "Claude") claudeSessions.push_back(s);
        else if (s.agent == "OpenCode") {
            openCodeSessions.push_back(s);
            openCodeSelected.insert(s.sessionId);
        }
    }

    // `opencode session delete` cascades to a session's own subagent children
    // on the vendor's side; this app lists those as ordinary separate rows,
    // so the selection alone understates what a delete here actually removes.
    // Walk out from the selection to warn about the real total before asking,
    // and keep the extras themselves - they get logged either way, since
    // opencode removes them silently and this is the only record of why.
    std::unordered_set<std::string> cascade = openCodeSelected;
    std::vector<Session> cascadeExtras;
    for (bool changed = true; changed;) {
        changed = false;
        for (const Session& s : all_) {
            if (s.agent != "OpenCode" || s.parentId.empty()) continue;
            if (cascade.count(s.parentId) && cascade.insert(s.sessionId).second) {
                cascadeExtras.push_back(s);
                changed = true;
            }
        }
    }

    std::wstring prompt;
    if (rows.size() == 1) {
        const SessionView& view = views_[filtered_[rows[0]]];
        prompt = L"Delete this session?\n\n" + view.title + L"\n" + view.sessionId + L"\n\n";
    } else {
        prompt = L"Delete " + std::to_wstring(rows.size()) + L" sessions?\n\n";
        size_t shown = 0;
        for (int row : rows) {
            if (shown++ == 8) { prompt += L"...\n"; break; }
            prompt += L"• " + views_[filtered_[row]].title + L"\n";
        }
        prompt += L"\n";
    }
    if (!claudeSessions.empty())
        prompt += L"Claude: the transcript goes to the Recycle Bin and the entry is "
                  L"removed from history.jsonl, which is backed up as history.jsonl.bak.\n";
    if (!openCodeSessions.empty()) {
        prompt += L"OpenCode: this runs `opencode session delete`. opencode.db is backed "
                  L"up first and restored automatically if anything fails partway "
                  L"through.\n";
        if (!cascadeExtras.empty())
            prompt += L"This also spawned " + std::to_wstring(cascadeExtras.size()) +
                      L" subagent session" + (cascadeExtras.size() == 1 ? L"" : L"s") +
                      L" not listed above, which opencode will delete along with "
                      L"their parent.\n";
    }

    if (MessageBoxW(hwnd_, prompt.c_str(), L"Delete session",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;

    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring errors;
    size_t deleted = 0;
    if (!claudeSessions.empty()) {
        long long freed = 0;
        std::wstring error;
        if (SessionActions::deleteClaudeSessions(claudeSessions, freed, error))
            deleted += claudeSessions.size();
        else
            errors += error + L"\n";
    }
    if (!openCodeSessions.empty()) {
        std::wstring error;
        bool ok = SessionActions::deleteOpenCodeSessions(openCodeSessions, error);
        if (ok) {
            deleted += openCodeSessions.size();
            // These were never passed to deleteOpenCodeSessions - opencode
            // removed them on its own as a side effect of their parent's
            // delete - so this app has to log them itself.
            for (const Session& s : cascadeExtras)
                SessionActions::logDeletion(
                    s, "deleted along with parent (subagent cascade, not requested directly)");
        } else {
            errors += error + L"\n";
        }
    }
    SetCursor(previous);

    if (!errors.empty())
        MessageBoxW(hwnd_, errors.c_str(), L"Delete session", MB_ICONERROR | MB_OK);
    if (deleted == 1 && rows.size() == 1)
        setStatus(L"Deleted " + views_[filtered_[rows[0]]].sessionId);
    else
        setStatus(L"Deleted " + std::to_wstring(deleted) + L" sessions");
    if (deleted) loadSessionsAsync();
}

//--------------------------------------------------------------------
// Sorting
//--------------------------------------------------------------------

const char* MainWindow::columnName(int column) {
    static const char* const names[kColumns] = {
        "agent", "sessionId", "title", "model", "size", "updated"};
    return (column >= 0 && column < kColumns) ? names[column] : "";
}

int MainWindow::columnByName(const std::string& name) {
    for (int i = 0; i < kColumns; i++)
        if (name == columnName(i)) return i;
    return -1;
}

void MainWindow::onColumnClick(int column) {
    if (column < 0 || column >= kColumns) return;
    if (column == sortColumn_) {
        sortDescending_ = !sortDescending_;
    } else {
        sortColumn_ = column;
        // Dates and sizes read naturally largest-first; text columns A to Z.
        sortDescending_ = (column == kUpdated || column == kSize);
    }
    sortFiltered();
    fillList();
    showSortIndicator();
    saveSettings();
}

void MainWindow::sortFiltered() {
    auto text = [this](int index) -> const std::wstring& {
        const SessionView& v = views_[index];
        switch (sortColumn_) {
        case kAgent: return v.agent;
        case kSessionId: return v.sessionId;
        case kTitle: return v.title;
        case kModel: return v.model;
        case kSize: return v.size;
        default: return v.updated;
        }
    };

    auto less = [&](int a, int b) {
        int order;
        if (sortColumn_ == kUpdated || sortColumn_ == kSize) {
            // Numeric columns compare the underlying value, not the label.
            long long va = sortColumn_ == kUpdated ? all_[a].updatedMs : all_[a].sizeBytes;
            long long vb = sortColumn_ == kUpdated ? all_[b].updatedMs : all_[b].sizeBytes;
            order = (va < vb) ? -1 : (va > vb) ? 1 : 0;
        } else {
            const std::wstring& sa = text(a);
            const std::wstring& sb = text(b);
            order = CompareStringOrdinal(sa.c_str(), static_cast<int>(sa.size()),
                                         sb.c_str(), static_cast<int>(sb.size()), TRUE)
                    - CSTR_EQUAL;
        }
        return sortDescending_ ? order > 0 : order < 0;
    };

    // Stable, so rows that compare equal keep their newest-first order.
    std::stable_sort(filtered_.begin(), filtered_.end(), less);
}

void MainWindow::showSortIndicator() {
    if (!hHeader_) return;
    for (int i = 0; i < kColumns; i++) {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        if (!SendMessageW(hHeader_, HDM_GETITEMW, i, reinterpret_cast<LPARAM>(&item))) continue;
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortColumn_) item.fmt |= sortDescending_ ? HDF_SORTDOWN : HDF_SORTUP;
        SendMessageW(hHeader_, HDM_SETITEMW, i, reinterpret_cast<LPARAM>(&item));
    }
}

void MainWindow::onFolderSelected(const std::wstring& path) {
    // An empty path means a virtual node such as "This PC": show everything.
    selectedDir_ = lowerW(path);
    // A drive root arrives as "E:\"; the trailing separator would make the
    // component-boundary test in isPathPrefixW reject every child path.
    while (selectedDir_.size() > 1 && selectedDir_.back() == L'\\')
        selectedDir_.pop_back();
    traceW(L"onFolderSelected '%s'", selectedDir_.c_str());
    applyFilter();
}

LRESULT MainWindow::onNotify(LPARAM lParam) {
    const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lParam);
    if (!hdr) return 0;

    if (hdr->hwndFrom == hList_) {
        if (hdr->code == LVN_KEYDOWN) {
            auto key = reinterpret_cast<const NMLVKEYDOWN*>(lParam);
            if (key->wVKey == VK_DELETE) deleteSessions(selectedRows());
            else if (key->wVKey == 'C' && GetKeyState(VK_CONTROL) < 0)
                copySessionIds(selectedRows());
            else if (key->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) {
                LVITEMW all{};
                all.state = LVIS_SELECTED;
                all.stateMask = LVIS_SELECTED;
                SendMessageW(hList_, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                             reinterpret_cast<LPARAM>(&all));
            }
            return 0;
        }
        if (hdr->code == LVN_COLUMNCLICK) {
            onColumnClick(reinterpret_cast<const NMLISTVIEW*>(lParam)->iSubItem);
            return 0;
        }
        if (hdr->code == NM_CUSTOMDRAW) return onListCustomDraw(lParam);
    }

    // Header notifications carry the header's own control id, not the list's,
    // so they have to be matched on hwndFrom.
    if (hHeader_ && hdr->hwndFrom == hHeader_ &&
        (hdr->code == HDN_ENDTRACKW || hdr->code == HDN_ENDTRACKA ||
         hdr->code == HDN_DIVIDERDBLCLICKW || hdr->code == HDN_DIVIDERDBLCLICKA)) {
        readColumnWidths();
        saveSettings();
        layout();
        return 0;
    }

    return 0;
}

LRESULT MainWindow::onListCustomDraw(LPARAM lParam) {
    auto cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT: {
        // Zebra striping, kept subtle so it reads as texture rather than lines.
        Theme& theme = Theme::instance();
        bool odd = (cd->nmcd.dwItemSpec % 2) != 0;
        cd->clrTextBk = odd ? (theme.dark() ? RGB(0x31, 0x31, 0x31)
                                            : RGB(0xf7, 0xf7, 0xf9))
                            : theme.surface();

        // A session that cannot be resumed is dimmed so the unavailability
        // is visible before anyone reaches for the menu.
        int row = static_cast<int>(cd->nmcd.dwItemSpec);
        bool resumable = row >= 0 && row < static_cast<int>(filtered_.size()) &&
                         all_[filtered_[row]].resumable;
        cd->clrText = resumable ? theme.text() : theme.dimText();
        return CDRF_DODEFAULT;
    }

    default:
        return CDRF_DODEFAULT;
    }
}

//--------------------------------------------------------------------
// Splitter
//--------------------------------------------------------------------

void MainWindow::onSplitterDown(LPARAM lParam) {
    int x = GET_X_LPARAM(lParam);
    if (x >= splitterX_ - scale(3) && x <= splitterX_ + scale(8) + scale(3)) {
        dragSplitter_ = true;
        SetCapture(hwnd_);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
    }
}

void MainWindow::onSplitterMove(LPARAM lParam) {
    if (!dragSplitter_) {
        if (cursorOverSplitter())
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return;
    }
    int x = GET_X_LPARAM(lParam);
    leftWidth_ = std::max(scale(140), x - scale(12));
    layout();
}

void MainWindow::onSplitterUp() {
    if (!dragSplitter_) return;
    dragSplitter_ = false;
    ReleaseCapture();
    saveSettings();
}

bool MainWindow::cursorOverSplitter() const {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    return pt.x >= splitterX_ - scale(3) && pt.x <= splitterX_ + scale(8) + scale(3);
}

//--------------------------------------------------------------------
// Loading
//--------------------------------------------------------------------

DWORD WINAPI MainWindow::loadThread(LPVOID param) {
    HWND hwnd = static_cast<HWND>(param);
    auto* list = new std::vector<Session>(SessionLoader::loadAll());
    if (!PostMessageW(hwnd, WM_APP_LOAD_DONE, 0, reinterpret_cast<LPARAM>(list)))
        delete list;  // the window went away while we were loading
    return 0;
}

void MainWindow::loadSessionsAsync() {
    if (loading_) return;
    loading_ = true;
    setStatus(L"Loading...");
    HANDLE th = CreateThread(nullptr, 0, &MainWindow::loadThread, hwnd_, 0, nullptr);
    if (th) CloseHandle(th);
    else loading_ = false;
}

void MainWindow::onLoadDone(LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<Session>*>(lParam);
    loading_ = false;
    if (!list) return;

    all_ = std::move(*list);
    delete list;
    bool firstLoad = !loaded_;
    loaded_ = true;

    buildViews();

    if (firstLoad) selectedDir_.clear();

    agents_.clear();
    for (const auto& v : views_) {
        if (std::find(agents_.begin(), agents_.end(), v.agent) == agents_.end())
            agents_.push_back(v.agent);
    }
    std::sort(agents_.begin(), agents_.end());

    // Keep the agent selection across a Refresh.
    std::wstring prevAgent;
    int prevSel = static_cast<int>(SendMessageW(hAgent_, CB_GETCURSEL, 0, 0));
    if (prevSel > 0) {
        int n = static_cast<int>(SendMessageW(hAgent_, CB_GETLBTEXTLEN, prevSel, 0));
        if (n > 0) {
            prevAgent.resize(static_cast<size_t>(n) + 1);
            int got = static_cast<int>(SendMessageW(hAgent_, CB_GETLBTEXT, prevSel,
                                       reinterpret_cast<LPARAM>(prevAgent.data())));
            prevAgent.resize(got > 0 ? got : 0);
        }
    }

    SendMessageW(hAgent_, CB_RESETCONTENT, 0, 0);
    SendMessageW(hAgent_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All agents"));
    int restore = 0;
    for (size_t i = 0; i < agents_.size(); i++) {
        SendMessageW(hAgent_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(agents_[i].c_str()));
        if (!prevAgent.empty() && agents_[i] == prevAgent)
            restore = static_cast<int>(i) + 1;
    }
    SendMessageW(hAgent_, CB_SETCURSEL, restore, 0);

    applyFilter();
    tree_.invalidate();  // dirCounts_ just changed under buildViews()
}

void MainWindow::buildViews() {
    views_.clear();
    views_.reserve(all_.size());
    for (const auto& s : all_) {
        SessionView v;
        v.agent = utf8to16(s.agent);
        v.sessionId = utf8to16(s.sessionId);
        v.directory = utf8to16(s.directory);
        v.title = utf8to16(s.title);
        v.model = utf8to16(s.model);
        v.updated = utf8to16(s.updatedStr());
        v.size = utf8to16(s.sizeStr());
        v.lcDirectory = lowerW(v.directory);
        v.lcSearch = lowerW(v.title) + L'\n' + v.lcDirectory + L'\n' + lowerW(v.sessionId);
        views_.push_back(std::move(v));
    }
    buildDirCounts();
}

void MainWindow::buildDirCounts() {
    // Every ancestor of a session's directory gets +1: "D:\a\b" contributes to
    // "d:", "d:\a" and "d:\a\b" alike, so a count already means "at or under
    // this folder" with no extra summing needed when the tree asks about it.
    dirCounts_.clear();
    for (const auto& v : views_) {
        if (v.lcDirectory.empty()) continue;
        std::wstring prefix;
        for (const auto& part : splitW(v.lcDirectory, L'\\')) {
            prefix = prefix.empty() ? part : prefix + L'\\' + part;
            ++dirCounts_[prefix];
        }
    }
}

//--------------------------------------------------------------------
// Filter & fill
//--------------------------------------------------------------------

void MainWindow::applyFilter() {
    if (!loaded_) return;

    std::wstring text = lowerW(getSearchText());

    std::wstring agentSel;
    int sel = static_cast<int>(SendMessageW(hAgent_, CB_GETCURSEL, 0, 0));
    if (sel > 0 && sel - 1 < static_cast<int>(agents_.size()))
        agentSel = agents_[sel - 1];

    filtered_.clear();
    for (int i = 0; i < static_cast<int>(views_.size()); i++) {
        const SessionView& v = views_[i];
        if (!agentSel.empty() && v.agent != agentSel) continue;
        if (!selectedDir_.empty() && !isPathPrefixW(v.lcDirectory, selectedDir_)) continue;
        if (!text.empty() && v.lcSearch.find(text) == std::wstring::npos) continue;
        filtered_.push_back(i);
    }

    sortFiltered();
    fillList();
    showSortIndicator();
    updatePruneButton();
    setStatus(std::to_wstring(filtered_.size()) + L" / " +
              std::to_wstring(views_.size()) + L" sessions");
}

void MainWindow::fillList() {
    // Rebuilding wipes the selection, so carry it over by session id: a re-sort
    // or a narrower filter should not make the user pick their rows again.
    std::vector<std::wstring> keep;
    for (int row : selectedRows()) keep.push_back(views_[filtered_[row]].sessionId);

    // Filling row by row with painting enabled is what makes a few hundred
    // sessions feel slow, so suppress redraw for the whole batch.
    SendMessageW(hList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hList_, LVM_DELETEALLITEMS, 0, 0);
    SendMessageW(hList_, LVM_SETITEMCOUNT, filtered_.size(), 0);

    for (int row = 0; row < static_cast<int>(filtered_.size()); row++) {
        const SessionView& v = views_[filtered_[row]];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_STATE;
        item.iItem = row;
        item.pszText = const_cast<LPWSTR>(v.agent.c_str());
        item.stateMask = LVIS_SELECTED;
        if (std::find(keep.begin(), keep.end(), v.sessionId) != keep.end())
            item.state = LVIS_SELECTED;
        int inserted = static_cast<int>(SendMessageW(hList_, LVM_INSERTITEMW, 0,
                                                     reinterpret_cast<LPARAM>(&item)));
        if (inserted < 0) continue;
        setSubItem(inserted, kSessionId, v.sessionId);
        setSubItem(inserted, kTitle, v.title);
        setSubItem(inserted, kModel, v.model);
        setSubItem(inserted, kSize, v.size);
        setSubItem(inserted, kUpdated, v.updated);
    }

    SendMessageW(hList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList_, nullptr, TRUE);
}

void MainWindow::setSubItem(int row, int col, const std::wstring& text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = col;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(hList_, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
}

std::wstring MainWindow::getSearchText() const {
    int n = GetWindowTextLengthW(hSearch_);
    if (n <= 0) return {};
    std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
    int got = GetWindowTextW(hSearch_, buf.data(), n + 1);
    buf.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return buf;
}

//--------------------------------------------------------------------
// Clipboard, status, settings
//--------------------------------------------------------------------

void MainWindow::copySessionIds(const std::vector<int>& rows) {
    if (rows.empty()) return;

    // One id per line, so a multi-selection pastes as a usable list.
    std::wstring text;
    for (int row : rows) {
        if (!text.empty()) text += L"\r\n";
        text += views_[filtered_[row]].sessionId;
    }

    bool copied = false;
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(hMem)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(hMem);
                // Ownership passes to the clipboard only on success.
                if (SetClipboardData(CF_UNICODETEXT, hMem)) copied = true;
                else GlobalFree(hMem);
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    }

    // Confirmed in the status line: a pop-up would be out of proportion for a
    // menu command whose effect the user asked for explicitly.
    if (!copied) setStatus(L"Could not copy to the clipboard");
    else if (rows.size() == 1) setStatus(L"Copied " + text);
    else setStatus(L"Copied " + std::to_wstring(rows.size()) + L" session IDs");
}

void MainWindow::setStatus(const std::wstring& text) {
    if (hStatus_) SetWindowTextW(hStatus_, text.c_str());
}

void MainWindow::readColumnWidths() {
    for (int i = 0; i < kColumns; i++) {
        if (i == kTitle) continue;  // derived from the leftover width
        colWidth_[i] = static_cast<int>(SendMessageW(hList_, LVM_GETCOLUMNWIDTH, i, 0));
    }
}

void MainWindow::saveSettings() {
    // Persisted in 96-dpi units so the layout carries over between monitors.
    settings_.leftWidth = unscale(leftWidth_);
    settings_.agentWidth = unscale(colWidth_[kAgent]);
    settings_.sessionIdWidth = unscale(colWidth_[kSessionId]);
    settings_.modelWidth = unscale(colWidth_[kModel]);
    settings_.sizeWidth = unscale(colWidth_[kSize]);
    settings_.updatedWidth = unscale(colWidth_[kUpdated]);
    settings_.sortBy = columnName(sortColumn_);
    settings_.sortDescending = sortDescending_;
    settings_.save();
}
