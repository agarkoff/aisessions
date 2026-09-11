#include "MainWindow.h"
#include "ToastWindow.h"
#include "Theme.h"
#include "StrUtil.h"
#include "core/SessionLoader.h"
#include "resource.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>

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
    colWidth_[0] = settings.col0;
    colWidth_[1] = settings.col1;
    colWidth_[3] = settings.col3;
    colWidth_[4] = settings.col4;

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
        for (int i = 0; i < 5; i++)
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
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, theme.backgroundBrush());
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, theme.dimText());
        SetBkColor(hdc, theme.background());
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(theme.backgroundBrush());
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, theme.text());
        SetBkColor(hdc, theme.surface());
        return reinterpret_cast<LRESULT>(theme.surfaceBrush());
    }

    case WM_APP_LOAD_DONE:
        onLoadDone(lParam);
        return 0;

    case WM_APP_DESELECT:
        clearSelection(static_cast<int>(wParam));
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
    hSearch_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
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

    // The real Explorer navigation pane: drives, folders, shell icons, lazy
    // expansion - all handled by the shell rather than reimplemented here.
    RECT treeRc{0, 0, 0, 0};
    tree_.create(hwnd_, treeRc);
    tree_.onSelectionChanged = [this](const std::wstring& path) { onFolderSelected(path); };

    hList_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP
        | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
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

    applyFonts();
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
    SetWindowTheme(hRefresh_, explorer, nullptr);
    SetWindowTheme(hAgent_, theme.dark() ? L"DarkMode_CFD" : L"CFD", nullptr);
    SetWindowTheme(hSearch_, theme.dark() ? L"DarkMode_CFD" : L"CFD", nullptr);
    tree_.applyTheme(explorer, theme.surface(), theme.text());

    ListView_SetBkColor(hList_, theme.surface());
    ListView_SetTextBkColor(hList_, CLR_NONE);
    ListView_SetTextColor(hList_, theme.text());

    if (tree_.handle()) InvalidateRect(tree_.handle(), nullptr, TRUE);
    InvalidateRect(hList_, nullptr, TRUE);
}

void MainWindow::applyFonts() {
    HWND children[] = {hSearch_, hAgent_, hRefresh_, hList_, hStatus_, tree_.handle()};
    for (HWND h : children) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hFont_), TRUE);
    }
}

void MainWindow::setupColumns() {
    if (columnsCreated_ || !hList_) return;
    columnsCreated_ = true;
    const wchar_t* headers[] = {L"Agent", L"Session ID", L"Title", L"Model", L"Updated"};
    for (int i = 0; i < 5; i++) {
        LVCOLUMNW col{};
        col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
        col.fmt = LVCFMT_LEFT;
        col.cx = colWidth_[i];
        col.pszText = const_cast<LPWSTR>(headers[i]);
        SendMessageW(hList_, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&col));
    }
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
    int toolH = scale(30);
    int statusH = scale(20);

    int refreshW = scale(88);
    int agentW = scale(150);

    int y = m;
    int searchW = cx - m * 2 - refreshW - agentW - gap * 2;
    if (searchW < scale(120)) searchW = scale(120);

    MoveWindow(hSearch_, m, y, searchW, toolH, TRUE);
    // A drop-down list draws its closed state at the control height but sizes
    // the popup from the window height, so it needs room for both.
    MoveWindow(hAgent_, m + searchW + gap, y, agentW, toolH + scale(200), TRUE);
    MoveWindow(hRefresh_, cx - m - refreshW, y, refreshW, toolH, TRUE);

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
    int others = colWidth_[0] + colWidth_[1] + colWidth_[3] + colWidth_[4];
    int titleW = std::max(scale(60),
                          listW - others - GetSystemMetrics(SM_CXVSCROLL) - scale(6));
    colWidth_[2] = titleW;
    SendMessageW(hList_, LVM_SETCOLUMNWIDTH, 2, titleW);
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
        break;
    case IDC_AGENT:
        if (code == CBN_SELCHANGE) applyFilter();
        break;
    case IDC_REFRESH:
        if (code == BN_CLICKED) loadSessionsAsync();
        break;
    }
}

