#include <windows.h>
#include <commctrl.h>
#include <ole2.h>

#include "core/Settings.h"
#include "win/MainWindow.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // The shell namespace tree control is an apartment-threaded COM object and
    // wants OLE (not just COM) for its drag-and-drop support.
    if (FAILED(OleInitialize(nullptr))) return 1;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES
              | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    int exitCode = 1;
    {
        Settings settings = Settings::load();
        MainWindow wnd(settings);
        if (wnd.handle()) {
            MSG msg{};
            BOOL got;
            while ((got = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
                if (got == -1) break;
                // Gives Tab / arrow navigation between the controls.
                if (wnd.translateAccelerator(msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
    }

    OleUninitialize();
    return exitCode;
}
