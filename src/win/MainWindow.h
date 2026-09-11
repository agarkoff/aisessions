#pragma once
#include <windows.h>
#include <string>
#include <vector>

#include "core/Session.h"
#include "core/Settings.h"
#include "win/ShellTree.h"

class ToastWindow;

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
    // UTF-16 projection of a Session, built once per load so filtering and
    // list filling never re-convert or re-lowercase the UTF-8 originals.
    struct SessionView {
        std::wstring agent;
        std::wstring sessionId;
        std::wstring directory;
        std::wstring title;
        std::wstring model;
        std::wstring updated;
        std::wstring lcDirectory;  // lowercased, for the folder filter
        std::wstring lcSearch;     // lowercased title + directory + id
    };

    static constexpr int IDC_SEARCH = 101;
    static constexpr int IDC_AGENT = 102;
    static constexpr int IDC_REFRESH = 103;
    static constexpr int IDC_LIST = 105;
    static constexpr int IDC_STATUS = 106;
    static constexpr UINT WM_APP_LOAD_DONE = WM_APP + 1;
    static constexpr UINT WM_APP_DESELECT = WM_APP + 2;
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
    void layout();
    int scale(int v) const;    // 96-dpi units -> physical pixels
    int unscale(int v) const;  // physical pixels -> 96-dpi units
    void rescaleGeometry(UINT fromDpi, UINT toDpi);

    void onCommand(WPARAM wParam, LPARAM lParam);
    LRESULT onNotify(LPARAM lParam);
    LRESULT onListCustomDraw(LPARAM lParam);
    void onFolderSelected(const std::wstring& path);
    void onSplitterDown(LPARAM lParam);
    void onSplitterMove(LPARAM lParam);
    void onSplitterUp();
    bool cursorOverSplitter() const;

    void loadSessionsAsync();
    void onLoadDone(LPARAM lParam);
    void buildViews();
    void applyFilter();
    void fillList();
    void setSubItem(int row, int col, const std::wstring& text);
    void clearSelection(int index);
    std::wstring getSearchText() const;
    void copySessionId(const std::wstring& sessionId);
    void setStatus(const std::wstring& text);
    void readColumnWidths();
    void saveSettings();

    HWND hwnd_ = nullptr;
    HWND hSearch_ = nullptr;
    HWND hAgent_ = nullptr;
    HWND hRefresh_ = nullptr;
    HWND hList_ = nullptr;
    HWND hHeader_ = nullptr;
    HWND hStatus_ = nullptr;
    HFONT hFont_ = nullptr;
    HICON hIconBig_ = nullptr;
    HICON hIconSmall_ = nullptr;
    HIMAGELIST hRowSpacer_ = nullptr;  // sets the list's row height

    ShellTree tree_;

    UINT dpi_ = 96;
    Settings settings_;
    // leftWidth_ and colWidth_ are physical pixels at dpi_; Settings stores the
    // same values in 96-dpi units so a saved layout survives a monitor change.
    std::vector<Session> all_;
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
    int colWidth_[5] = {80, 180, 0, 100, 132};
};