void MainWindow::onFolderSelected(const std::wstring& path) {
    // An empty path means a virtual node such as "This PC": show everything.
    selectedDir_ = lowerW(path);
    // A drive root arrives as "E:\"; the trailing separator would make the
    // component-boundary test in isPathPrefixW reject every child path.
    while (selectedDir_.size() > 1 && selectedDir_.back() == L'\\')
        selectedDir_.pop_back();
    applyFilter();
}

LRESULT MainWindow::onNotify(LPARAM lParam) {
    const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lParam);
    if (!hdr) return 0;

    if (hdr->hwndFrom == hList_) {
        if (hdr->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW* nm = reinterpret_cast<const NMLISTVIEW*>(lParam);
            bool becameSelected = (nm->uNewState & LVIS_SELECTED) &&
                                  !(nm->uOldState & LVIS_SELECTED);
            if (becameSelected && nm->iItem >= 0 &&
                nm->iItem < static_cast<int>(filtered_.size())) {
                copySessionId(views_[filtered_[nm->iItem]].sessionId);
                // Deselecting from inside the notification would re-enter the
                // list view while it is still processing the click, so defer it.
                PostMessageW(hwnd_, WM_APP_DESELECT, static_cast<WPARAM>(nm->iItem), 0);
            }
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
        cd->clrText = theme.text();
        return CDRF_DODEFAULT;
    }

    default:
        return CDRF_DODEFAULT;
    }
}

void MainWindow::clearSelection(int index) {
    if (!hList_) return;
    LVITEMW item{};
    item.stateMask = LVIS_SELECTED;
    SendMessageW(hList_, LVM_SETITEMSTATE, static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&item));
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
        v.lcDirectory = lowerW(v.directory);
        v.lcSearch = lowerW(v.title) + L'\n' + v.lcDirectory + L'\n' + lowerW(v.sessionId);
        views_.push_back(std::move(v));
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

    fillList();
    setStatus(std::to_wstring(filtered_.size()) + L" / " +
              std::to_wstring(views_.size()) + L" sessions");
}

void MainWindow::fillList() {
    // Filling row by row with painting enabled is what makes a few hundred
    // sessions feel slow, so suppress redraw for the whole batch.
    SendMessageW(hList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hList_, LVM_DELETEALLITEMS, 0, 0);
    SendMessageW(hList_, LVM_SETITEMCOUNT, filtered_.size(), 0);

    for (int row = 0; row < static_cast<int>(filtered_.size()); row++) {
        const SessionView& v = views_[filtered_[row]];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<LPWSTR>(v.agent.c_str());
        int inserted = static_cast<int>(SendMessageW(hList_, LVM_INSERTITEMW, 0,
                                                     reinterpret_cast<LPARAM>(&item)));
        if (inserted < 0) continue;
        setSubItem(inserted, 1, v.sessionId);
        setSubItem(inserted, 2, v.title);
        setSubItem(inserted, 3, v.model);
        setSubItem(inserted, 4, v.updated);
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

void MainWindow::copySessionId(const std::wstring& sessionId) {
    if (sessionId.empty()) return;

    bool copied = false;
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        size_t bytes = (sessionId.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(hMem)) {
                memcpy(dst, sessionId.c_str(), bytes);
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

    ToastWindow::instance().show(this,
        copied ? L"Copied  " + sessionId : L"Could not copy to clipboard");
}

void MainWindow::setStatus(const std::wstring& text) {
    if (hStatus_) SetWindowTextW(hStatus_, text.c_str());
}

void MainWindow::readColumnWidths() {
    colWidth_[0] = static_cast<int>(SendMessageW(hList_, LVM_GETCOLUMNWIDTH, 0, 0));
    colWidth_[1] = static_cast<int>(SendMessageW(hList_, LVM_GETCOLUMNWIDTH, 1, 0));
    colWidth_[3] = static_cast<int>(SendMessageW(hList_, LVM_GETCOLUMNWIDTH, 3, 0));
    colWidth_[4] = static_cast<int>(SendMessageW(hList_, LVM_GETCOLUMNWIDTH, 4, 0));
}

void MainWindow::saveSettings() {
    // Persisted in 96-dpi units so the layout carries over between monitors.
    settings_.leftWidth = unscale(leftWidth_);
    settings_.col0 = unscale(colWidth_[0]);
    settings_.col1 = unscale(colWidth_[1]);
    settings_.col3 = unscale(colWidth_[3]);
    settings_.col4 = unscale(colWidth_[4]);
    settings_.save();
}
