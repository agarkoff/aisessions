#pragma once
#include <windows.h>
#include <shobjidl.h>

#include <functional>
#include <string>

// Hosts CLSID_NamespaceTreeControl - the same navigation pane Explorer uses -
// so drives and folders look and behave exactly as they do in Explorer,
// including shell icons, lazy expansion and theming.
//
// The class is its own INameSpaceTreeControlEvents sink, and also its own
// custom-draw sink: the control discovers INameSpaceTreeControlCustomDraw by
// QueryInterface on the same object passed to TreeAdvise, so one registration
// covers both. Its lifetime is owned by MainWindow, so Release() never frees
// it; destroy() detaches the sink.
class ShellTree : public INameSpaceTreeControlEvents,
                  public INameSpaceTreeControlCustomDraw {
public:
    ShellTree() = default;
    ~ShellTree();

    ShellTree(const ShellTree&) = delete;
    ShellTree& operator=(const ShellTree&) = delete;

    // Creates the control as a child of `parent`. Returns false when the shell
    // control is unavailable, in which case the caller should fall back.
    bool create(HWND parent, const RECT& rc);
    void destroy();

    HWND handle() const { return hwnd_; }
    void move(const RECT& rc);

    // SetTheme alone leaves the inner tree view painting on the system's
    // default (light) backdrop, so the colours are applied to it directly.
    void applyTheme(const wchar_t* themeName, COLORREF background, COLORREF text);

    // Moves the selection back to the root ("This PC").
    void selectRoot();

    // Repaints the tree, e.g. after folderSessionCount's answers have changed.
    void invalidate();

    // Called with the selected folder's filesystem path, or an empty string for
    // a virtual node such as "This PC" (meaning: no folder filter).
    std::function<void(const std::wstring&)> onSelectionChanged;

    // Called while painting a folder row, with its lowercased filesystem path
    // (no trailing backslash, except a bare drive root such as "e:"). Returns
    // how many sessions sit at or under that folder; 0 draws nothing, so a
    // folder with no sessions is left exactly as Explorer would show it.
    std::function<int(const std::wstring&)> folderSessionCount;

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // INameSpaceTreeControlEvents - only OnSelectionChanged does any work.
    IFACEMETHODIMP OnItemClick(IShellItem*, NSTCEHITTEST, NSTCECLICKTYPE) override;
    IFACEMETHODIMP OnPropertyItemCommit(IShellItem*) override;
    IFACEMETHODIMP OnItemStateChanging(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) override;
    IFACEMETHODIMP OnItemStateChanged(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) override;
    IFACEMETHODIMP OnSelectionChanged(IShellItemArray* selection) override;
    IFACEMETHODIMP OnKeyboardInput(UINT, WPARAM, LPARAM) override;
    IFACEMETHODIMP OnBeforeExpand(IShellItem*) override;
    IFACEMETHODIMP OnAfterExpand(IShellItem*) override;
    IFACEMETHODIMP OnBeginLabelEdit(IShellItem*) override;
    IFACEMETHODIMP OnEndLabelEdit(IShellItem*) override;
    IFACEMETHODIMP OnGetToolTip(IShellItem*, LPWSTR, int) override;
    IFACEMETHODIMP OnBeforeItemDelete(IShellItem*) override;
    IFACEMETHODIMP OnItemAdded(IShellItem*, BOOL) override;
    IFACEMETHODIMP OnItemDeleted(IShellItem*, BOOL) override;
    IFACEMETHODIMP OnBeforeContextMenu(IShellItem*, REFIID, void**) override;
    IFACEMETHODIMP OnAfterContextMenu(IShellItem*, IContextMenu*, REFIID, void**) override;
    IFACEMETHODIMP OnBeforeStateImageChange(IShellItem*) override;
    IFACEMETHODIMP OnGetDefaultIconIndex(IShellItem*, int*, int*) override;

    // INameSpaceTreeControlCustomDraw - only the item post-paint step draws
    // anything; the rest exist to ask for that step and to leave the control's
    // own drawing (icon, label, selection) untouched.
    IFACEMETHODIMP PrePaint(HDC hdc, RECT* prc, LRESULT* plres) override;
    IFACEMETHODIMP PostPaint(HDC hdc, RECT* prc) override;
    IFACEMETHODIMP ItemPrePaint(HDC hdc, RECT* prc, NSTCCUSTOMDRAW* item,
                               COLORREF* textColor, COLORREF* backColor,
                               LRESULT* plres) override;
    IFACEMETHODIMP ItemPostPaint(HDC hdc, RECT* prc, NSTCCUSTOMDRAW* item) override;

private:
    // Set when the inner tree view sees real mouse or keyboard input, which is
    // how a selection the user made is told apart from the one the control
    // restores by itself while populating.
    static LRESULT CALLBACK inputWatchProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR id, DWORD_PTR data);

    INameSpaceTreeControl* control_ = nullptr;
    IShellItem* root_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND innerTree_ = nullptr;
    DWORD adviseCookie_ = 0;
    LONG refs_ = 1;
    bool userDriven_ = false;
};
