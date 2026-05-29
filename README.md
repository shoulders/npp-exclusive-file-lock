# FileLock – Notepad++ Plugin

A Windows-exclusive Notepad++ plugin that places an **exclusive file lock** on
any file open in the editor, preventing other processes from writing to or
deleting the file while it is open.

---

## How the lock works

The lock is implemented with a single Win32 `CreateFile()` call:

```cpp
HANDLE hFile = CreateFileW(
    filePath,
    GENERIC_READ,           // Minimal access needed to hold the lock
    FILE_SHARE_READ,        // Share READ only — deny WRITE and DELETE
    nullptr,                // Default security attributes
    OPEN_EXISTING,          // File must already exist
    FILE_ATTRIBUTE_NORMAL,  // No special flags
    nullptr                 // No template file
);
```

| Share mode omitted | Effect on competing processes |
|---|---|
| `FILE_SHARE_WRITE` | Any attempt to open the file for writing receives `ERROR_SHARING_VIOLATION` (0x20) |
| `FILE_SHARE_DELETE` | Any attempt to rename or delete the file receives `ERROR_SHARING_VIOLATION` |

Notepad++ uses its **own** separate handle for reading and saving, so the
plugin does not interfere with normal editor operations.

Calling `CloseHandle()` on the lock handle removes the lock instantly.

---

## Menu items

| Menu item | What it does |
|---|---|
| **Toggle File Locking (On/Off)** | Master switch. Shows a ✔ check-mark when enabled. Turning OFF releases all currently held locks immediately. |
| *(separator)* | — |
| **Lock Current File** | Manually locks the active tab's file (useful if it was opened before locking was enabled). |
| **Unlock Current File** | Releases the lock on the active tab's file without closing the tab. |
| **Show Lock Status** | Message box listing every currently locked file and the on/off state. |

---

## Automatic behaviour

| Notepad++ event | Plugin response |
|---|---|
| File opened | If locking is **ON**, the new file is locked immediately. |
| File saved (including Save-As) | Lock is re-acquired on the new path so the handle always matches the on-disk file. |
| File tab closed | Lock is **always** released, regardless of the on/off state. |
| Notepad++ shutdown | All locks are released. |

---

## Project structure

