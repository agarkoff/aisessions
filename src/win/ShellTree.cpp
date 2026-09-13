#include "ShellTree.h"
#include "StrUtil.h"
#include "Theme.h"
#include "Trace.h"

#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <uxtheme.h>

ShellTree::~ShellTree() {
    destroy();
}

bool ShellTree::create(HWND parent, const RECT& rc) {
    if (control_) return true;

    HRESULT hr = CoCreateInstance(CLSID_NamespaceTreeControl, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&control_));
    if (FAILED(hr) || !control_) return false;

    RECT r = rc;
    // Single-click expansion is done in OnItemClick rather than with
    // NSTCS_SINGLECLICKEXPAND: that style also claims clicks on the expando,
    // leaving no way to collapse a node again.
    //
    // No NSTCS_SPRINGEXPAND either: that expands - and moves the selection to -
    // whatever the pointer merely rests on, silently re-filtering the list.
    const NSTCSTYLE style = NSTCS_HASEXPANDOS | NSTCS_ROOTHASEXPANDO
                          | NSTCS_FULLROWSELECT | NSTCS_SHOWSELECTIONALWAYS
                          | NSTCS_TABSTOP | NSTCS_NOINFOTIP | NSTCS_EVENHEIGHT;
    hr = control_->Initialize(parent, &r, style);
    if (FAILED(hr)) {
        control_->Release();
        control_ = nullptr;
        return false;
    }

    // The control creates its own child window; we need its HWND to lay it out.
    IOleWindow* oleWindow = nullptr;
    if (SUCCEEDED(control_->QueryInterface(IID_PPV_ARGS(&oleWindow)))) {
        oleWindow->GetWindow(&hwnd_);
        oleWindow->Release();
    }

    control_->TreeAdvise(static_cast<INameSpaceTreeControlEvents*>(this), &adviseCookie_);

    innerTree_ = hwnd_ ? FindWindowExW(hwnd_, nullptr, WC_TREEVIEWW, nullptr) : nullptr;
    if (innerTree_)
        SetWindowSubclass(innerTree_, &ShellTree::inputWatchProc, 1,
                          reinterpret_cast<DWORD_PTR>(this));

    // Root at "This PC" so the user sees drives exactly as Explorer shows them.
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT,
                                       nullptr, IID_PPV_ARGS(&root_)))) {
        control_->AppendRoot(root_, SHCONTF_FOLDERS | SHCONTF_INCLUDEHIDDEN,
                             NSTCRS_EXPANDED, nullptr);
    }

    return true;
}

void ShellTree::selectRoot() {
    if (control_ && root_)
        control_->SetItemState(root_, NSTCIS_SELECTED, NSTCIS_SELECTED);
}

void ShellTree::invalidate() {
    if (innerTree_) InvalidateRect(innerTree_, nullptr, TRUE);
}

LRESULT CALLBACK ShellTree::inputWatchProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (auto* self = reinterpret_cast<ShellTree*>(data)) self->userDriven_ = true;
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, &ShellTree::inputWatchProc, id);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void ShellTree::destroy() {
    if (innerTree_) {
        RemoveWindowSubclass(innerTree_, &ShellTree::inputWatchProc, 1);
        innerTree_ = nullptr;
    }
    if (!control_) return;
    if (adviseCookie_) {
        control_->TreeUnadvise(adviseCookie_);
        adviseCookie_ = 0;
    }
    control_->RemoveAllRoots();
    control_->Release();
    control_ = nullptr;
    if (root_) { root_->Release(); root_ = nullptr; }
    hwnd_ = nullptr;
}

