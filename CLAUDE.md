# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build commands

Build tasks must run under `cmd.exe` (not PowerShell) because `vcvars*.bat && msbuild` chaining requires `cmd.exe`. The VS Code tasks in [.vscode/tasks.json](.vscode/tasks.json) handle this automatically via `Ctrl+Shift+B`.

**x64 (matches 64-bit Notepad++ installations):**

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && msbuild vs.proj\ExclusiveFileLock.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

**Win32 (matches 32-bit Notepad++ installations):**

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" && msbuild vs.proj\ExclusiveFileLock.vcxproj /p:Configuration=Release /p:Platform=Win32 /m
```

Output: `vs.proj\x64\Release\ExclusiveFileLock.dll` or `vs.proj\Win32\Release\ExclusiveFileLock.dll`

There are no automated tests. Validation is done by copying the DLL to `C:\Program Files\Notepad++\plugins\ExclusiveFileLock\ExclusiveFileLock.dll` and testing manually in Notepad++.

## Architecture

All plugin logic lives in a single file: [src/PluginDefinition.cpp](src/PluginDefinition.cpp). The four header files in `src/` (`Sci_Position.h`, `Scintilla.h`, `Notepad_plus_msgs.h`, `PluginInterface.h`) are the unmodified Notepad++ plugin template headers — only [src/PluginDefinition.h](src/PluginDefinition.h) and `PluginDefinition.cpp` are project-specific.

**Core locking mechanism:** `CreateFileW()` with `FILE_SHARE_READ | FILE_SHARE_WRITE` (no `FILE_SHARE_DELETE`). `FILE_SHARE_WRITE` is required to coexist with Notepad++'s own write handle. `FILE_SHARE_DELETE` is deliberately omitted — adding it allows overwrite-via-rename attacks. Handles are stored in `g_lockMap` (a `std::map<std::wstring, HANDLE>`) keyed by absolute file path.

**Key global state:**

- `g_lockMap` — path → HANDLE for all active locks
- `g_idToPath` — buffer ID → path (for notification handling)
- `g_lockingEnabled` — master on/off toggle for automatic locking
- `g_addReadOnly` — when true, every locked file also gets `FILE_ATTRIBUTE_READONLY` set on disk
- `g_nppActive` — tracks whether Notepad++ currently has foreground focus
- `g_activeTabPath` — absolute path of the currently active tab
- `g_readOnlyOriginals` — path → original `DWORD` file attributes, saved before `FILE_ATTRIBUTE_READONLY` is applied
- `g_pendingRestorePaths` — paths where the plugin set `FILE_ATTRIBUTE_READONLY` (i.e., the file was originally writable); persisted to registry for crash recovery
- `g_pendingInitialLock` — set at startup when `g_lockingEnabled` is restored as true; cleared by the first `WM_SETTEXT` which defers a lock-all sweep to the message loop
- `g_hTitleHook` — `WH_CALLWNDPROCRET` hook handle (post-processing, sent messages)
- `g_hCmdHook` — `WH_CALLWNDPROC` hook handle (pre-processing, sent messages; handles focus tracking and Ctrl+S save interception)
- `g_hGetMsgHook` — `WH_GETMESSAGE` hook handle (pre-processing, posted messages)
- `g_tabHwnd` — cached tab bar HWND
- `g_enumerating` — flag that suppresses hook side-effects during tab cycling
- `g_preRenameLockedPaths` — paths unlocked for a pending rename
- `g_renameOpDispatched` — set when `IDM_FILE_RENAME_EXEC` (22003) is dequeued
- `g_preDeleteLockedPaths` — all paths unlocked for a pending "Move to Recycle Bin"
- `g_deleteOpDispatched` — set when `IDM_FILE_DELETE_EXEC` (22007) is dequeued
- `g_saveClearedPaths` — paths whose `FILE_ATTRIBUTE_READONLY` was temporarily cleared for a pending save
- `g_foreignOpenPaths` — paths skipped because another process held the file open (suppresses repeated warnings)

**Event handling:** `beNotified()` handles `NPPN_FILEOPENED` (no-op; title bar not yet updated), `NPPN_BUFFERACTIVATED`, `NPPN_FILEBEFORESAVE`, `NPPN_FILESAVED`, `NPPN_FILEBEFORERENAME`, `NPPN_FILERENAMED`, `NPPN_FILERENAMECANCEL`, `NPPN_FILEBEFORECLOSE`, `NPPN_READONLYCHANGED`, and `NPPN_SHUTDOWN`. However, many of these are unreliable — see the broken API section below.

**Path resolution:** The standard `NPPM_GET*` path messages are broken in this build. The only reliable source of the active file path is the Notepad++ window title bar, parsed via `GetWindowTextW()` in `getCurrentFilePath()`. The parser extracts paths by looking for a drive letter + `:` or a UNC `\\` prefix.

**Hook architecture — three hooks:**

1. `g_hTitleHook` (`WH_CALLWNDPROCRET`, fires *after* the target window proc returns): watches `WM_SETTEXT` on the main Notepad++ window for every title-bar update. Used for auto-locking newly activated files, tab-close detection, rename/delete outcome resolution, and per-tab `FILE_ATTRIBUTE_READONLY` management on tab switch.

2. `g_hCmdHook` (`WH_CALLWNDPROC`, fires *before* the target window proc): watches `WM_ACTIVATE` on the main window to track focus — on `WA_INACTIVE` sets `FILE_ATTRIBUTE_READONLY` on all managed files; on `WA_ACTIVE` clears it on the active tab only. Also intercepts *sent* `WM_COMMAND` `IDM_FILE_SAVE` / `IDM_FILE_SAVEALL` (the Ctrl+S path) to clear `FILE_ATTRIBUTE_READONLY` and `buffer._isReadOnly` before Notepad++ writes to disk.

3. `g_hGetMsgHook` (`WH_GETMESSAGE`, fires when a *posted* message is dequeued): intercepts `IDM_FILE_RENAME` (41017), `IDM_FILE_RENAME_EXEC` (22003), `IDM_FILE_DELETE` (41016), `IDM_FILE_DELETE_EXEC` (22007), and *posted* `WM_COMMAND` `IDM_FILE_SAVE` / `IDM_FILE_SAVEALL` (the File-menu path). Also processes the three deferred thread messages `WM_FL_LOCK_ALL`, `WM_FL_CHECK_CLOSE`, and `WM_FL_CLEAR_RO`.

**Scintilla subclass (`sciSubclassProc`):** Installed on both Scintilla views via `SetWindowSubclass`. Intercepts `SCI_SETREADONLY 1` and changes `wParam` to `0`, preventing `pdoc->readonly` from being set to true. Only active when `g_pendingRestorePaths` is non-empty (i.e., when the plugin has made at least one originally-writable file read-only). Files that were already read-only before being opened are unaffected.

**Deferred thread messages:** Three `WM_APP` messages are posted to the UI thread's own queue so that work which requires `enumerateOpenFilePaths()` (or `NPPM_SETBUFFERREADONLY`) runs in the normal message loop rather than inside a hook callback where Notepad++'s re-entrancy guard blocks title-bar updates:

- `WM_FL_LOCK_ALL` (`WM_APP+1`) — locks all open files at startup after session restore
- `WM_FL_CHECK_CLOSE` (`WM_APP+2`) — enumerates open files after a tab-count decrease to release orphaned locks
- `WM_FL_CLEAR_RO` (`WM_APP+3`) — clears `pdoc->readonly` via `SCI_SETREADONLY 0` after `NPPN_READONLYCHANGED` fires re-entrantly inside `buf->setReadOnly(true)`

**Auto-locking and tab-close detection:** The `WH_CALLWNDPROCRET` hook (`g_hTitleHook`) fires after every `WM_SETTEXT` on the main Notepad++ window (i.e., every title-bar update). This is used for auto-locking newly activated files. Tab closes are detected by watching for a decrease in `TCM_GETITEMCOUNT`; when the count drops, `WM_FL_CHECK_CLOSE` is posted to the thread message queue so `enumerateOpenFilePaths()` can run outside the hook context (calling it directly from `WH_CALLWNDPROCRET` fails due to Notepad++'s re-entrancy guard, which blocks title-bar updates during hook execution).

**Save support (Add Read-only mode):** When `g_addReadOnly` is on, `FILE_ATTRIBUTE_READONLY` on disk and `buffer._isReadOnly` inside Notepad++ both block saves. Both hooks intercept `IDM_FILE_SAVE` / `IDM_FILE_SAVEALL` (the `WH_CALLWNDPROC` hook catches Ctrl+S sent messages; `WH_GETMESSAGE` catches File-menu posted messages). Strategy: (1) clear `FILE_ATTRIBUTE_READONLY` from disk, (2) send the Edit > "Clear Read-Only Flag" command to clear `buffer._isReadOnly`, (3) let the original save command proceed normally. `FILE_ATTRIBUTE_READONLY` is restored by the `WM_SETTEXT` handler after the title bar updates (asterisk disappears post-save).

**Rename support:** `NPPN_FILEBEFORERENAME` never fires in this build. Rename is handled via two `WH_GETMESSAGE`-intercepted posted `WM_COMMAND` messages:

- `41017` (`IDM_FILE_RENAME`) — opens the rename dialog; all locks released here.
- `22003` (`IDM_FILE_RENAME_EXEC`) — fires after dialog closes, just before `MoveFileExW`; sets `g_renameOpDispatched` and re-releases any accidentally re-locked files.
`WM_ACTIVATE` fires between these two commands (when the dialog closes) — **do not re-lock on `WM_ACTIVATE`**, it fires before `MoveFileExW`. Cancel vs. success is resolved in the `WM_SETTEXT` handler: if a saved path is gone from disk → success (re-lock under new name); if all paths exist and `g_renameOpDispatched` is set → cancel (restore all locks).

**Delete (Move to Recycle Bin) support:** Handled via two `WH_GETMESSAGE`-intercepted `WM_COMMAND` messages (determined empirically from the event log):

- `41016` (`IDM_FILE_DELETE`) — opens the delete confirmation dialog; all locks released here. `unlockPath()` calls `restoreReadOnly()`, restoring `FILE_ATTRIBUTE_READONLY` to its original state before `SHFileOperationW` runs.
- `22007` (`IDM_FILE_DELETE_EXEC`) — fires after the dialog closes, just before `SHFileOperationW`; sets `g_deleteOpDispatched` and re-releases any accidentally re-locked files.
`WM_ACTIVATE` fires between these two commands (dialog closed) — **do not re-lock on `WM_ACTIVATE`**. Cancel vs. success is resolved in `WM_SETTEXT`: if a locked path is gone from disk → success (re-lock survivors); if all paths still exist and `g_deleteOpDispatched` is set → cancel (re-lock everything).

**Tab enumeration:** To enumerate all open files (e.g., for "Lock All" or orphan-lock cleanup), the plugin cycles through tabs by sending `TCM_SETCURSEL` + `WM_NOTIFY(TCN_SELCHANGE)` to the tab bar HWND (found via `EnumChildWindows` looking for class `"TabBar"` or `"SysTabControl32"`), then reads the title bar after each switch. The `g_enumerating` flag prevents the hook from auto-locking or updating `prevTabCount` during this cycling. The original tab is restored afterward.

**Menu structure:**

```text
Index  Label                       Behaviour
 [0]   Enable File Locking         Flips g_lockingEnabled; updates check-mark; persisted to registry
 [1]   (separator)
 [2]   Add Read-only               Toggles FILE_ATTRIBUTE_READONLY on locked files; persisted to registry
 [3]   (separator)
 [4]   Lock Current File           Manually locks the active tab
 [5]   Unlock Current File         Manually releases the active tab's lock
 [6]   (separator)
 [7]   Show Status                 Message box listing all locked files
 [8]   (separator)
 [9]   Show Diagnostics            Displays captured events and live state
