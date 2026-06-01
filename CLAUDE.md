# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build commands

Build tasks must run under `cmd.exe` (not PowerShell) because `vcvars*.bat && msbuild` chaining requires `cmd.exe`. The VS Code tasks in [.vscode/tasks.json](.vscode/tasks.json) handle this automatically via `Ctrl+Shift+B`.

**x64 (matches 64-bit Notepad++ installations):**
```bat
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && msbuild vs.proj\FileLockPlugin.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

**Win32 (matches 32-bit Notepad++ installations):**
```bat
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" && msbuild vs.proj\FileLockPlugin.vcxproj /p:Configuration=Release /p:Platform=Win32 /m
```

Output: `vs.proj\x64\Release\FileLockPlugin.dll` or `vs.proj\Win32\Release\FileLockPlugin.dll`

There are no automated tests. Validation is done by copying the DLL to `C:\Program Files\Notepad++\plugins\FileLockPlugin\FileLockPlugin.dll` and testing manually in Notepad++.

## Architecture

All plugin logic lives in a single file: [src/PluginDefinition.cpp](src/PluginDefinition.cpp). The four header files in `src/` (`Sci_Position.h`, `Scintilla.h`, `Notepad_plus_msgs.h`, `PluginInterface.h`) are the unmodified Notepad++ plugin template headers — only [src/PluginDefinition.h](src/PluginDefinition.h) and `PluginDefinition.cpp` are project-specific.

**Core locking mechanism:** `CreateFileW()` with `FILE_SHARE_READ | FILE_SHARE_WRITE` (no `FILE_SHARE_DELETE`). `FILE_SHARE_WRITE` is required to coexist with Notepad++'s own write handle. `FILE_SHARE_DELETE` is deliberately omitted — adding it allows overwrite-via-rename attacks. Handles are stored in `g_lockMap` (a `std::map<std::wstring, HANDLE>`) keyed by absolute file path.

**Key global state:**
- `g_lockMap` — path → HANDLE for all active locks
- `g_idToPath` — buffer ID → path (for notification handling)
- `g_lockingEnabled` — master on/off toggle
- `g_hTitleHook` — WH_CALLWNDPROCRET hook handle (post-processing, sent messages)
- `g_hGetMsgHook` — WH_GETMESSAGE hook handle (pre-processing, posted messages)
- `g_tabHwnd` — cached tab bar HWND
- `g_enumerating` — flag that suppresses hook side-effects during tab cycling
- `g_preRenameLockedPaths` — paths unlocked for a pending rename
- `g_renameOpDispatched` — set when `IDM_FILE_RENAME_EXEC` (22003) is dequeued
- `g_preDeleteLockedPaths` — all paths unlocked for a pending "Move to Recycle Bin"

**Event handling:** `beNotified()` handles `NPPN_BUFFERACTIVATED`, `NPPN_FILESAVED`, `NPPN_FILEBEFORECLOSE`, and `NPPN_SHUTDOWN`. However, many of these are unreliable — see the broken API section below.

**Path resolution:** The standard `NPPM_GET*` path messages are broken in this build. The only reliable source of the active file path is the Notepad++ window title bar, parsed via `GetWindowTextW()` in `getCurrentFilePath()`. The parser extracts paths by looking for a drive letter + `:` or a UNC `\\` prefix.

**Auto-locking and tab-close detection:** The `WH_CALLWNDPROCRET` hook (`g_hTitleHook`) fires after every `WM_SETTEXT` on the main Notepad++ window (i.e., every title-bar update). This is used for auto-locking newly activated files. Tab closes are detected by watching for a decrease in `TCM_GETITEMCOUNT`; when the count drops, the remaining open paths are enumerated and any locks whose paths are no longer open are released.

**Rename support:** `NPPN_FILEBEFORERENAME` never fires in this build. Rename is handled via two `WH_GETMESSAGE`-intercepted posted `WM_COMMAND` messages:
- `41017` (`IDM_FILE_RENAME`) — opens the rename dialog; all locks released here.
- `22003` (`IDM_FILE_RENAME_EXEC`) — fires after dialog closes, just before `MoveFileExW`; sets `g_renameOpDispatched` and re-releases any accidentally re-locked files.
`WM_ACTIVATE` fires between these two commands (when the dialog closes) — **do not re-lock on `WM_ACTIVATE`**, it fires before `MoveFileExW`. Cancel vs. success is resolved in the `WM_SETTEXT` handler: if a saved path is gone from disk → success (re-lock under new name); if all paths exist and `g_renameOpDispatched` is set → cancel (restore all locks).

**Delete (Move to Recycle Bin) support:** Handled via two `WH_GETMESSAGE`-intercepted `WM_COMMAND` messages (determined empirically from the event log):

- `41016` (`IDM_FILE_DELETE`) — opens the delete confirmation dialog; all locks released here. `unlockPath()` calls `restoreReadOnly()`, restoring `FILE_ATTRIBUTE_READONLY` to its original state before `SHFileOperationW` runs.
- `22007` (`IDM_FILE_DELETE_EXEC`) — fires after the dialog closes, just before `SHFileOperationW`; sets `g_deleteOpDispatched` and re-releases any accidentally re-locked files.
`WM_ACTIVATE` fires between these two commands (dialog closed) — **do not re-lock on `WM_ACTIVATE`**. Cancel vs. success is resolved in `WM_SETTEXT`: if a locked path is gone from disk → success (re-lock survivors); if all paths still exist and `g_deleteOpDispatched` is set → cancel (re-lock everything).

**Tab enumeration:** To enumerate all open files (e.g., for "Lock All" or orphan-lock cleanup), the plugin cycles through tabs by sending `TCM_SETCURSEL` + `WM_NOTIFY(TCN_SELCHANGE)` to the tab bar HWND (found via `EnumChildWindows` looking for class `"TabBar"` or `"SysTabControl32"`), then reads the title bar after each switch. The `g_enumerating` flag prevents the hook from auto-locking or updating `prevTabCount` during this cycling. The original tab is restored afterward.

## Broken Notepad++ API — do not use

This specific Notepad++ build has a mis-mapped message table. These standard plugin API calls must not be used:

| API | Symptom |
|-----|---------|
| `NPPM_GETFULLPATHFROMBUFFERID` | Returns -1 for all buffer IDs |
| `NPPM_GETCURRENTBUFFERID` | Returns the same value for every open tab |
| `NPPM_GETFULLCURRENTPATH` | Triggers an "open file" dialog |
| `NPPM_GETCURRENTDIRECTORY` | Triggers a "save failed" dialog |
| `NPPM_GETCURRENTFILENAME` | Similar side effects |
| `NPPM_SETMENUITEMCHECK` | Crashes Notepad++ — use `CheckMenuItem()` via `GetMenu()` instead |
| `NPPM_GETOPENFILENAMESPRIMARY` | Crashes Notepad++ |
| `NPPM_GETOPENFILENAMESSECOND` | Crashes Notepad++ |
| `NPPN_READY` | Never fires — hook is installed in `setInfo()` instead |
| `NPPN_FILEBEFORECLOSE` | Never fires — tab close is detected via hook watching tab count |

The workarounds for all of the above are already in place in `PluginDefinition.cpp`. Do not "fix" them by switching back to the standard API calls.
