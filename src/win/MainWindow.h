#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Session.h"
#include "core/Settings.h"
#include "win/ShellTree.h"

class MainWindow {
public:
    explicit MainWindow(const Settings& settings);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    HWND handle() const { return hwnd_; }
    UINT dpi() const { return dpi_; }
    HFONT font() const { return hFont_; }

    // True when the message was consumed as dialog navigation (Tab, arrows).
    bool translateAccelerator(MSG& msg);

private:
    // Hover/pressed state for a control whose chrome we paint ourselves.
    struct FlatChrome {
        MainWindow* owner = nullptr;
        bool isCombo = false;
        bool hot = false;
        bool pressed = false;
    };

    // UTF-16 projection of a Session, built once per load so filtering and
    // list filling never re-convert or re-lowercase the UTF-8 originals.
    struct SessionView {
        std::wstring agent;
        std::wstring sessionId;
        std::wstring directory;
        std::wstring title;
        std::wstring model;
        std::wstring updated;
        std::wstring size;
        std::wstring lcDirectory;  // lowercased, for the folder filter
        std::wstring lcSearch;     // lowercased title + directory + id
    };

    // Column order is this enum's order and nothing else. Settings refer to
    // columns by name, so this can be reshuffled without touching them.
    enum Column { kAgent, kSessionId, kTitle, kModel, kSize, kUpdated, kColumns };

    static constexpr int IDC_SEARCH = 101;
    static constexpr int IDC_AGENT = 102;
    static constexpr int IDC_REFRESH = 103;
    static constexpr int IDC_PRUNE = 104;
    static constexpr int IDC_LIST = 105;
    static constexpr int IDC_STATUS = 106;
    static constexpr int IDM_COPY = 200;
    static constexpr int IDM_RESUME = 201;
    static constexpr int IDM_DELETE = 202;
    static constexpr UINT WM_APP_LOAD_DONE = WM_APP + 1;
    static constexpr wchar_t kClassName[] = L"AISessions_MainWindow";

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static DWORD WINAPI loadThread(LPVOID param);

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void createChildren();
    void createFont();
    void applyFonts();
    void setupColumns();
    void applyTheme();
    void applyRowHeight();
    static LRESULT CALLBACK flatChromeProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    void paintFlatChrome(HWND hwnd, FlatChrome& chrome);
    void paintSearchFrame(HDC hdc);
    void layout();
    int textHeight() const;    // one line in the current font
    int scale(int v) const;    // 96-dpi units -> physical pixels
    int unscale(int v) const;  // physical pixels -> 96-dpi units
    void rescaleGeometry(UINT fromDpi, UINT toDpi);

    void onCommand(WPARAM wParam, LPARAM lParam);
    LRESULT onNotify(LPARAM lParam);
    LRESULT onListCustomDraw(LPARAM lParam);
    void onFolderSelected(const std::wstring& path);
    void showListMenu(int x, int y);
    int  rowUnderCursor(int screenX, int screenY) const;
    // Every action works on the whole selection.
    std::vector<int> selectedRows() const;
    void copySessionIds(const std::vector<int>& rows);
    void resumeSessions(const std::vector<int>& rows);
    // From the folder tree's own context menu, not the list's selection.
    void startNewSessionInDirectory(const std::string& agent,
                                    const std::wstring& directory);
    void deleteSessions(const std::vector<int>& rows);
    void deleteStaleSessions();  // every visible row with no transcript left
    void updatePruneButton();

    void onColumnClick(int column);
    void sortFiltered();
    void showSortIndicator();
    static const char* columnName(int column);
    static int columnByName(const std::string& name);
    void onSplitterDown(LPARAM lParam);
    void onSplitterMove(LPARAM lParam);
    void onSplitterUp();
    bool cursorOverSplitter() const;

    void loadSessionsAsync();
    void onLoadDone(LPARAM lParam);
    void buildViews();
    void buildDirCounts();  // one pass over views_, feeds the tree's counts
    void applyFilter();
    void fillList();
    void setSubItem(int row, int col, const std::wstring& text);
    std::wstring getSearchText() const;
    void setStatus(const std::wstring& text);
    void readColumnWidths();
    void saveSettings();

    HWND hwnd_ = nullptr;
    HWND hSearch_ = nullptr;
    HWND hAgent_ = nullptr;
    HWND hRefresh_ = nullptr;
    HWND hPrune_ = nullptr;
    HWND hList_ = nullptr;
    HWND hHeader_ = nullptr;
    HWND hStatus_ = nullptr;
    HFONT hFont_ = nullptr;
    HICON hIconBig_ = nullptr;
    HICON hIconSmall_ = nullptr;
    HIMAGELIST hRowSpacer_ = nullptr;  // sets the list's row height

    FlatChrome refreshChrome_;
    FlatChrome pruneChrome_;
    FlatChrome agentChrome_;
    RECT searchFrame_{};  // frame the parent paints around the borderless edit

    ShellTree tree_;

    UINT dpi_ = 96;
    Settings settings_;
    // leftWidth_ and colWidth_ are physical pixels at dpi_; Settings stores the
    // same values in 96-dpi units so a saved layout survives a monitor change.
    std::vector<Session> all_;
    // Lowercased directory -> sessions at or under it; drives the tree's counts.
    std::unordered_map<std::wstring, int> dirCounts_;
    std::vector<SessionView> views_;
    std::vector<int> filtered_;  // indices into all_/views_
    std::vector<std::wstring> agents_;
    std::wstring selectedDir_;   // lowercased folder path, empty = no filter
    bool loaded_ = false;
    bool loading_ = false;
    bool columnsCreated_ = false;
    int leftWidth_ = 260;
    bool dragSplitter_ = false;
    int splitterX_ = 0;
    int colWidth_[kColumns] = {};  // pixels; Title is derived, the rest persist
    int sortColumn_ = kUpdated;
    bool sortDescending_ = true;
};