void ShellTree::move(const RECT& rc) {
    if (!hwnd_) return;
    MoveWindow(hwnd_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void ShellTree::applyTheme(const wchar_t* themeName, COLORREF background, COLORREF text) {
    if (!control_ || !themeName) return;
    control_->SetTheme(themeName);

    // The host window wraps a plain tree view; that is the window that actually
    // paints, so it needs the theme and the colours.
    HWND inner = hwnd_ ? FindWindowExW(hwnd_, nullptr, WC_TREEVIEWW, nullptr) : nullptr;
    if (!inner) return;
    SetWindowTheme(inner, themeName, nullptr);
    SendMessageW(inner, TVM_SETBKCOLOR, 0, static_cast<LPARAM>(background));
    SendMessageW(inner, TVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(text));
    InvalidateRect(inner, nullptr, TRUE);
}

//--------------------------------------------------------------------
// IUnknown - lifetime belongs to MainWindow, so Release never deletes.
//--------------------------------------------------------------------

IFACEMETHODIMP ShellTree::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_INameSpaceTreeControlEvents) {
        *ppv = static_cast<INameSpaceTreeControlEvents*>(this);
    } else if (riid == IID_INameSpaceTreeControlCustomDraw) {
        // Discovered on this same object by the control, via QueryInterface on
        // the punk handed to TreeAdvise - there is no separate registration call.
        *ppv = static_cast<INameSpaceTreeControlCustomDraw*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ShellTree::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
}

IFACEMETHODIMP_(ULONG) ShellTree::Release() {
    return static_cast<ULONG>(InterlockedDecrement(&refs_));
}

//--------------------------------------------------------------------
// INameSpaceTreeControlEvents
//--------------------------------------------------------------------

IFACEMETHODIMP ShellTree::OnSelectionChanged(IShellItemArray* selection) {
    if (!onSelectionChanged) return S_OK;

    std::wstring path;
    DWORD count = 0;
    if (selection && SUCCEEDED(selection->GetCount(&count)) && count > 0) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(selection->GetItemAt(0, &item)) && item) {
            PWSTR name = nullptr;
            // Fails for virtual nodes such as "This PC", which is exactly the
            // signal that no folder filter applies.
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                path = name;
                CoTaskMemFree(name);
            }
            item->Release();
        }
    }

    if (!userDriven_) {
        // The control remembers the folder selected in a previous run and
        // re-selects it once its tree is populated. Put it back on the root so
        // the app does not silently start filtered; selecting the root reports
        // an empty path, so this cannot recurse.
        if (!path.empty()) selectRoot();
        return S_OK;
    }

    onSelectionChanged(path);
    return S_OK;
}

