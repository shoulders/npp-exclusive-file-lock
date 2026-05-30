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
    GENERIC_READ,                        // Minimal access needed to hold the lock
    FILE_SHARE_READ | FILE_SHARE_WRITE,  // See share-mode notes below
    nullptr,                             // Default security attributes
    OPEN_EXISTING,                       // File must already exist
    FILE_ATTRIBUTE_NORMAL,               // No special flags
    nullptr                              // No template file
);
```

**Why `FILE_SHARE_WRITE`?**
Notepad++ opens files with `FILE_SHARE_READ | FILE_SHARE_WRITE` internally,
so our `CreateFile` call must also grant `FILE_SHARE_WRITE` or it will fail
with `ERROR_SHARING_VIOLATION` against Notepad++'s already-open handle.
Granting `FILE_SHARE_WRITE` in our handle does **not** allow external editors
to write: Windows share-mode rules require both sides to agree.  An external
editor that opens with `GENERIC_WRITE` but without `FILE_SHARE_WRITE` in its
own `dwShareMode` still receives `ERROR_SHARING_VIOLATION`, because Notepad++
holds a `GENERIC_WRITE` handle that the external editor's share mode must
cover.  Standard editors behave this way, so they are blocked.

**Why `FILE_SHARE_DELETE` is intentionally omitted:**
Granting `FILE_SHARE_DELETE` would allow any process to open the file with
`DELETE` access, enabling overwrite-by-rename (write to a temp file, then
rename it over the locked file) — this would defeat the lock entirely.  As a
side effect, external rename or move of the locked file is also blocked.  Use
the **Rename Current File…** menu command to rename a locked file safely
(see *Menu items* below).

Notepad++ uses its **own** separate handle for reading and saving, so the
plugin does not interfere with normal editor operations.

Calling `CloseHandle()` on the lock handle removes the lock instantly.

---

## Menu items

| Menu item | What it does |
| --- | --- |
| **Toggle File Locking (On/Off)** | Master switch. Shows a ✔ check-mark when enabled. Turning OFF releases all currently held locks immediately. |
| *(separator)* | — |
| **Lock Current File** | Manually locks the active tab's file (useful if it was opened before locking was enabled). |
| **Unlock Current File** | Releases the lock on the active tab's file without closing the tab. |
| **Show Lock Status** | Message box listing every currently locked file and the on/off state. |
| *(separator)* | — |
| **Add Read-only** | When enabled (✔), also sets `FILE_ATTRIBUTE_READONLY` on each locked file. See [*Add Read-only option*](#add-read-only-option) below. |

---

## Automatic behaviour

| Notepad++ event | Plugin response |
| --- | --- |
| File opened | If locking is **ON**, the new file is locked immediately. |
| File saved (including Save-As) | Lock is re-acquired on the new path so the handle always matches the on-disk file. |
| File renamed in Notepad++ | `NPPN_FILEBEFORERENAME`: lock released. `NPPN_FILERENAMED`: lock re-acquired on new path. `NPPN_FILERENAMECANCEL`: lock restored on original path. |
| File tab closed | Lock is **always** released, regardless of the on/off state. |
| Notepad++ shutdown | All locks are released. |

---

## Add Read-only option

Some applications ignore Win32 exclusive file locks entirely and will overwrite
a locked file without any error.  The most common example is **Windows
Notepad** (`notepad.exe`): it opens files with permissive share flags, so it
does not receive `ERROR_SHARING_VIOLATION` and can silently overwrite a file
that this plugin has locked.  Notepad also does not place any file lock of its
own on files it opens, so other applications (including this plugin) have no
way to tell from the file itself that Notepad has it open.

Enabling **Add Read-only** adds a second layer of protection.  When the option
is on, the plugin calls `SetFileAttributes` to add `FILE_ATTRIBUTE_READONLY` to
every locked file.  Applications that bypass share-mode locking almost always
check this flag and will either refuse to save or prompt the user before
overwriting a read-only file.

### What changes when Add Read-only is on

| Event | Behaviour |
| --- | --- |
| File locked (auto or manual) | `FILE_ATTRIBUTE_READONLY` is set.  The original attribute value is saved internally. |
| File saved from Notepad++ | The flag is **temporarily cleared** just before Notepad++ writes, then **restored immediately** after the save completes — so saving works normally from Notepad++ while the file remains read-only to everything else. |
| File unlocked (any reason) | The original attribute value is restored exactly as it was before the plugin touched it. |
| Tab closed | Original attributes restored and lock released. |
| **Toggle File Locking** turned OFF | All locks released; all attributes restored. |
| **Add Read-only** turned OFF | All attributes restored; locks remain held. |
| Notepad++ shutdown | All locks released; all attributes restored. |

### Limitations

- The read-only attribute is an advisory signal, not a security boundary.
  Any process running with sufficient privileges (e.g. as Administrator) can
  clear the flag and write to the file anyway.
- If Notepad++ crashes or is force-killed, the attribute may be left in the
  read-only state.  To recover, open File Explorer, right-click the file →
  Properties, and untick **Read-only**.

---

## Detecting concurrent file access

When locking is enabled and a file is opened in Notepad++, the plugin checks
whether another application already has that file open before acquiring the
lock.  If one is found, the plugin:

- **does not** lock the file,
- **does not** set `FILE_ATTRIBUTE_READONLY`, and
- shows a warning message box naming the other application.

The check is repeated each time you switch back to that tab.  Once the other
application closes the file, the next tab switch locks it normally.

### How detection works

The plugin uses the **Windows Restart Manager API** (`RmGetList`), the same
mechanism Windows uses to display "this file is open in another program" dialogs
during software installation.  It requires no elevated privileges, does not
enumerate system handles, and does not open or duplicate handles from other
processes.

It reliably detects any application that **currently holds an open file handle**:
Microsoft Word, VS Code, most professional editors, and any other tool that keeps
its handle open while editing.

### Detection limitation — open-read-close applications

The Restart Manager, and every other unprivileged Windows API, can only detect a
process that currently **holds an open kernel handle** to the file.  Some
applications open a file, read its content into memory, **immediately close the
handle**, and then display the data without retaining any OS-visible reference.
Once the handle is closed, Windows retains no record that the process ever opened
the file — there is nothing for any API to report.

**Windows 11 Notepad** (`notepad.exe`) behaves this way.  It releases its file
handle after reading, so the plugin has no means to detect it.  This is a
fundamental OS constraint, not a limitation of the implementation; it cannot be
worked around without a kernel-mode file-system filter driver, which is outside
the scope of a Notepad++ plugin.

The alternatives considered and rejected:

| Alternative | Why rejected |
| --- | --- |
| `NtQuerySystemInformation` + `DuplicateHandle` | Requires `PROCESS_DUP_HANDLE` on every running process — a significant security capability; also expensive (dumps entire kernel handle table) |
| Scan window titles for the filename | Unreliable — any window with the same filename in its title triggers a false positive |
| Kernel-mode filter driver | Entirely out of scope for a Notepad++ plugin |

---

## Project structure

This plugin follows the layout of the official Notepad++ plugin template
(<https://github.com/npp-plugins/plugintemplate>):

```text
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
| --- | --- |
| `Sci_Position.h` | Defines `Sci_Position` (ptrdiff_t alias). Required by `Scintilla.h`. |
| `Scintilla.h` | Defines `uptr_t`, `sptr_t`, and `SCNotification`. The standard Scintilla types used throughout the Notepad++ plugin API. |
| `Notepad_plus_msgs.h` | Defines all `NPPM_*` SendMessage codes and `NPPN_*` notification codes. |
| `PluginInterface.h` | Defines `NppData`, `FuncItem`, `ShortcutKey`, `PFUNCPLUGINCMD`, and the six `extern "C"` exported function signatures. |
| `PluginDefinition.h` | Plugin-specific: sets `PLUGIN_NAME`, `nbFunc`, and forward-declares each menu callback. This is the file you edit when following the four-step template. |
| `PluginDefinition.cpp` | Plugin-specific: implements all callbacks and the six Notepad++ exports. |

