// Development probe for driving the running AI Sessions window from outside.
//
// Replaces the earlier C#/PowerShell harness. It deliberately never calls
// SetForegroundWindow: raising the window while someone is working is
// disruptive, so every action goes through window messages, and screenshots
// use PrintWindow, which renders an unfocused (even obscured) window.
//
// Usage:
//   probe list                    enumerate the window and its controls
//   probe count                   list view item count
//   probe status                  status line text
//   probe clip [set <text>]       read (or overwrite) the clipboard
//   probe search <text>           type into the search box
//   probe agent <index>           pick an entry in the agent combo
//   probe refresh                 press the Refresh button
//   probe click <class> <x> <y>   click inside a child control
//   probe shot <file.png>         capture the window

#include <windows.h>
#include <objidl.h>  // IStream / PROPID, which WIN32_LEAN_AND_MEAN would omit

#include <algorithm>
// GDI+ headers use unqualified min/max, which NOMINMAX takes away.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <commctrl.h>
#include <shlwapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMainClass[] = L"AISessions_MainWindow";

struct Child {
    HWND hwnd;
    std::wstring cls;
    RECT rc;
    LONG_PTR style;
};

std::wstring classOf(HWND h) {
    wchar_t buf[128]{};
    GetClassNameW(h, buf, 128);
    return buf;
}

std::wstring textOf(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    int got = GetWindowTextW(h, s.data(), n + 1);
    s.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return s;
}

struct FindContext {
    DWORD wantPid;  // 0 = any
    HWND found;
};

BOOL CALLBACK collectTopLevel(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindContext*>(lp);
    if (classOf(hwnd) != kMainClass) return TRUE;
    if (ctx->wantPid) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->wantPid) return TRUE;
    }
    ctx->found = hwnd;
    return FALSE;
}

// A specific pid can be requested so a test run drives its own instance
// instead of whichever window the user happens to have open.
HWND findMain(DWORD pid) {
    FindContext ctx{pid, nullptr};
    EnumWindows(&collectTopLevel, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

BOOL CALLBACK collectChild(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<Child>*>(lp);
    Child c{hwnd, classOf(hwnd), {}, GetWindowLongPtrW(hwnd, GWL_STYLE)};
    GetWindowRect(hwnd, &c.rc);
    out->push_back(std::move(c));
    return TRUE;
}

std::vector<Child> children(HWND main) {
    std::vector<Child> out;
    EnumChildWindows(main, &collectChild, reinterpret_cast<LPARAM>(&out));
    return out;
}

int controlId(HWND h) {
    return static_cast<int>(GetWindowLongPtrW(h, GWLP_ID));
}

// Matches on a class-name substring so "List" finds SysListView32 and the
// namespace tree's inner control can be reached by "Tree".
HWND childByClass(HWND main, const std::wstring& needle) {
    for (const Child& c : children(main)) {
        if (StrStrIW(c.cls.c_str(), needle.c_str())) return c.hwnd;
    }
    return nullptr;
}

// The shell tree brings its own Static and Edit children, so controls the app
// owns must be found by id rather than by class.
HWND childById(HWND main, int id) {
    for (const Child& c : children(main)) {
        if (controlId(c.hwnd) == id) return c.hwnd;
    }
    return nullptr;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring readClipboard() {
    if (!OpenClipboard(nullptr)) return L"<open failed>";
    std::wstring out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    } else {
        out = L"<empty>";
    }
    CloseClipboard();
    return out;
}

void writeClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (!text.empty()) {
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(mem)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            } else {
                GlobalFree(mem);
            }
        }
    }
    CloseClipboard();
}

// WM_LBUTTONDOWN/UP carry packed coordinates rather than pointers, so unlike
// LVM_/TVM_ messages they are safe to hand to another process.
//
// They must be posted, not sent: a tree view's button-down handler runs a
// nested DragDetect loop that waits for the button-up, so a blocking
// SendMessage deadlocks until the message times out.
void msgClick(HWND target, int x, int y) {
    LPARAM lp = MAKELPARAM(x, y);
    PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(target, WM_LBUTTONUP, 0, lp);
}

// Some shell controls only raise their COM events for genuine input, so a
// posted WM_LBUTTONDOWN cannot exercise them. This drives the real pointer,
// then puts it back where the user left it.
// Without the foreground window a synthetic click lands in whatever is on top
// instead, which would otherwise look like the app ignoring it.
bool bringForward(HWND main) {
    DWORD dummy = 0;
    DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), &dummy);
    DWORD self = GetCurrentThreadId();
    AttachThreadInput(self, foreground, TRUE);
    SetForegroundWindow(main);
    AttachThreadInput(self, foreground, FALSE);
    Sleep(250);

    if (GetForegroundWindow() != main) {
        fwprintf(stderr, L"ERROR: could not bring the window forward; "
                         L"input would have gone elsewhere\n");
        return false;
    }
    return true;
}