IFACEMETHODIMP ShellTree::OnItemClick(IShellItem* psi, NSTCEHITTEST hit,
                                      NSTCECLICKTYPE click) {
    userDriven_ = true;
    traceW(L"OnItemClick hit=0x%X click=0x%X", static_cast<unsigned>(hit),
           static_cast<unsigned>(click));

    if (!control_ || !psi || !ISLBUTTON(click) || ISDBLCLICK(click)) return S_OK;

    // Both gestures are driven from here rather than left to the control: it
    // reports the hit reliably but does not act on it, so neither the expando
    // nor a click on the label opened anything on its own.
    //
    // Explorer's navigation pane semantics: the expando toggles, while a click
    // on the label or icon only ever opens a folder, never closes it.
    if (hit & NSTCEHT_ONITEMBUTTON) {
        NSTCITEMSTATE state = 0;
        control_->GetItemState(psi, NSTCIS_EXPANDED, &state);
        bool expanded = (state & NSTCIS_EXPANDED) != 0;
        HRESULT hr = control_->SetItemState(psi, NSTCIS_EXPANDED,
                                            expanded ? 0 : NSTCIS_EXPANDED);
        traceW(L"  toggle expanded=%d -> hr=0x%08X", expanded ? 1 : 0,
               static_cast<unsigned>(hr));
    } else if (hit & (NSTCEHT_ONITEMLABEL | NSTCEHT_ONITEMICON)) {
        HRESULT hr = control_->SetItemState(psi, NSTCIS_EXPANDED, NSTCIS_EXPANDED);
        traceW(L"  expand -> hr=0x%08X", static_cast<unsigned>(hr));
    }
    return S_OK;
}
IFACEMETHODIMP ShellTree::OnPropertyItemCommit(IShellItem*) { return S_OK; }
IFACEMETHODIMP ShellTree::OnItemStateChanging(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) { return S_OK; }
IFACEMETHODIMP ShellTree::OnItemStateChanged(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) { return S_OK; }
IFACEMETHODIMP ShellTree::OnKeyboardInput(UINT, WPARAM, LPARAM) { return S_OK; }
IFACEMETHODIMP ShellTree::OnBeforeExpand(IShellItem*) { return S_OK; }
IFACEMETHODIMP ShellTree::OnAfterExpand(IShellItem*) { return S_OK; }
IFACEMETHODIMP ShellTree::OnBeginLabelEdit(IShellItem*) { return E_NOTIMPL; }
IFACEMETHODIMP ShellTree::OnEndLabelEdit(IShellItem*) { return E_NOTIMPL; }
IFACEMETHODIMP ShellTree::OnGetToolTip(IShellItem*, LPWSTR, int) { return E_NOTIMPL; }
IFACEMETHODIMP ShellTree::OnBeforeItemDelete(IShellItem*) { return E_NOTIMPL; }
IFACEMETHODIMP ShellTree::OnItemAdded(IShellItem*, BOOL) { return S_OK; }
IFACEMETHODIMP ShellTree::OnItemDeleted(IShellItem*, BOOL) { return S_OK; }
IFACEMETHODIMP ShellTree::OnBeforeContextMenu(IShellItem*, REFIID, void** ppv) {
    if (ppv) *ppv = nullptr;
    return E_NOTIMPL;
}
IFACEMETHODIMP ShellTree::OnAfterContextMenu(IShellItem*, IContextMenu*, REFIID, void** ppv) {
    if (ppv) *ppv = nullptr;
    return E_NOTIMPL;
}
IFACEMETHODIMP ShellTree::OnBeforeStateImageChange(IShellItem*) { return S_OK; }
IFACEMETHODIMP ShellTree::OnGetDefaultIconIndex(IShellItem*, int*, int*) { return E_NOTIMPL; }

//--------------------------------------------------------------------
// INameSpaceTreeControlCustomDraw
//--------------------------------------------------------------------

IFACEMETHODIMP ShellTree::PrePaint(HDC, RECT*, LRESULT* plres) {
    // Ask for a callback before each item so its post-paint step can be
    // requested individually; the control's own drawing is left untouched.
    if (plres) *plres = CDRF_NOTIFYITEMDRAW;
    return S_OK;
}

IFACEMETHODIMP ShellTree::PostPaint(HDC, RECT*) { return S_OK; }

IFACEMETHODIMP ShellTree::ItemPrePaint(HDC, RECT*, NSTCCUSTOMDRAW*, COLORREF*,
                                       COLORREF*, LRESULT* plres) {
    // Colours are left as the control chose them; only a post-paint pass is
    // requested, to add the count after the item has drawn normally.
    if (plres) *plres = CDRF_NOTIFYPOSTPAINT;
    return S_OK;
}

IFACEMETHODIMP ShellTree::ItemPostPaint(HDC hdc, RECT* prc, NSTCCUSTOMDRAW* item) {
    if (!folderSessionCount || !item || !item->psi || !prc || !hdc) return S_OK;

    PWSTR name = nullptr;
    // Fails for a virtual node (This PC, a library) exactly like the selection
    // handler, which is the right outcome: nothing to count for those.
    if (FAILED(item->psi->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name)
        return S_OK;
    std::wstring path = lowerW(name);
    CoTaskMemFree(name);
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();

    int count = folderSessionCount(path);
    if (count <= 0) return S_OK;

    std::wstring label = L"(" + std::to_wstring(count) + L")";

    // Right-aligned with padding proportional to the row height, so it scales
    // with DPI the same way the row itself does.
    int pad = (prc->bottom - prc->top) / 3;
    RECT textRc = *prc;
    textRc.right -= pad;
    if (textRc.right <= textRc.left) return S_OK;

    int saved = SaveDC(hdc);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, Theme::instance().dimText());
    DrawTextW(hdc, label.c_str(), -1, &textRc,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(hdc, saved);
    return S_OK;
}