> **Note on header files:** The official Notepad++ documentation recommends
> always using the up-to-date versions of `Scintilla.h`, `Sci_Position.h`, and
> `Notepad_plus_msgs.h` from the Notepad++ repository:
>
> - <https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Scintilla.h>
> - <https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Sci_Position.h>
> - <https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h>

---

## Building with VS Code

### Requirements

| Requirement | Notes |
| --- | --- |
| **Build Tools for Visual Studio 2026** | Free — provides the MSVC compiler. Download from <https://visualstudio.microsoft.com/downloads/> |
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
| --- | --- |
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
>
> **Why is the whole command one string?**
> When `cmd.exe /C` runs a command that contains a quoted path, the entire
> argument after `/C` must be a single string.  Splitting it into an `args`
> array causes `cmd.exe` to misparse the quoting.

### Step 4 – Build

Press **Ctrl+Shift+B** to run the default build task (x64 Release).

On success the terminal shows:

```text
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

```text
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

**Can I rename the file while it is open and locked?**
Yes — use Notepad++'s own rename function (right-click the tab → Rename, or
File > Rename, depending on your build).  The rename happens transparently:

1. `WM_COMMAND 41017` (`IDM_FILE_RENAME`) is dequeued → plugin releases all
   locks so nothing blocks `MoveFileExW`.