This plugin follows the layout of the official Notepad++ plugin template
(https://github.com/npp-plugins/plugintemplate):

```
FileLockPlugin/
├── src/
│   ├── Sci_Position.h          ← Scintilla position type (Sci_Position, Sci_Line)
│   ├── Scintilla.h             ← Scintilla types: uptr_t, sptr_t, SCNotification
│   ├── Notepad_plus_msgs.h     ← Npp message (NPPM_*) and notification (NPPN_*) constants
│   ├── PluginInterface.h       ← Core plugin types: NppData, FuncItem, ShortcutKey, exports
│   ├── PluginDefinition.h      ← Plugin name, command count, forward declarations (STEPS 1–3)
│   ├── PluginDefinition.cpp    ← All plugin logic and Npp interface exports  (STEP 4)
│   └── FileLockPlugin.def      ← Module definition — ensures clean DLL export names
├── vs.proj/
│   └── FileLockPlugin.vcxproj  ← Visual Studio / MSBuild project file
└── README.md                   ← This file
```

### Header file roles

| File | Purpose |
|---|---|
| `Sci_Position.h` | Defines `Sci_Position` (ptrdiff_t alias). Required by `Scintilla.h`. |
| `Scintilla.h` | Defines `uptr_t`, `sptr_t`, and `SCNotification`. The standard Scintilla types used throughout the Notepad++ plugin API. |
| `Notepad_plus_msgs.h` | Defines all `NPPM_*` SendMessage codes and `NPPN_*` notification codes. |
| `PluginInterface.h` | Defines `NppData`, `FuncItem`, `ShortcutKey`, `PFUNCPLUGINCMD`, and the six `extern "C"` exported function signatures. |
| `PluginDefinition.h` | Plugin-specific: sets `PLUGIN_NAME`, `nbFunc`, and forward-declares each menu callback. This is the file you edit when following the four-step template. |
| `PluginDefinition.cpp` | Plugin-specific: implements all callbacks and the six Notepad++ exports. |

> **Note on header files:** The official Notepad++ documentation recommends
> always using the up-to-date versions of `Scintilla.h`, `Sci_Position.h`, and
> `Notepad_plus_msgs.h` from the Notepad++ repository:
> - https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Scintilla.h
> - https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Sci_Position.h
> - https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h

---

## Building with VS Code

### Requirements

| Requirement | Notes |
|---|---|
| **Build Tools for Visual Studio 2026** | Free — provides the MSVC compiler. Download from https://visualstudio.microsoft.com/downloads/ |
| Workload: **Desktop development with C++** | Select this in the installer |
| Component: **MSVC v145 C++ x64/x86 build tools** | The compiler — must be ticked |
| Component: **Windows 11 SDK (10.0.28000 or later)** | Must be ticked |
| VS Code extension: **C/C++** by Microsoft | IntelliSense and syntax highlighting |

### Step 1 – Verify your Build Tools path

Open a PowerShell terminal in VS Code (**Ctrl+`**) and run:

```powershell
Get-ChildItem -Path "C:\Program Files", "C:\Program Files (x86)" `
    -Recurse -Filter "vcvars64.bat" -ErrorAction SilentlyContinue |
    Select-Object FullName
```

This tells you the exact path to `vcvars64.bat`.  Common locations:

| Installation | Path |
|---|---|
| Build Tools 2026 (x86 install dir) | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |
| Build Tools 2026 (x64 install dir) | `C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |
| Community / Professional 2026 | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat` |

Note that Visual Studio 2026 uses **`\18\`** as its folder name (the internal
version number), not `\2026\`.

### Step 2 – Create the .vscode folder

Inside the `FileLockPlugin\` project root, create a folder named `.vscode`.

### Step 3 – Create tasks.json

Create `.vscode\tasks.json` with the content below.
Replace the `vcvars64.bat` / `vcvars32.bat` paths if your Build Tools are in
a different location (use the path found in Step 1).

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "Build FileLockPlugin (x64 Release)",
      "type": "shell",
      "command": "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat\" && msbuild vs.proj\\FileLockPlugin.vcxproj /p:Configuration=Release /p:Platform=x64 /m",
      "options": {
        "shell": {
          "executable": "cmd.exe",
          "args": ["/C"]
        }
      },
      "group": {
        "kind": "build",
        "isDefault": true
      },
      "problemMatcher": "$msCompile",
      "detail": "Builds a 64-bit DLL for use with 64-bit Notepad++"
    },
    {
      "label": "Build FileLockPlugin (Win32 Release)",
      "type": "shell",
      "command": "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat\" && msbuild vs.proj\\FileLockPlugin.vcxproj /p:Configuration=Release /p:Platform=Win32 /m",
      "options": {
        "shell": {
          "executable": "cmd.exe",
          "args": ["/C"]
        }
      },
      "group": "build",
      "problemMatcher": "$msCompile",
      "detail": "Builds a 32-bit DLL for use with 32-bit Notepad++"
    }
  ]
}
```

> **Why `options.shell.executable`?**
> VS Code's default integrated terminal is PowerShell, which does not support
> `&&` as a command separator.  Setting `shell.executable` to `cmd.exe` for
> the task ensures it always runs in `cmd.exe` regardless of your default
> terminal, keeping the `&&` syntax valid.

> **Why is the whole command one string?**
> When `cmd.exe /C` runs a command that contains a quoted path, the entire
> argument after `/C` must be a single string.  Splitting it into an `args`
> array causes `cmd.exe` to misparse the quoting.

### Step 4 – Build

Press **Ctrl+Shift+B** to run the default build task (x64 Release).

On success the terminal shows:

```
Build succeeded.
    0 Warning(s)
    0 Error(s)
```

The compiled DLL is written to `vs.proj\x64\Release\FileLockPlugin.dll`.

### Step 5 – IntelliSense configuration (optional but recommended)

Create `.vscode\c_cpp_properties.json` so the C/C++ extension resolves
includes correctly and shows accurate error highlighting:

```json
{
  "configurations": [
    {
      "name": "Win32 MSVC",
      "includePath": [
        "${workspaceFolder}/src/**"
      ],
      "defines": [
        "UNICODE",
        "_UNICODE",
        "WIN32",
        "NDEBUG",
        "_WINDOWS",
        "_USRDLL"
      ],
      "windowsSdkVersion": "10.0.28000.0",
      "compilerPath": "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.44.35211/bin/Hostx64/x64/cl.exe",
      "cStandard": "c17",
      "cppStandard": "c++17",
      "intelliSenseMode": "windows-msvc-x64"
    }
  ],
  "version": 4
}
```

> The MSVC version number in `compilerPath` (`14.44.35211`) may differ.
> Find the exact folder at:
> `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\`

### Recommended folder layout after VS Code setup

```
FileLockPlugin/
├── .vscode/
│   ├── tasks.json                   ← build tasks (required)
│   └── c_cpp_properties.json        ← IntelliSense config (optional)
├── src/
│   ├── Sci_Position.h
│   ├── Scintilla.h
│   ├── Notepad_plus_msgs.h
│   ├── PluginInterface.h
│   ├── PluginDefinition.h
│   ├── PluginDefinition.cpp
│   └── FileLockPlugin.def
├── vs.proj/
│   └── FileLockPlugin.vcxproj
└── README.md
```

---

## Building with Visual Studio IDE (alternative)

1. Open `vs.proj\FileLockPlugin.vcxproj` in Visual Studio 2026.
2. Select **Release | x64** (or **Win32** for 32-bit Notepad++).
3. Press **Ctrl+Shift+B**.
4. Output DLL: `vs.proj\x64\Release\FileLockPlugin.dll`.

### Command-line (Developer Command Prompt)

```bat
cd FileLockPlugin
msbuild vs.proj\FileLockPlugin.vcxproj /p:Configuration=Release /p:Platform=x64
```

---

## Installation

1. Locate your Notepad++ plugins folder:
   - 64-bit: `C:\Program Files\Notepad++\plugins\`
   - 32-bit: `C:\Program Files (x86)\Notepad++\plugins\`

2. Create a sub-folder named **`FileLockPlugin`**:
   `...\plugins\FileLockPlugin\`

3. Copy **`FileLockPlugin.dll`** into that sub-folder.

4. **Unblock the DLL** (important for downloaded files):
   Right-click `FileLockPlugin.dll` → Properties → tick **Unblock** → OK.
   Windows will refuse to load a DLL downloaded from the internet until it
   is explicitly unblocked.

5. Restart Notepad++.

6. The plugin appears under **Plugins > FileLock**.

> **Architecture must match:** A 64-bit Notepad++ installation requires a
> **64-bit DLL** (built with Platform=x64).  A 32-bit installation requires a
> **32-bit DLL** (Platform=Win32).  A mismatched DLL is silently ignored.

---

## Frequently asked questions

**Can Notepad++ still save the file while it is locked?**
Yes. The lock handle uses `FILE_SHARE_READ`, which allows Notepad++ to open
the file for reading. Notepad++ uses its own internal handle when saving, and
our lock does not conflict with that handle.

**What happens if another program already holds a write lock?**
`CreateFile()` will return `INVALID_HANDLE_VALUE` and the lock attempt fails.
The **Lock Current File** command will show an error message. The file cannot
be locked by this plugin until the competing handle is released.

**Does the lock survive a Save-As to a new path?**
Yes. The plugin listens for `NPPN_FILESAVED`, releases the old handle (on the
old path), and re-acquires a new one on the current path automatically.

**Is this safe for network/UNC paths?**
It depends on the network file system and server configuration. Locking
behaviour on network shares (SMB, NFS, etc.) is governed by the server and
is not guaranteed by this plugin.

**Why does the plugin use `FILE_SHARE_READ` instead of zero share mode?**
Using share mode `0` (no sharing at all) would also prevent Notepad++ itself
from reading the file, causing the editor to fail on its next read or save.
`FILE_SHARE_READ` strikes the right balance: other processes can still read
the file (including Notepad++), but they cannot write to or delete it.

---

## Implementation notes

The standard Notepad++ plugin API is largely non-functional in this build:
most `NPPM_` messages either crash Notepad++, trigger unrelated dialogs, or
return wrong data, and `NPPN_` notifications fire with unexpected codes or not
at all. The plugin therefore bypasses almost the entire standard API and relies
on lower-level Windows mechanisms instead:

| Concern | Approach |
|---|---|
| **Path resolution** | Title-bar parse via `GetWindowTextW` — the only reliable source of the active file's path |
| **Lock identity** | `g_lockMap` keyed by file path, not buffer ID (`NPPM_GETCURRENTBUFFERID` returns the same value for every tab) |
| **Hook installation** | `setInfo()` — `NPPN_READY` never fires in this build |
| **Auto-locking on file open** | `WH_CALLWNDPROCRET` hook watching `WM_SETTEXT` on the main window — fires after every title-bar update |
| **Enumerating open files** | Programmatic tab cycling via `TCM_SETCURSEL` + `WM_NOTIFY(TCN_SELCHANGE)` on the tab bar `HWND` |
| **Lock release on tab close** | `NPPN_FILEBEFORECLOSE` never fires; instead the hook detects a tab-count decrease and releases any lock whose path is no longer open |

---

## Licence

This plugin is released into the public domain. Use, modify, and distribute
freely with no restrictions.
