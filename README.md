# AI Sessions

Browser for Claude Code and OpenCode session history. Pure Win32 / C++20, no
runtime dependencies — a single self-contained `aisessions.exe`.

Click a row to copy its session id to the clipboard; a toast confirms the copy.

## Sources

| Agent    | Where it reads from |
|----------|---------------------|
| Claude   | `%USERPROFILE%\.claude\history.jsonl`, with AI titles from `%USERPROFILE%\.claude\projects\**\*.jsonl` |
| OpenCode | `%LOCALAPPDATA%\share\opencode\opencode.db`, falling back to `%USERPROFILE%\.local\share\opencode\opencode.db` |

The project transcripts run to hundreds of megabytes, so they are scanned in
1 MB blocks and lines are rejected on a substring test before any JSON parsing.

## UI

* Left pane is the shell's own navigation control (`CLSID_NamespaceTreeControl`)
  — the same one Explorer uses, so drives, folders and icons look identical.
  Selecting a folder filters the list to sessions at or below it.
* Expanding is driven from `INameSpaceTreeControlEvents::OnItemClick` rather
  than by a control style: a click on a label or icon opens a folder and never
  closes it, while a click on the expando toggles it and leaves the selection
  alone — matching the navigation pane. The control reports the hit reliably
  but does not act on it, so both gestures are applied explicitly.
* Theme follows the Windows app mode. `AISESSIONS_THEME=dark|light` overrides it.
* `AISESSIONS_TRACE=<file>` logs the shell control's callbacks. Those callbacks
  are the only visible evidence of what a gesture did, and they fire for real
  input only — a posted `WM_LBUTTONDOWN` never raises `OnItemClick`.
* Window geometry and column widths persist to
  `%LOCALAPPDATA%\AISessions\settings.json`, stored in 96-dpi units so a saved
  layout survives moving between monitors of different scaling.

## Build

Requires the MSVC toolchain and CMake.

```
cmake -S . -B build
cmake --build build
```

Produces `build\aisessions.exe` and `build\probe.exe`.

## probe.exe

Development tool that drives a running window from outside for testing. It
never raises the window — actions go through window messages and screenshots
use `PrintWindow` — so a test run does not interrupt whatever is in front.

```
probe [--pid N] list                  enumerate the window and its controls
probe [--pid N] count | status        list item count / status line
probe [--pid N] search <text>         type into the search box
probe [--pid N] agent <index>         pick an entry in the agent combo
probe [--pid N] refresh               press Refresh
probe [--pid N] click <class> <x> <y> click inside a child control
probe [--pid N] hover <class> <x> <y> set the hot item
probe [--pid N] treecount | treestate  node count / expanded+selected per row
probe [--pid N] treeclick <row> [chevron|label]
                                      real mouse click on a tree row, with the
                                      position derived from the item height
probe [--pid N] realclick <class> <x> <y>
probe [--pid N] move <x> <y> <w> <h>  reposition the window
probe [--pid N] restore | minimize
probe [--pid N] clip [set <text>]     read or overwrite the clipboard
probe [--pid N] toast                 report the toast window's state
probe [--pid N] shot <file.png>       capture the window
```

Note that `LVM_*` / `TVM_*` messages take pointers and cannot be sent across a
process boundary — doing so faults inside comctl32 in the target. Only
pointer-free messages (`WM_LBUTTONDOWN`, `WM_CHAR`, `WM_COMMAND`, and the tree
queries that pass `HTREEITEM`s by value) are used, and clicks are posted rather
than sent, because a tree view's button-down handler runs a nested loop waiting
for the button-up.

`realclick` and `treeclick` are the exceptions: they drive the physical pointer,
because the shell tree raises its COM events for genuine input only. They take
the foreground for a moment and put the cursor back afterwards, and they fail
loudly if the window could not be brought forward, since a click that lands in
another window would otherwise read as the app ignoring it.