2. The rename dialog is shown; `WM_ACTIVATE` fires when it closes (this is
   *before* `MoveFileExW`, so the plugin deliberately ignores it).
3. `WM_COMMAND 22003` is dequeued → plugin re-releases any accidentally
   re-acquired locks and arms the outcome detector.
4. **Success:** `MoveFileExW` runs, Notepad++ updates the title bar
   (`WM_SETTEXT`) → plugin detects the old file is gone, re-locks under the
   new name, and updates path tracking.
5. **Cancel:** `WM_SETTEXT` fires with the file still on disk → plugin
   restores the original lock.

External renames (Windows Explorer, command line) are still blocked.

External renames (Windows Explorer, command line, etc.) are still blocked
because the lock handle omits `FILE_SHARE_DELETE` and there is no message to
intercept for those operations.

**Does the lock survive a Save-As to a new path?**
Yes. The plugin listens for `NPPN_FILESAVED`, releases the old handle (on the
old path), and re-acquires a new one on the current path automatically.

**Is this safe for network/UNC paths?**
It depends on the network file system and server configuration. Locking
behaviour on network shares (SMB, NFS, etc.) is governed by the server and
is not guaranteed by this plugin.

**Why does the plugin grant `FILE_SHARE_WRITE` but not `FILE_SHARE_DELETE`?**
`FILE_SHARE_WRITE` is required so our handle can coexist with Notepad++'s own
write-capable handle — without it, `CreateFile` fails immediately against
Notepad++'s already-open handle.  Granting it does not let external editors
write freely, because Windows share-mode rules require both handles to agree;
standard editors open with restrictive share modes and are still blocked.
`FILE_SHARE_DELETE` is intentionally omitted because granting it would allow
any process to open the file with `DELETE` access — enough to delete it or
replace it via an atomic rename-over, defeating the lock.  Rename support is
provided through the **Rename Current File…** plugin command instead.

---

## Implementation notes

The standard Notepad++ plugin API is largely non-functional in this build:
most `NPPM_` messages either crash Notepad++, trigger unrelated dialogs, or
return wrong data, and `NPPN_` notifications fire with unexpected codes or not
at all. The plugin therefore bypasses almost the entire standard API and relies
on lower-level Windows mechanisms instead:

| Concern | Approach |
| --- | --- |
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