bool realClick(HWND main, HWND target, int x, int y, WORD modifier = 0) {
    POINT restore{};
    GetCursorPos(&restore);
    if (!bringForward(main)) return false;

    // Held for the whole click so Ctrl/Shift-click selection semantics apply.
    if (modifier) keybd_event(static_cast<BYTE>(modifier), 0, 0, 0);

    POINT pt{x, y};
    ClientToScreen(target, &pt);
    SetCursorPos(pt.x, pt.y);
    Sleep(120);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(60);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    if (modifier) keybd_event(static_cast<BYTE>(modifier), 0, KEYEVENTF_KEYUP, 0);
    Sleep(350);

    SetCursorPos(restore.x, restore.y);
    return true;
}

bool realRightClick(HWND main, HWND target, int x, int y) {
    POINT restore{};
    GetCursorPos(&restore);
    if (!bringForward(main)) return false;

    POINT pt{x, y};
    ClientToScreen(target, &pt);
    SetCursorPos(pt.x, pt.y);
    Sleep(120);
    mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
    Sleep(60);
    mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
    Sleep(350);

    SetCursorPos(restore.x, restore.y);
    return true;
}

WORD virtualKey(const std::wstring& name) {
    if (name == L"down") return VK_DOWN;
    if (name == L"up") return VK_UP;
    if (name == L"left") return VK_LEFT;
    if (name == L"right") return VK_RIGHT;
    if (name == L"enter") return VK_RETURN;
    if (name == L"esc") return VK_ESCAPE;
    if (name == L"del") return VK_DELETE;
    return 0;
}

// Keystrokes have to be real: a pop-up menu and a message box each run their
// own modal loop and read the input queue, not the window's message queue.
void sendKeys(int count, wchar_t** names) {
    for (int i = 0; i < count; i++) {
        std::wstring name = names[i];
        // "ctrl+x" holds Control around a single letter or named key.
        bool ctrl = name.rfind(L"ctrl+", 0) == 0;
        if (ctrl) name = name.substr(5);

        WORD vk = virtualKey(name);
        if (!vk && name.size() == 1 && iswalnum(name[0]))
            vk = static_cast<WORD>(towupper(name[0]));
        if (!vk) {
            fwprintf(stderr, L"unknown key '%s'\n", names[i]);
            continue;
        }

        if (ctrl) keybd_event(VK_CONTROL, 0, 0, 0);
        keybd_event(static_cast<BYTE>(vk), 0, 0, 0);
        Sleep(40);
        keybd_event(static_cast<BYTE>(vk), 0, KEYEVENTF_KEYUP, 0);
        if (ctrl) keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        Sleep(220);
    }
}

bool savePng(HBITMAP bmp, const std::wstring& path) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);

    for (UINT i = 0; i < num; i++) {
        if (wcscmp(codecs[i].MimeType, L"image/png") != 0) continue;
        Gdiplus::Bitmap image(bmp, nullptr);
        return image.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
    }
    return false;
}