[10]   Enable Logging              Toggles diagnostic event logging
[11]   (separator)
[12]   About                       Version info and help link dialog
```

## Broken Notepad++ API — do not use

This specific Notepad++ build has a mis-mapped message table. These standard plugin API calls must not be used:

| API | Symptom |
| --- | --- |
| `NPPM_GETFULLPATHFROMBUFFERID` | Returns -1 for all buffer IDs |
| `NPPM_GETCURRENTBUFFERID` | Returns the same value for every open tab |
| `NPPM_GETFULLCURRENTPATH` | Triggers an "open file" dialog |
| `NPPM_GETCURRENTDIRECTORY` | Triggers a "save failed" dialog |
| `NPPM_GETCURRENTFILENAME` | Similar side effects |
| `NPPM_SETMENUITEMCHECK` | Crashes Notepad++ — use `CheckMenuItem()` via `GetMenu()` instead |
| `NPPM_GETOPENFILENAMESPRIMARY` | Crashes Notepad++ |
| `NPPM_GETOPENFILENAMESSECOND` | Crashes Notepad++ |
| `NPPN_READY` | Never fires — hook is installed in `setInfo()` instead |
| `NPPN_FILEBEFORERENAME` | Never fires — rename is intercepted via `WH_GETMESSAGE` on `IDM_FILE_RENAME` (41017) |
| `NPPN_FILEBEFORECLOSE` | Unreliable — tab close is detected via hook watching tab count + `WM_FL_CHECK_CLOSE` |

The workarounds for all of the above are already in place in `PluginDefinition.cpp`. Do not "fix" them by switching back to the standard API calls.
