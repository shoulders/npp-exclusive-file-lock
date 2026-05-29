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

**Core locking mechanism:** `CreateFileW()` with `FILE_SHARE_READ` only (denies `FILE_SHARE_WRITE` and `FILE_SHARE_DELETE`). Notepad++ uses its own handle for reading/saving so the plugin lock doesn't interfere. Handles are stored in `g_lockMap` (a `std::map<std::wstring, HANDLE>`) keyed by absolute file path.

**Key global state:**
- `g_lockMap` — path → HANDLE for all active locks
- `g_idToPath` — buffer ID → path (for notification handling)
- `g_lockingEnabled` — master on/off toggle
- `g_hTitleHook` — WH_CALLWNDPROCRET hook handle
- `g_tabHwnd` — cached tab bar HWND
- `g_enumerating` — flag that suppresses hook side-effects during tab cycling

**Event handling:** `beNotified()` handles `NPPN_BUFFERACTIVATED`, `NPPN_FILESAVED`, `NPPN_FILEBEFORECLOSE`, and `NPPN_SHUTDOWN`. However, many of these are unreliable — see the broken API section below.

**Path resolution:** The standard `NPPM_GET*` path messages are broken in this build. The only reliable source of the active file path is the Notepad++ window title bar, parsed via `GetWindowTextW()` in `getCurrentFilePath()`. The parser extracts paths by looking for a drive letter + `:` or a UNC `\\` prefix.

**Auto-locking and tab-close detection:** A `WH_CALLWNDPROCRET` hook fires after every `WM_SETTEXT` on the main Notepad++ window (i.e., every title-bar update). This is used for auto-locking newly activated files. Tab closes are detected by watching for a decrease in `TCM_GETITEMCOUNT`; when the count drops, the remaining open paths are enumerated and any locks whose paths are no longer open are released.

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