int shot(HWND main, const std::wstring& path) {
    RECT rc{};
    GetWindowRect(main, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return 1;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    // PW_RENDERFULLCONTENT captures a window that is neither focused nor on top.
    BOOL ok = PrintWindow(main, mem, 2);
    SelectObject(mem, old);

    bool saved = savePng(bmp, path);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    printf("%s %dx%d printwindow=%d saved=%d\n", narrow(path).c_str(), w, h,
           ok ? 1 : 0, saved ? 1 : 0);
    return saved ? 0 : 1;
}

void notifyParent(HWND main, HWND control, int notifyCode) {
    SendMessageW(main, WM_COMMAND,
                 MAKEWPARAM(controlId(control), notifyCode),
                 reinterpret_cast<LPARAM>(control));
}

int usage() {
    fwprintf(stderr, L"probe list|count|status|clip|search|agent|refresh|click|shot\n");
    return 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Match the app's per-monitor awareness, otherwise every coordinate this
    // tool reads or sends is silently scaled by Windows.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    // Optional leading "--pid <n>" selects one instance.
    DWORD wantPid = 0;
    if (argc > 2 && std::wstring(argv[1]) == L"--pid") {
        wantPid = static_cast<DWORD>(_wtoi(argv[2]));
        argv += 2;
        argc -= 2;
    }

    int rc = 0;
    HWND main = findMain(wantPid);
    if (!main) {
        fwprintf(stderr, L"ERROR: AI Sessions window not found\n");
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    std::wstring cmd = argc > 1 ? argv[1] : L"list";
    HWND list = childById(main, 105);
    HWND status = childById(main, 106);

    if (cmd == L"list") {
        RECT wr{};
        GetWindowRect(main, &wr);
        printf("main=0x%llX rect=%d,%d,%d,%d\n",
               reinterpret_cast<unsigned long long>(main), wr.left, wr.top, wr.right, wr.bottom);
        for (const Child& c : children(main)) {
            printf("  hwnd=0x%-10llX id=%-4d class=%-20s rect=%d,%d,%d,%d visible=%d\n",
                   reinterpret_cast<unsigned long long>(c.hwnd), controlId(c.hwnd),
                   narrow(c.cls).c_str(), c.rc.left, c.rc.top, c.rc.right, c.rc.bottom,
                   (c.style & WS_VISIBLE) ? 1 : 0);
        }
    } else if (cmd == L"count") {
        printf("%d\n", static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0)));
    } else if (cmd == L"combo") {
        HWND combo = childById(main, 102);
        printf("items=%d cursel=%d dropped=%d\n",
               static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0)),
               static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)),
               static_cast<int>(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)));
    } else if (cmd == L"treecount") {
        // Grows when a node expands, so it shows whether one click was enough.
        HWND tree = childByClass(main, L"SysTreeView32");
        printf("%d\n", tree ? static_cast<int>(SendMessageW(tree, TVM_GETCOUNT, 0, 0)) : -1);
    } else if (cmd == L"treestate") {
        // HTREEITEMs are returned by value and fed straight back, so this needs
        // no pointer to cross the process boundary.
        HWND tree = childByClass(main, L"SysTreeView32");
        HTREEITEM root = tree ? reinterpret_cast<HTREEITEM>(
            SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0)) : nullptr;
        if (!root) {
            printf("no tree\n");
        } else {
            int row = 0;
            for (HTREEITEM it = root; it; it = reinterpret_cast<HTREEITEM>(
                     SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXTVISIBLE,
                                  reinterpret_cast<LPARAM>(it)))) {
                UINT state = static_cast<UINT>(SendMessageW(
                    tree, TVM_GETITEMSTATE, reinterpret_cast<WPARAM>(it),
                    TVIS_EXPANDED | TVIS_SELECTED));
                printf("  row %-2d expanded=%d selected=%d\n", row++,
                       (state & TVIS_EXPANDED) ? 1 : 0,
                       (state & TVIS_SELECTED) ? 1 : 0);
                if (row > 40) break;
            }
        }
    } else if (cmd == L"status") {
        printf("%s\n", narrow(textOf(status)).c_str());
    } else if (cmd == L"gettext" && argc > 2) {
        HWND target = childById(main, _wtoi(argv[2]));
        if (!target) {
            fwprintf(stderr, L"ERROR: no control with id %s\n", argv[2]);
            rc = 1;
        } else {
            printf("[%s]\n", narrow(textOf(target)).c_str());
        }
    } else if (cmd == L"clip") {
        if (argc > 3 && std::wstring(argv[2]) == L"set") writeClipboard(argv[3]);
        else if (argc > 2 && std::wstring(argv[2]) == L"set") writeClipboard(L"");
        printf("%s\n", narrow(readClipboard()).c_str());
    } else if (cmd == L"search") {
        // Typed a character at a time so the edit raises its own EN_CHANGE,
        // exactly as it would for a real keystroke.
        HWND edit = childById(main, 101);
        SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L""));
        notifyParent(main, edit, EN_CHANGE);
        if (argc > 2) {
            for (const wchar_t* p = argv[2]; *p; ++p)
                SendMessageW(edit, WM_CHAR, static_cast<WPARAM>(*p), 1);
        }
    } else if (cmd == L"agent" && argc > 2) {
        HWND combo = childById(main, 102);
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(_wtoi(argv[2])), 0);
        notifyParent(main, combo, CBN_SELCHANGE);
    } else if (cmd == L"restore") {
        // SW_SHOWNOACTIVATE: make the window measurable again without pulling
        // it in front of whatever the user is doing.
        ShowWindow(main, SW_SHOWNOACTIVATE);
    } else if (cmd == L"minimize") {
        ShowWindow(main, SW_MINIMIZE);
    } else if (cmd == L"refresh") {
        notifyParent(main, childById(main, 103), BN_CLICKED);
    } else if (cmd == L"treeclick" && argc > 2) {
        // Row position is derived from the control's own item height, so a
        // window that moved or scrolled cannot invalidate the coordinates.
        HWND tree = childByClass(main, L"SysTreeView32");
        int h = tree ? static_cast<int>(SendMessageW(tree, TVM_GETITEMHEIGHT, 0, 0)) : 0;
        if (h <= 0) {
            fwprintf(stderr, L"ERROR: no tree\n");
            rc = 1;
        } else {
            int row = _wtoi(argv[2]);
            bool chevron = argc > 3 && std::wstring(argv[3]) == L"chevron";
            int y = h * row + h / 2;
            int x = chevron ? h + h / 4 : h * 4;
            printf("itemheight=%d click at %d,%d\n", h, x, y);
            realClick(main, tree, x, y);
        }
    } else if (cmd == L"rightclick" && argc > 4) {
        HWND target = childByClass(main, argv[2]);
        if (!target) {
            fwprintf(stderr, L"ERROR: no child matching '%s'\n", argv[2]);
            rc = 1;
        } else if (!realRightClick(main, target, _wtoi(argv[3]), _wtoi(argv[4]))) {
            rc = 1;
        }
    } else if (cmd == L"key" && argc > 2) {
        // Only steal focus when the app does not already have it: a pop-up menu
        // or a message box is a foreground window of the same process, and
        // calling SetForegroundWindow on the main window would dismiss it.
        DWORD targetPid = 0, activePid = 0;
        GetWindowThreadProcessId(main, &targetPid);
        GetWindowThreadProcessId(GetForegroundWindow(), &activePid);
        if (activePid == targetPid || bringForward(main)) sendKeys(argc - 2, argv + 2);
        else rc = 1;
    } else if (cmd == L"popup") {
        // The context menu is a separate top-level window of class #32768.
        // The system keeps cached, hidden ones around, and FindWindow returns
        // whichever comes first in Z-order, so scan for the visible one.
        HWND popup = nullptr;
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            if (classOf(hwnd) == L"#32768" && IsWindowVisible(hwnd)) {
                *reinterpret_cast<HWND*>(lp) = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&popup));

        if (!popup) {
            printf("no popup menu open\n");
        } else {
            RECT pr{};
            GetWindowRect(popup, &pr);
            printf("popup rect=%d,%d,%d,%d\n", pr.left, pr.top, pr.right, pr.bottom);
            if (argc > 2) {
                int w = pr.right - pr.left, h = pr.bottom - pr.top;
                HDC screen = GetDC(nullptr);
                HDC mem = CreateCompatibleDC(screen);
                HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
                HGDIOBJ old = SelectObject(mem, bmp);
                // A menu is drawn by the system; copy it off the screen.
                BitBlt(mem, 0, 0, w, h, screen, pr.left, pr.top, SRCCOPY);
                SelectObject(mem, old);
                printf("saved=%d\n", savePng(bmp, argv[2]) ? 1 : 0);
                DeleteObject(bmp);
                DeleteDC(mem);
                ReleaseDC(nullptr, screen);
            }
        }
    } else if (cmd == L"move" && argc > 5) {
        SetWindowPos(main, nullptr, _wtoi(argv[2]), _wtoi(argv[3]),
                     _wtoi(argv[4]), _wtoi(argv[5]), SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (cmd == L"realclick" && argc > 4) {
        HWND target = childByClass(main, argv[2]);
        WORD modifier = 0;
        if (argc > 5) {
            std::wstring mod = argv[5];
            if (mod == L"ctrl") modifier = VK_CONTROL;
            else if (mod == L"shift") modifier = VK_SHIFT;
        }
        if (!target) {
            fwprintf(stderr, L"ERROR: no child matching '%s'\n", argv[2]);
            rc = 1;
        } else {
            realClick(main, target, _wtoi(argv[3]), _wtoi(argv[4]), modifier);
        }
    } else if (cmd == L"selcount") {
        printf("%d\n", static_cast<int>(SendMessageW(list, LVM_GETSELECTEDCOUNT, 0, 0)));
    } else if ((cmd == L"click" || cmd == L"hover") && argc > 4) {
        HWND target = childByClass(main, argv[2]);
        if (!target) {
            fwprintf(stderr, L"ERROR: no child matching '%s'\n", argv[2]);
            rc = 1;
        } else if (cmd == L"hover") {
            // Sets the list view's hot item so a screenshot shows the hover fill.
            SendMessageW(target, WM_MOUSEMOVE, 0, MAKELPARAM(_wtoi(argv[3]), _wtoi(argv[4])));
        } else {
            msgClick(target, _wtoi(argv[3]), _wtoi(argv[4]));
        }
    } else if (cmd == L"shot" && argc > 2) {
        rc = shot(main, argv[2]);
    } else {
        rc = usage();
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return rc;
}
