// PluginDefinition.cpp
//
// FileLock – Notepad++ Plugin
//
// Implements all plugin logic:
//   • The six mandatory Notepad++ plugin exports (DLL interface)
//   • The four menu-item callback functions
//   • The Windows file-locking mechanism
//   • Lifecycle helpers called from DllMain
//
// FILE LOCKING MECHANISM (Windows-exclusive)
// ──────────────────────────────────────────
// An exclusive lock is held by calling CreateFile() with:
//   dwShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE
//
// This keeps an open kernel HANDLE to the file.  The FILE_SHARE_WRITE flag
// is required so that our handle coexists with Notepad++'s own write-capable
// handle (Notepad++ opens files with FILE_SHARE_READ | FILE_SHARE_WRITE
// internally).  Without it, CreateFile() would fail immediately with
// ERROR_SHARING_VIOLATION against Notepad++'s already-open handle.
//
// Granting FILE_SHARE_WRITE in our handle does NOT permit external processes
// to write.  Windows share-mode rules require both sides to agree: an external
// editor that opens with GENERIC_WRITE and no FILE_SHARE_WRITE in its own
// dwShareMode receives ERROR_SHARING_VIOLATION (0x20) because our handle has
// not granted that share right to callers.  Standard editors behave this way,
// so they are blocked as intended.  The lock is removed by calling
// CloseHandle() on the HANDLE.
//
// FILE_SHARE_DELETE is intentionally omitted.  Including it would allow any
// process to open the file with DELETE access, enabling overwrite-via-rename
// (delete + rename a temp file over the locked file), which would defeat the
// purpose of the lock.  As a consequence, external rename/move of the locked
// file is also blocked.  Use "Rename Current File…" from this plugin's menu
// to rename a locked file: the plugin temporarily releases its lock, performs
// the MoveFileExW call, then immediately re-acquires the lock on the new path.
//
// Notepad++ continues to work normally because it already holds a compatible
// handle, and our lock handle does not deny readers.
//
// LOCK TRACKING
// ─────────────
// g_lockMap  std::map<std::wstring, HANDLE>
//   Key   = absolute file path (from getCurrentFilePath() / title-bar parse).
//   Value = Win32 HANDLE from CreateFile().
//   Path is used as the key because NPPM_GETCURRENTBUFFERID returns the same
//   value for every open tab in this Notepad++ build, making buffer IDs
//   unusable as unique per-tab identifiers.
//
// g_idToPath  std::map<uptr_t, std::wstring>
//   Key   = nmhdr.idFrom buffer ID (real internal ID from NPPN_ notifications).
//   Value = file path recorded when the tab was opened (NPPN_FILEOPENED).
//   Used in NPPN_FILEBEFORECLOSE to look up and release the lock for the
//   closing tab without needing NPPM_GETFULLPATHFROMBUFFERID (which does not
//   work in this Notepad++ build).
//
// MENU STRUCTURE
// ──────────────
// Index  Label                       Behaviour
//  [0]   Enable File Locking          Flips g_lockingEnabled; updates check-mark; persisted to registry
//  [1]   (separator)                  Empty name → Notepad++ renders a line
//  [2]   Lock Current File            Manually locks the active tab
//  [3]   Unlock Current File          Manually releases the active tab's lock
//  [4]   (separator)                  Empty name → Notepad++ renders a line
//  [5]   Show Status                  Message box listing all locked files
//  [6]   (separator)                  Empty name → Notepad++ renders a line
//  [7]   Add Read-only                Toggles FILE_ATTRIBUTE_READONLY on locked files; persisted to registry
//  [8]   (separator)                  Empty name → Notepad++ renders a line
//  [9]   Enable Logging               Toggles diagnostic event logging
// [10]   Show Log                     Displays captured events and live state
//
// NOTEPAD++ MESSAGE USAGE
// ───────────────────────
// getCurrentFilePath() uses GetWindowTextW on the Notepad++ title bar — no
//   NPPM_ messages.  NPPM_ path-query messages (+53/+54/+55) map to unrelated
//   operations in this build and produce side-effect dialogs.
//   NPPM_GETFULLPATHFROMBUFFERID (+58) returns -1 for all buffer IDs.
//   NPPM_GETCURRENTBUFFERID (+60) returns the same value for all open tabs.
//   All path resolution therefore uses the title-bar approach exclusively.
//
// NPPM_GETNBOPENFILES (+7) and NPPM_GETOPENFILENAMESPRIMARY/SECOND (+8/+9)
//   are used in toggleLocking() to enumerate already-open files when locking
//   is enabled, so the user can be offered to lock them immediately.
//
// toggleLocking() uses CheckMenuItem() via GetMenu() — no NPPM_ message.
//   NPPM_SETMENUITEMCHECK (+40) caused a Notepad++ crash in this build.

#include "PluginDefinition.h"

// Standard C++ library
#include <map>          // std::map – path→HANDLE lock map and id→path tracking
#include <set>          // std::set – foreign-open path warning suppression
#include <string>       // std::wstring – wide-character file paths
#include <sstream>      // std::wstringstream – building multi-line messages
#include <vector>       // std::vector – temporary collections
#include <tchar.h>      // _T() macro – literal portability (Unicode / ANSI)
#include <cstdarg>      // va_list – diagnostic helper

// Windows extended controls (TCM_* tab-control messages, TCN_SELCHANGE)
// SetWindowSubclass / RemoveWindowSubclass / DefSubclassProc (comctl32 v6)
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

// Restart Manager — enumerates processes that have a file open
#include <restartmanager.h>
#pragma comment(lib, "rstrtmgr.lib")

// About dialog resources and version reading
#include "resource.h"
#include <shellapi.h>           // ShellExecuteW
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib") // GetFileVersionInfoW / VerQueryValueW

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 1 – Plugin-wide state
// ═══════════════════════════════════════════════════════════════════════════

// g_hDllInstance
//   HINSTANCE of this DLL, captured in DllMain.  Required by RegisterClassExW
//   and CreateWindowEx for the log dialog window class.
static HINSTANCE g_hDllInstance = nullptr;

// g_nppData
//   Holds the Notepad++ window handles supplied by setInfo() at load time.
static NppData g_nppData;

// g_funcItems[]
//   The menu-item definitions returned to Notepad++ by getFuncsArray().
FuncItem funcItem[nbFunc];

// g_lockingEnabled
//   Master on/off switch for AUTOMATIC locking.
//   TRUE  → every file Notepad++ opens is locked immediately.
//   FALSE → no new automatic locks; existing ones are released on toggle-off.
//   Manual Lock/Unlock commands work regardless of this flag.
static bool g_lockingEnabled = false;

// g_lockMap
//   Maps each locked file's absolute path to its exclusive Win32 HANDLE.
//   Path is the key because NPPM_GETCURRENTBUFFERID returns the same value
//   for all open tabs in this build, rendering buffer IDs non-unique.
static std::map<std::wstring, HANDLE> g_lockMap;

// g_idToPath
//   Maps nmhdr.idFrom buffer ID (real internal ID from NPPN_ notifications)
//   to the file path recorded when that tab was opened.  Used by
//   NPPN_FILEBEFORECLOSE to identify and release the lock for the closing tab.
static std::map<uptr_t, std::wstring> g_idToPath;

// g_hTitleHook
//   WH_CALLWNDPROCRET hook installed on Notepad++'s UI thread.
//   Fires after WM_SETTEXT is processed on the main window — i.e., after every
//   title-bar update — so getCurrentFilePath() reliably returns the new path.
//   Used to auto-lock files as they become active while g_lockingEnabled is true.
static HHOOK g_hTitleHook = nullptr;

// g_hCmdHook
//   WH_CALLWNDPROC hook installed on Notepad++'s UI thread.
//   Fires BEFORE a sent message is processed by its destination window.
//   Used to clear FILE_ATTRIBUTE_READONLY on locked files BEFORE Notepad++
//   processes WM_ACTIVATE (WA_ACTIVE) — so when Notepad++ reads file attributes
//   during activation it sees the files as writable and never sets pdoc->readonly.
static HHOOK g_hCmdHook = nullptr;



// g_enumerating
//   Set to true while enumerateOpenFilePaths() cycles through tabs.
//   Suppresses the hook's auto-locking during that phase so that files are not
//   pre-locked before the toggle-on offer is shown to the user.
static bool g_enumerating = false;

// g_tabHwnd / g_prevTabCount
//   Cached tab bar window handle and tab count from the last WM_SETTEXT event.
//   Used by the hook to detect tab closes (count decreases) and release orphaned locks.
//   NPPN_FILEBEFORECLOSE does not fire in this Notepad++ build.
static HWND g_tabHwnd      = nullptr;
static int  g_prevTabCount = 0;

// g_renamePendingLocks
//   Maps buffer ID to pre-rename path for any file whose lock was released in
//   NPPN_FILEBEFORERENAME.  The entry is consumed in NPPN_FILERENAMED (re-lock
//   on new path) or NPPN_FILERENAMECANCEL (re-lock on original path).
//   Secondary mechanism — NPPN_FILEBEFORERENAME does not fire in this build;
//   the primary mechanism uses WH_GETMESSAGE to intercept IDM_FILE_RENAME.
static std::map<uptr_t, std::wstring> g_renamePendingLocks;

// g_hGetMsgHook
//   WH_GETMESSAGE hook installed on Notepad++'s UI thread.
//   Fires when a posted message is retrieved from the message queue (i.e.
//   inside GetMessage / PeekMessage), before DispatchMessage is called.
//   Menu commands are posted (PostMessage), not sent (SendMessage), so they
//   are NOT visible to WH_CALLWNDPROC — WH_GETMESSAGE is required.
//   Used to release the lock just before Notepad++ dispatches IDM_FILE_RENAME
//   (WM_COMMAND 41017), so that MoveFileExW inside Notepad++ can proceed.
static HHOOK g_hGetMsgHook = nullptr;

// g_preRenameLockedPaths
//   All paths locked when IDM_FILE_RENAME (41017) was dequeued.  Every lock is
//   released immediately so that whichever file Notepad++ renames is not blocked.
//   Cleared once the rename outcome is confirmed (see g_renameOpDispatched).
static std::vector<std::wstring> g_preRenameLockedPaths;

// g_renameOpDispatched
//   Set when WM_COMMAND 22003 is dequeued.  This is the command that causes
//   Notepad++ to call MoveFileExW; it fires after WM_ACTIVATE (dialog closed).
//   Once this flag is set, the next WM_SETTEXT in titleBarHookProc resolves the
//   rename outcome:
//     • A locked path is gone from disk → rename succeeded → re-lock under new name.
//     • All paths still exist          → rename cancelled  → restore all locks.
static bool g_renameOpDispatched = false;

// g_preDeleteLockedPaths
//   All paths that were locked when IDM_FILE_DELETE (41016) was dequeued.
//   Every lock is released immediately so whichever file Notepad++ moves to
//   the recycle bin is not blocked by our handle (which omits FILE_SHARE_DELETE).
//   Resolved on the next WM_SETTEXT in titleBarHookProc:
//     • any path gone from disk → that file was deleted; re-lock the survivors
//     • all paths still exist  → delete was cancelled; re-lock everything
static std::vector<std::wstring> g_preDeleteLockedPaths;

// g_deleteOpDispatched
//   Set when WM_COMMAND IDM_FILE_DELETE_EXEC (22007) is dequeued.  This fires
//   after the confirmation dialog closes (and after WM_ACTIVATE), just before
//   Notepad++ calls SHFileOperationW.  Once set, the next WM_SETTEXT in
//   titleBarHookProc resolves the outcome:
//     • a locked path gone from disk → delete succeeded → re-lock survivors
//     • all paths still exist        → delete cancelled → re-lock everything
static bool g_deleteOpDispatched = false;

// IDM_FILE_RENAME_EXEC — the WM_COMMAND ID that Notepad++ posts when it is
// about to call MoveFileExW, fired after the rename dialog closes.
// Determined empirically from the WM_COMMAND log (captured while files were locked).
static const UINT IDM_FILE_RENAME_EXEC = 22003;

// IDM_FILE_RENAME — Notepad++ menu command ID for "Rename" (IDM_FILE + 17).
// IDM_FILE = IDM + 1000 = 41000;  IDM_FILE_RENAME = 41000 + 17 = 41017.
static const UINT IDM_FILE_RENAME = 41017;

// IDM_FILE_DELETE — Notepad++ menu command ID for "Move to Recycle Bin" (IDM_FILE + 16).
// IDM_FILE = 41000; IDM_FILE_DELETE = 41000 + 16 = 41016.
// Determined empirically: the WM_COMMAND log showed 41016 when "Move to Recycle Bin"
// was clicked, not 41018 (IDM_FILE + 18) which the Notepad++ source suggests.
static const UINT IDM_FILE_DELETE = 41016;

// IDM_FILE_DELETE_EXEC — the WM_COMMAND ID posted after the delete confirmation
// dialog closes and just before Notepad++ calls SHFileOperationW.
// Determined empirically (parallel to IDM_FILE_RENAME_EXEC = 22003).
// WM_ACTIVATE fires between IDM_FILE_DELETE and IDM_FILE_DELETE_EXEC (when the
// confirmation dialog closes) — do NOT re-lock on WM_ACTIVATE; wait for this ID.
static const UINT IDM_FILE_DELETE_EXEC = 22007;

// WM_FL_LOCK_ALL — thread message posted by titleBarHookProc when a remembered
// locking state is detected at startup.  Processed by getMsgHookProc in the
// normal message loop so that enumerateOpenFilePaths() runs outside any hook
// re-entrancy context (calling it directly from WH_CALLWNDPROCRET causes
// Notepad++ to skip title-bar updates for all but the first tab switch).
static const UINT WM_FL_LOCK_ALL = WM_APP + 1;

// WM_FL_CHECK_CLOSE — thread message posted by the WM_SETTEXT handler when
// the tab count decreases.  enumerateOpenFilePaths() fails from WH_CALLWNDPROCRET
// (Notepad++ re-entrancy guard prevents title updates during hook execution),
// so close detection is deferred to the normal message loop where it works.
static const UINT WM_FL_CHECK_CLOSE = WM_APP + 2;

// WM_FL_CLEAR_RO — thread message posted by the NPPN_READONLYCHANGED handler.
// NPPN_READONLYCHANGED fires from INSIDE Notepad++'s buf->setReadOnly(true) call.
// Calling NPPM_SETBUFFERREADONLY from within that notification fires re-entrantly,
// and Notepad++ overrides it when the outer buf->setReadOnly(true) resumes.
// Posting this message defers the NPPM_SETBUFFERREADONLY call to the normal
// message loop, after the entire buf->setReadOnly(true) call stack has unwound.
// wParam = buffer ID (uptr_t cast to WPARAM).
static const UINT WM_FL_CLEAR_RO = WM_APP + 3;

// SCI_SETREADONLY and SCI_GETREADONLY are now defined in the full Scintilla.h.

// g_loggingEnabled
//   When true, log() writes timestamped entries to g_log and OutputDebugStringW.
//   When false, log() is a no-op.  The value is persisted in the registry
//   so it survives restarts unconditionally.
static bool g_loggingEnabled = false;

// g_log — in-memory event log shown by Show Log.
// Accumulates timestamped entries from key points in the hook chain.
// Capped at 200 entries.  Cleared when logging is toggled on.
static std::vector<std::wstring> g_log;
static DWORD g_logBase = 0;   // GetTickCount() at first log entry

static void log(const wchar_t* fmt, ...)
{
    if (!g_loggingEnabled) return;
    if (g_log.size() >= 200) return;

    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    ::_vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    DWORD now = ::GetTickCount();
    if (g_logBase == 0) g_logBase = now;

    wchar_t entry[600];
    ::_snwprintf_s(entry, _countof(entry), _TRUNCATE,
                   L"[+%4lums] %s", (unsigned long)(now - g_logBase), buf);
    g_log.push_back(entry);
    ::OutputDebugStringW(entry);
}

// dbg — unchanged, still sends to OutputDebugStringW.
static void dbg(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    ::_vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    ::OutputDebugStringW(buf);
}

// IDM_FILE_SAVE / IDM_FILE_SAVEALL — WM_COMMAND IDs for File > Save and
// File > Save All (IDM_FILE + 6 and IDM_FILE + 8).  Intercepted in
// getMsgHookProc to temporarily clear FILE_ATTRIBUTE_READONLY before
// Notepad++ writes to disk, allowing saves to succeed on read-only-flagged files.
static const UINT IDM_FILE_SAVE    = 41006;   // IDM_FILE + 6
static const UINT IDM_FILE_SAVEALL = 41008;   // IDM_FILE + 8

// NPPM_SAVEFILE — saves the file at the given path (must be open in Notepad++).
// wParam: 0, lParam: const wchar_t* full file path.
// Returns TRUE on success, FALSE if the path is not open or the save fails.
// Used to save a pseudo-read-only locked file: the hook clears FILE_ATTRIBUTE_READONLY,
// calls NPPM_SAVEFILE to perform the actual write (bypassing Notepad++'s UI-level
// read-only check on buffer._isReadOnly), then WM_SETTEXT restores the attribute.
static const UINT NPPM_SAVEFILE_MSG = NPPMSG + 94;  // 2024 + 94 = 2118

// g_saveInProgress — prevents the save hook from re-entering itself.
static bool g_saveInProgress = false;

// g_setReadOnlyCmdId — cached command ID for the Edit > Set Read-Only toggle.
// Found lazily by scanning the Edit menu text.  0 = not yet searched / not found.
static UINT g_setReadOnlyCmdId = 0;

// g_clearReadOnlyCmdId — cached command ID for Edit > "Clear Read-Only Flag".
// This is a separate, one-directional command: it only clears buffer._isReadOnly,
// never sets it.  This is the correct command to call before a save because it
// does not need direction-detection.
// Example text scanned: "&Clear Read-Only Flag"
static UINT g_clearReadOnlyCmdId = 0;

// g_toggleInProgress / g_lastToggleDocStatus — the Set Read-Only command is a
// toggle: it flips buffer._isReadOnly without knowing the current direction.
// We capture the NPPN_READONLYCHANGED docStatus that fires synchronously during
// the SendMessage call.  If docStatus has DOCSTATUS_READONLY set, we went the
// wrong way (buffer was already writable) and must toggle again to correct it.
static bool g_toggleInProgress   = false;
static UINT g_lastToggleDocStatus = 0;

// NPPN_READONLYCHANGED is now defined in the official header (NPPN_FIRST + 16 = 1016).
// With the correct NPPN_FIRST = 1000, this notification may now fire reliably.

// g_addReadOnly
//   When true, every locked file also has FILE_ATTRIBUTE_READONLY set on disk.
//   The original attribute state is saved in g_readOnlyOriginals and is
//   restored exactly when the file is unlocked, locking is disabled,
//   the file is closed, or Notepad++ shuts down.
static bool g_addReadOnly = false;

// g_pendingInitialLock
//   Set by loadSettings() when g_lockingEnabled is restored as true.
//   The first WM_SETTEXT in titleBarHookProc after startup clears this flag and
//   locks all already-open files, covering session-restored tabs that were loaded
//   before setInfo() ran (i.e., before NPPN_BUFFERACTIVATED could act on the
//   restored g_lockingEnabled value).
static bool g_pendingInitialLock = false;

// g_nppActive
//   Tracks whether Notepad++ currently has the foreground focus.
//   FILE_ATTRIBUTE_READONLY is only applied to locked files while g_nppActive is FALSE.
//   While Notepad++ is focused, the attribute stays clear so saves and tab switches
//   never trigger the file-monitor cycle that causes grey icons and save failures.
//   Initialized to true: Notepad++ is assumed focused at startup.
static bool g_nppActive = true;

// g_pendingBufferId — buffer ID saved from NPPN_BUFFERACTIVATED before the
// title bar updates.  Correlated with the path in the next WM_SETTEXT so
// that g_idToPath always has an entry for NPPM_SETBUFFERREADONLY lookups.
static uptr_t g_pendingBufferId = 0;

// Registry key used to persist plugin option states.
static const wchar_t* REG_KEY = L"Software\\FileLockPlugin";

// g_readOnlyOriginals
//   For each path where we applied FILE_ATTRIBUTE_READONLY, stores the
//   DWORD file attributes that were present before we made the change.
//   Used by restoreReadOnly() to put the attribute back exactly as found.
static std::map<std::wstring, DWORD> g_readOnlyOriginals;

// g_pendingRestorePaths
//   Paths where WE set FILE_ATTRIBUTE_READONLY in the current session (i.e.,
//   the original file was writable).  Persisted to the registry under
//   "PendingRestore" so that if Notepad++ crashes without running cleanup,
//   the next startup can clear the attribute before Notepad++ opens the files.
//   Without this, a crash leaves files permanently stuck as read-only:
//   applyReadOnly() then sees the flag already set, records "originally read-only",
//   and restoreReadOnly() restores that same flag — a loop that never breaks.
static std::set<std::wstring> g_pendingRestorePaths;

// g_saveClearedPaths
//   Paths whose FILE_ATTRIBUTE_READONLY was temporarily cleared so that
//   Notepad++ can write the file.  Populated when IDM_FILE_SAVE or
//   IDM_FILE_SAVEALL is dequeued (WH_GETMESSAGE).  Each path is removed
//   in NPPN_FILESAVED after the write completes and r/o is re-applied.
//   Any paths remaining after a failed save are swept up by the safety-net
//   check in titleBarHookProc on the next WM_SETTEXT.
static std::vector<std::wstring> g_saveClearedPaths;

// g_foreignOpenPaths
//   Paths for which a foreign (non-Notepad++) process was detected holding
//   the file open.  Prevents the warning popup from repeating on every tab
//   switch.  The RM check is still re-run each time; once the other process
//   closes the file, lockPath() proceeds normally and removes the entry.
//   Cleared in releaseAllLocks() and when a file's tab is closed.
static std::set<std::wstring> g_foreignOpenPaths;


// ═══════════════════════════════════════════════════════════════════════════
// SECTION 2 – Settings persistence (registry)
// ═══════════════════════════════════════════════════════════════════════════

static void saveSettings()
{
    HKEY hKey = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                          &hKey, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v;
    v = g_lockingEnabled ? 1 : 0;
    ::RegSetValueExW(hKey, L"LockingEnabled", 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&v), sizeof(v));
    v = g_addReadOnly ? 1 : 0;
    ::RegSetValueExW(hKey, L"AddReadOnly", 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&v), sizeof(v));
    ::RegCloseKey(hKey);
}

static void loadSettings()
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE,
                        &hKey) != ERROR_SUCCESS)
        return;
    DWORD type = 0, size = sizeof(DWORD), v = 0;
    if (::RegQueryValueExW(hKey, L"LockingEnabled", nullptr, &type,
                           reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS
        && type == REG_DWORD)
    {
        g_lockingEnabled = (v != 0);
        if (g_lockingEnabled)
            g_pendingInitialLock = true;
    }
    v = 0; size = sizeof(DWORD);
    if (::RegQueryValueExW(hKey, L"AddReadOnly", nullptr, &type,
                           reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        g_addReadOnly = (v != 0);
    v = 0; size = sizeof(DWORD);
    if (::RegQueryValueExW(hKey, L"LoggingEnabled", nullptr, &type,
                           reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        g_loggingEnabled = (v != 0);
    ::RegCloseKey(hKey);
}

// ── saveLoggingRegistry ───────────────────────────────────────────────────────
//
// Persists only g_loggingEnabled to the registry.  Called from toggleLogging()
// so the logging state survives Notepad++ restarts.
// ─────────────────────────────────────────────────────────────────────────────
static void saveLoggingRegistry()
{
    HKEY hKey = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                          &hKey, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = g_loggingEnabled ? 1 : 0;
    ::RegSetValueExW(hKey, L"LoggingEnabled", 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&v), sizeof(v));
    ::RegCloseKey(hKey);
}

// ── savePendingRestoreRegistry ────────────────────────────────────────────────
//
// Writes g_pendingRestorePaths to HKCU\Software\FileLockPlugin\PendingRestore
// as REG_MULTI_SZ so crash recovery can find them at the next startup.
// Deletes the value if the set is empty.
// ─────────────────────────────────────────────────────────────────────────────
static void savePendingRestoreRegistry()
{
    HKEY hKey = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                          nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;

    if (g_pendingRestorePaths.empty())
    {
        ::RegDeleteValueW(hKey, L"PendingRestore");
    }
    else
    {
        std::vector<wchar_t> buf;
        for (const auto& p : g_pendingRestorePaths)
        {
            buf.insert(buf.end(), p.begin(), p.end());
            buf.push_back(L'\0');
        }
        buf.push_back(L'\0');   // REG_MULTI_SZ double-null terminator
        ::RegSetValueExW(hKey, L"PendingRestore", 0, REG_MULTI_SZ,
                         reinterpret_cast<const BYTE*>(buf.data()),
                         static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
    }
    ::RegCloseKey(hKey);
}

// ── crashRecoveryRestore ──────────────────────────────────────────────────────
//
// Called once from setInfo() before Notepad++ opens any session files.
// Reads the PendingRestore registry list — paths where we set
// FILE_ATTRIBUTE_READONLY in a session that ended without cleanup (crash /
// force-kill).  Clears the attribute on those files so Notepad++ opens them
// as writable, then deletes the registry value.
//
// Without this, a crash leaves files permanently stuck: applyReadOnly() sees
// the flag already set, saves "originally read-only", and restoreReadOnly()
// restores that same flag every normal shutdown — a loop that never breaks.
// ─────────────────────────────────────────────────────────────────────────────
static void crashRecoveryRestore()
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    DWORD type = 0, size = 0;
    if (::RegQueryValueExW(hKey, L"PendingRestore", nullptr,
                           &type, nullptr, &size) != ERROR_SUCCESS
        || type != REG_MULTI_SZ || size == 0)
    {
        ::RegCloseKey(hKey);
        return;
    }

    std::vector<wchar_t> buf(size / sizeof(wchar_t));
    if (::RegQueryValueExW(hKey, L"PendingRestore", nullptr, &type,
                           reinterpret_cast<BYTE*>(buf.data()),
                           &size) == ERROR_SUCCESS)
    {
        for (const wchar_t* p = buf.data(); *p; p += ::wcslen(p) + 1)
        {
            DWORD attrs = ::GetFileAttributesW(p);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                ::SetFileAttributesW(p, attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }

    ::RegDeleteValueW(hKey, L"PendingRestore");
    ::RegCloseKey(hKey);
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 3 – Internal file-lock helpers
// ═══════════════════════════════════════════════════════════════════════════

// ── getCurrentFilePath ───────────────────────────────────────────────────────
//
// Returns the absolute file path of the currently active Notepad++ tab.
//
// Uses GetWindowTextW on the Notepad++ main window — no NPPM_ message required.
// Notepad++ always reflects the active tab's full path in its title bar:
//
//   [*]C:\full\path\to\file.txt - Notepad++
//
// where the optional leading '*' marks unsaved changes.  The suffix
// ' - Notepad++' has been stable across every version since v4.
//
// Untitled tabs produce titles like "new 1 - Notepad++".  After stripping the
// suffix the remainder contains no drive letter (':') and no UNC prefix ('\\'),
// so they are correctly returned as empty.
//
// Returns an empty wstring if the active tab is untitled/unsaved, or if the
// title bar does not match the expected format.
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring getCurrentFilePath()
{
    wchar_t title[MAX_PATH * 2] = { 0 };
    if (::GetWindowTextW(g_nppData._nppHandle, title, MAX_PATH * 2) <= 0)
        return L"";

    std::wstring t(title);

    const std::wstring suffix = L" - Notepad++";
    if (t.size() <= suffix.size() ||
        t.compare(t.size() - suffix.size(), suffix.size(), suffix) != 0)
        return L"";

    std::wstring path = t.substr(0, t.size() - suffix.size());

    if (!path.empty() && path[0] == L'*')
        path = path.substr(1);

    bool isDrive = (path.size() >= 2 && path[1] == L':');
    bool isUNC   = (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
    if (!isDrive && !isUNC)
        return L"";

    return path;
}

// ── acquireLock ──────────────────────────────────────────────────────────────
//
// Opens an exclusive Win32 handle to the given file that blocks external
// writers and deleters while allowing Notepad++ to continue working normally.
// Returns INVALID_HANDLE_VALUE on failure.
// ─────────────────────────────────────────────────────────────────────────────
static HANDLE acquireLock(const std::wstring& filePath)
{
    if (filePath.empty())
        return INVALID_HANDLE_VALUE;

    return ::CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
}

// ── releaseLock ──────────────────────────────────────────────────────────────
//
// Closes a Win32 HANDLE, removing the kernel lock on the file.
// Safe to call with INVALID_HANDLE_VALUE or nullptr (both are no-ops).
// ─────────────────────────────────────────────────────────────────────────────
static void releaseLock(HANDLE hFile)
{
    if (hFile != INVALID_HANDLE_VALUE && hFile != nullptr)
        ::CloseHandle(hFile);
}

// ── applyReadOnly ─────────────────────────────────────────────────────────────
//
// Sets FILE_ATTRIBUTE_READONLY on the file at 'path', saving the original
// DWORD attributes in g_readOnlyOriginals so they can be restored later.
// No-op if the path is already tracked (avoids double-saving) or if the
// file already has the read-only flag set (saves original without changing).
// ─────────────────────────────────────────────────────────────────────────────
static void applyReadOnly(const std::wstring& path)
{
    if (path.empty() || g_readOnlyOriginals.count(path))
        return;
    DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return;
    g_readOnlyOriginals[path] = attrs;
    if (!(attrs & FILE_ATTRIBUTE_READONLY))
    {
        g_pendingRestorePaths.insert(path);
        savePendingRestoreRegistry();
        // Only set FILE_ATTRIBUTE_READONLY on disk when Notepad++ does not have focus.
        // While Notepad++ is focused, leaving the attribute clear prevents the
        // file-monitor cycle (attr set → monitor fires → buffer._isReadOnly=true →
        // grey icon + save failures).  The attribute is applied when Notepad++
        // loses focus (WA_INACTIVE handler) and cleared when it regains focus.
        if (!g_nppActive)
            ::SetFileAttributesW(path.c_str(), attrs | FILE_ATTRIBUTE_READONLY);
    }
}

// ── restoreReadOnly ───────────────────────────────────────────────────────────
//
// Restores the file attributes saved by applyReadOnly(), removing the entry
// from g_readOnlyOriginals.  No-op if the path is not tracked.
// ─────────────────────────────────────────────────────────────────────────────
static void restoreReadOnly(const std::wstring& path)
{
    auto it = g_readOnlyOriginals.find(path);
    if (it == g_readOnlyOriginals.end())
        return;
    ::SetFileAttributesW(path.c_str(), it->second);
    g_readOnlyOriginals.erase(it);
    if (g_pendingRestorePaths.erase(path))
        savePendingRestoreRegistry();
}

// ── getFileOwnerProcessNames ──────────────────────────────────────────────────
//
// Uses the Windows Restart Manager API to enumerate every process (other than
// Notepad++ itself) that currently has 'path' open.  Returns their EXE
// basenames (e.g. L"notepad.exe"), falling back to the RM friendly app name
// if the process handle cannot be opened.
//
// The Restart Manager is appropriate here: it is lightweight, requires no
// elevated access, and correctly detects modern applications that hold their
// file handles open during editing (Windows 11 Notepad, Word, VS Code, etc.).
//
// Limitation: applications that open-read-close (i.e. read file content into
// memory and then release the handle) are not detectable by any handle-based
// API.  See README § "Detecting concurrent file access" for details.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::wstring> getFileOwnerProcessNames(const std::wstring& path)
{
    std::vector<std::wstring> result;
    if (path.empty())
        return result;

    DWORD dwSession = 0;
    WCHAR szKey[CCH_RM_SESSION_KEY + 1] = {};
    if (::RmStartSession(&dwSession, 0, szKey) != ERROR_SUCCESS)
        return result;

    PCWSTR pszFile = path.c_str();
    if (::RmRegisterResources(dwSession, 1, &pszFile, 0, nullptr, 0, nullptr)
        == ERROR_SUCCESS)
    {
        DWORD dwReason = 0;
        UINT  nNeeded  = 0, nInfo = 0;
        DWORD err = ::RmGetList(dwSession, &nNeeded, &nInfo, nullptr, &dwReason);
        if (err == ERROR_MORE_DATA && nNeeded > 0)
        {
            std::vector<RM_PROCESS_INFO> rgpi(nNeeded);
            nInfo = nNeeded;
            err = ::RmGetList(dwSession, &nNeeded, &nInfo, rgpi.data(), &dwReason);
            if (err == ERROR_SUCCESS)
            {
                DWORD currentPid = ::GetCurrentProcessId();
                for (UINT i = 0; i < nInfo; i++)
                {
                    if (rgpi[i].Process.dwProcessId == currentPid)
                        continue;

                    std::wstring name;
                    HANDLE hProc = ::OpenProcess(
                        PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                        rgpi[i].Process.dwProcessId);
                    if (hProc)
                    {
                        wchar_t exePath[MAX_PATH] = {};
                        DWORD   len = MAX_PATH;
                        if (::QueryFullProcessImageNameW(hProc, 0, exePath, &len))
                        {
                            std::wstring full(exePath);
                            size_t pos = full.rfind(L'\\');
                            name = (pos != std::wstring::npos)
                                   ? full.substr(pos + 1) : full;
                        }
                        ::CloseHandle(hProc);
                    }
                    if (name.empty())
                        name = rgpi[i].strAppName;  // friendly name fallback
                    if (!name.empty())
                        result.push_back(name);
                }
            }
        }
    }

    ::RmEndSession(dwSession);
    return result;
}

// ── lockPath ─────────────────────────────────────────────────────────────────
//
// Acquires an exclusive lock on the given file path and records it in g_lockMap.
// Returns true if the file is now locked (freshly acquired or already held).
// Returns false if the path is empty or CreateFile() fails.
// ─────────────────────────────────────────────────────────────────────────────
static bool lockPath(const std::wstring& path)
{
    if (path.empty())
        return false;
    if (g_lockMap.count(path))
        return true;    // already locked — nothing to do

    // Check whether any foreign process currently has this file open.
    // Re-run every time so we detect when the other app has closed the file.
    std::vector<std::wstring> owners = getFileOwnerProcessNames(path);
    if (!owners.empty())
    {
        if (!g_foreignOpenPaths.count(path))
        {
            // Warn once per conflict episode; suppress until the other app closes.
            g_foreignOpenPaths.insert(path);
            std::wstringstream ss;
            ss << L"This file is already open in another application.\r\n"
                  L"FileLock will not lock it or set it read-only.\r\n\r\n"
               << path << L"\r\n\r\n"
                  L"Opened by:";
            for (const auto& n : owners)
                ss << L"\r\n  \x2022 " << n;
            ss << L"\r\n\r\n"
                  L"Close the other application and switch back to this tab\r\n"
                  L"to allow FileLock to protect the file.";
            ::MessageBoxW(g_nppData._nppHandle, ss.str().c_str(),
                          L"FileLock \x2013 File In Use", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

    // Other app has closed (or was never there) — clear any stale warning entry.
    g_foreignOpenPaths.erase(path);

    HANDLE h = acquireLock(path);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    g_lockMap[path] = h;
    if (g_addReadOnly)
        applyReadOnly(path);
    return true;
}

// ── unlockPath ────────────────────────────────────────────────────────────────
//
// Releases the lock on the given file path and removes it from g_lockMap.
// Safe to call with an empty or unlocked path (no-op).
// ─────────────────────────────────────────────────────────────────────────────
static void unlockPath(const std::wstring& path)
{
    auto it = g_lockMap.find(path);
    if (it != g_lockMap.end())
    {
        log(L"unlockPath: releasing '%s'", path.c_str());
        restoreReadOnly(path);
        releaseLock(it->second);
        g_lockMap.erase(it);
    }
}

// ── releaseAllLocks ──────────────────────────────────────────────────────────
//
// Releases every lock in g_lockMap and clears it.
// Called when automatic locking is disabled or the plugin shuts down.
// ─────────────────────────────────────────────────────────────────────────────
static void releaseAllLocks()
{
    for (auto& kv : g_lockMap)
    {
        restoreReadOnly(kv.first);
        releaseLock(kv.second);
    }
    g_lockMap.clear();
    g_readOnlyOriginals.clear();  // safety net for any entries not in g_lockMap
    g_foreignOpenPaths.clear();
    g_pendingRestorePaths.clear();
    savePendingRestoreRegistry();  // clear the crash-recovery registry entry
}


// ── findTabBarCallback ────────────────────────────────────────────────────────
//
// EnumChildWindows callback that locates Notepad++'s main document tab bar.
// Matches the first child window with class "TabBar" (Notepad++'s registered
// name for its document tab bar) or "SysTabControl32" that has at least one tab.
// ─────────────────────────────────────────────────────────────────────────────
static BOOL CALLBACK findTabBarCallback(HWND hwnd, LPARAM lParam)
{
    wchar_t cls[64] = {};
    ::GetClassNameW(hwnd, cls, _countof(cls));
    if ((::wcscmp(cls, L"TabBar") == 0 || ::wcscmp(cls, L"SysTabControl32") == 0) &&
        ::SendMessage(hwnd, TCM_GETITEMCOUNT, 0, 0) > 0)
    {
        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE;   // stop enumeration — found it
    }
    return TRUE;        // keep searching
}

// ── enumerateOpenFilePaths ────────────────────────────────────────────────────
//
// Returns the absolute paths of all real files currently open in Notepad++ by
// programmatically cycling through every tab in the document tab bar.
//
// For each tab it:
//   1. Selects the tab via TCM_SETCURSEL (standard Windows tab-control message).
//   2. Sends WM_NOTIFY / TCN_SELCHANGE to Notepad++'s main window — the same
//      notification the tab bar fires when the user clicks a tab — causing
//      Notepad++ to switch its active document and update the title bar.
//   3. Reads getCurrentFilePath() from the now-updated title bar.
//
// The originally-active tab is restored before the function returns.
// This uses no NPPM_ messages; all NPPM enumeration messages either crash or
// return wrong data in this Notepad++ build.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::wstring> enumerateOpenFilePaths()
{
    std::vector<std::wstring> result;

    // Use the cached tab bar handle, or find it now.
    if (!g_tabHwnd)
        ::EnumChildWindows(g_nppData._nppHandle, findTabBarCallback,
                           reinterpret_cast<LPARAM>(&g_tabHwnd));
    if (!g_tabHwnd)
        return result;

    int count    = static_cast<int>(::SendMessage(g_tabHwnd, TCM_GETITEMCOUNT, 0, 0));
    int original = static_cast<int>(::SendMessage(g_tabHwnd, TCM_GETCURSEL,    0, 0));
    if (count <= 0)
        return result;

    UINT_PTR ctrlId = static_cast<UINT_PTR>(::GetDlgCtrlID(g_tabHwnd));

    g_enumerating = true;   // prevent hook from locking/counting during tab cycling
    for (int i = 0; i < count; i++)
    {
        ::SendMessage(g_tabHwnd, TCM_SETCURSEL, static_cast<WPARAM>(i), 0);
        NMHDR nm = { g_tabHwnd, ctrlId, TCN_SELCHANGE };
        ::SendMessage(g_nppData._nppHandle, WM_NOTIFY,
                      static_cast<WPARAM>(ctrlId),
                      reinterpret_cast<LPARAM>(&nm));

        std::wstring path = getCurrentFilePath();
        if (!path.empty())
            result.push_back(path);
    }

    // Restore the original tab.
    ::SendMessage(g_tabHwnd, TCM_SETCURSEL, static_cast<WPARAM>(original), 0);
    NMHDR nm = { g_tabHwnd, ctrlId, TCN_SELCHANGE };
    ::SendMessage(g_nppData._nppHandle, WM_NOTIFY,
                  static_cast<WPARAM>(ctrlId),
                  reinterpret_cast<LPARAM>(&nm));
    g_enumerating = false;

    return result;
}

// ── findSetReadOnlyCmdId ──────────────────────────────────────────────────────
//
// Scans the Notepad++ Edit menu (and one level of submenus) for an item whose
// text contains both "Read" and "Only" — i.e. the "Set Read-Only" toggle.
// Returns its WM_COMMAND ID and caches it in g_setReadOnlyCmdId.
// Returns 0 if the item cannot be found.
//
// This command is used to clear buffer._isReadOnly before a save: NPPM_SAVEFILE
// and the standard IDM_FILE_SAVE both check buffer._isReadOnly internally and
// refuse to write if it is true.  Toggling it false first lets the save proceed.
// ─────────────────────────────────────────────────────────────────────────────
static UINT findSetReadOnlyCmdId()
{
    if (g_setReadOnlyCmdId) return g_setReadOnlyCmdId;

    HMENU hMenu = ::GetMenu(g_nppData._nppHandle);
    if (!hMenu) return 0;

    int topCount = ::GetMenuItemCount(hMenu);
    for (int i = 0; i < topCount; i++)
    {
        wchar_t topText[64] = {};
        ::GetMenuStringW(hMenu, i, topText, _countof(topText), MF_BYPOSITION);
        if (!::wcsstr(topText, L"Edit") && !::wcsstr(topText, L"edit")) continue;

        HMENU hEditMenu = ::GetSubMenu(hMenu, i);
        if (!hEditMenu) break;

        int editCount = ::GetMenuItemCount(hEditMenu);
        for (int j = 0; j < editCount; j++)
        {
            // Check direct items
            UINT cmdId = ::GetMenuItemID(hEditMenu, j);
            if (cmdId && cmdId != (UINT)-1)
            {
                wchar_t text[128] = {};
                ::GetMenuStringW(hEditMenu, j, text, _countof(text), MF_BYPOSITION);
                if (::wcsstr(text, L"Read") != nullptr && ::wcsstr(text, L"Only") != nullptr)
                {
                    g_setReadOnlyCmdId = cmdId;
                    log(L"findSetReadOnlyCmdId: '%s' id=%u", text, cmdId);
                    return cmdId;
                }
            }
            // Check one level of submenu
            HMENU hSub = ::GetSubMenu(hEditMenu, j);
            if (!hSub) continue;
            int subCount = ::GetMenuItemCount(hSub);
            for (int k = 0; k < subCount; k++)
            {
                UINT sid = ::GetMenuItemID(hSub, k);
                if (!sid || sid == (UINT)-1) continue;
                wchar_t stext[128] = {};
                ::GetMenuStringW(hSub, k, stext, _countof(stext), MF_BYPOSITION);
                if (::wcsstr(stext, L"Read") != nullptr && ::wcsstr(stext, L"Only") != nullptr)
                {
                    g_setReadOnlyCmdId = sid;
                    log(L"findSetReadOnlyCmdId (sub): '%s' id=%u", stext, sid);
                    return sid;
                }
            }
        }
        break;  // found Edit menu, stop looking at top-level items
    }

    log(L"findSetReadOnlyCmdId: NOT FOUND in Edit menu");
    return 0;
}

// ── findClearReadOnlyCmdId ────────────────────────────────────────────────────
//
// Scans the Notepad++ Edit menu for the item whose text contains "Clear" AND
// either "Read" or "Only" — i.e. "Clear Read-Only Flag".
// This is the directional opposite of "Set Read-Only": it only makes a buffer
// writable, never makes it read-only.  Caches the result in g_clearReadOnlyCmdId.
// Returns 0 if not found.
// ─────────────────────────────────────────────────────────────────────────────
static UINT findClearReadOnlyCmdId()
{
    if (g_clearReadOnlyCmdId) return g_clearReadOnlyCmdId;

    HMENU hMenu = ::GetMenu(g_nppData._nppHandle);
    if (!hMenu) return 0;

    int topCount = ::GetMenuItemCount(hMenu);
    for (int i = 0; i < topCount; i++)
    {
        wchar_t topText[64] = {};
        ::GetMenuStringW(hMenu, i, topText, _countof(topText), MF_BYPOSITION);
        if (!::wcsstr(topText, L"Edit") && !::wcsstr(topText, L"edit")) continue;

        HMENU hEditMenu = ::GetSubMenu(hMenu, i);
        if (!hEditMenu) break;

        int editCount = ::GetMenuItemCount(hEditMenu);
        for (int j = 0; j < editCount; j++)
        {
            // Check direct items
            UINT cmdId = ::GetMenuItemID(hEditMenu, j);
            if (cmdId && cmdId != (UINT)-1)
            {
                wchar_t text[128] = {};
                ::GetMenuStringW(hEditMenu, j, text, _countof(text), MF_BYPOSITION);
                if (::wcsstr(text, L"Clear") != nullptr
                    && (::wcsstr(text, L"Read") != nullptr || ::wcsstr(text, L"Only") != nullptr))
                {
                    g_clearReadOnlyCmdId = cmdId;
                    log(L"findClearReadOnlyCmdId: '%s' id=%u", text, cmdId);
                    return cmdId;
                }
            }
            // Check one level of submenu
            HMENU hSub = ::GetSubMenu(hEditMenu, j);
            if (!hSub) continue;
            int subCount = ::GetMenuItemCount(hSub);
            for (int k = 0; k < subCount; k++)
            {
                UINT sid = ::GetMenuItemID(hSub, k);
                if (!sid || sid == (UINT)-1) continue;
                wchar_t stext[128] = {};
                ::GetMenuStringW(hSub, k, stext, _countof(stext), MF_BYPOSITION);
                if (::wcsstr(stext, L"Clear") != nullptr
                    && (::wcsstr(stext, L"Read") != nullptr || ::wcsstr(stext, L"Only") != nullptr))
                {
                    g_clearReadOnlyCmdId = sid;
                    log(L"findClearReadOnlyCmdId (sub): '%s' id=%u", stext, sid);
                    return sid;
                }
            }
        }
        break;  // found Edit menu
    }

    log(L"findClearReadOnlyCmdId: NOT FOUND in Edit menu");
    return 0;
}

// ── ensureBufferWritable ──────────────────────────────────────────────────────
//
// Sends the Edit > Set Read-Only toggle and checks whether the toggle went in
// the correct direction (buffer._isReadOnly → false).  If it went the wrong
// way (buffer was already writable, so we accidentally made it read-only), a
// second toggle corrects the state.
//
// NPPN_READONLYCHANGED fires synchronously inside SendMessage.  When
// g_toggleInProgress is set, the handler captures docStatus rather than
// posting WM_FL_CLEAR_RO, and sets g_lastToggleDocStatus.
//
// docStatus & DOCSTATUS_READONLY = 1 → buffer BECAME read-only (wrong direction)
// docStatus & DOCSTATUS_READONLY = 0 → buffer BECAME writable (correct)
//
// Returns true if buffer ended up writable, false if the command was not found.
// ─────────────────────────────────────────────────────────────────────────────
static bool ensureBufferWritable()
{
    UINT roCmd = findSetReadOnlyCmdId();
    if (!roCmd)
    {
        log(L"ensureBufferWritable: Set Read-Only command not found");
        return false;
    }

    // First toggle — direction unknown.
    g_toggleInProgress   = true;
    g_lastToggleDocStatus = 0;
    ::SendMessage(g_nppData._nppHandle, WM_COMMAND, MAKEWPARAM(roCmd, 0), 0);
    g_toggleInProgress = false;

    UINT result = g_lastToggleDocStatus;
    g_lastToggleDocStatus = 0;

    if (result & DOCSTATUS_READONLY)
    {
        // Wrong direction: buffer was already writable, we made it read-only.
        // Send a second toggle to restore writable state.
        ::SendMessage(g_nppData._nppHandle, WM_COMMAND, MAKEWPARAM(roCmd, 0), 0);
        log(L"ensureBufferWritable: corrected direction (first docStatus=%u)", result);
    }
    else
    {
        log(L"ensureBufferWritable: buffer now writable (docStatus=%u)", result);
    }
    return true;
}

// ── clearSciReadOnly ──────────────────────────────────────────────────────────
//
// Sends SCI_SETREADONLY 0 to both Scintilla views to ensure pdoc->readonly is
// false.  Called during tab switches and window activation to counter any
// SCI_SETREADONLY 1 that slipped past sciSubclassProc.
//
// Note: there is no NPPM_SETBUFFERREADONLY in this Notepad++ API version.
// Notepad++'s internal buffer._isReadOnly is not directly clearable via the
// plugin API.  For saving, NPPM_SAVEFILE (NPPMSG+94) is used instead, which
// bypasses the buffer._isReadOnly check entirely.
// ─────────────────────────────────────────────────────────────────────────────
static void clearSciReadOnly(const std::wstring& path)
{
    if (!g_addReadOnly || path.empty()) return;
    if (!g_pendingRestorePaths.count(path)) return;  // originally read-only: leave alone

    ::SendMessage(g_nppData._scintillaMainHandle,   SCI_SETREADONLY, 0, 0);
    ::SendMessage(g_nppData._scintillaSecondHandle, SCI_SETREADONLY, 0, 0);
    log(L"clearSciReadOnly: SCI_SETREADONLY 0 applied for '%s'", path.c_str());
}

// ── cmdHookProc ───────────────────────────────────────────────────────────────
//
// WH_CALLWNDPROC hook procedure.  Fires BEFORE a sent message reaches its
// destination window procedure.  Unlike WH_CALLWNDPROCRET (which fires after),
// modifications to the CWPSTRUCT here take effect — the window proc receives
// our modified wParam.
//
// We intercept SCI_SETREADONLY 1 sent to either Scintilla view and change the
// wParam to 0 so Scintilla processes SCI_SETREADONLY(0) instead.  This prevents
// pdoc->readonly from being set to true while keeping the document writable.
// Only fires when g_pendingRestorePaths is non-empty (i.e., we have at least
// one file whose original attribute was writable but we set FILE_ATTRIBUTE_READONLY
// on it), so genuinely originally-read-only files are unaffected.
// ─────────────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK cmdHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && !g_enumerating)
    {
        const CWPSTRUCT* p = reinterpret_cast<const CWPSTRUCT*>(lParam);

        // Track Notepad++ focus and manage FILE_ATTRIBUTE_READONLY.
        //
        // Strategy: FILE_ATTRIBUTE_READONLY is ONLY on disk while Notepad++ does NOT
        // have focus.  While focused, the attribute is always clear so tab switches,
        // saves, and file-monitor callbacks never trigger the grey-icon cycle.
        //
        // WA_INACTIVE → Notepad++ losing focus: set FILE_ATTRIBUTE_READONLY on all
        //               files we manage (external protection while user is elsewhere).
        // WA_ACTIVE   → Notepad++ gaining focus: clear FILE_ATTRIBUTE_READONLY on all
        //               files so Notepad++ sees them as writable during activation.
        if (p->hwnd == g_nppData._nppHandle && p->message == WM_ACTIVATE
            && g_preRenameLockedPaths.empty())
        {
            UINT activationState = LOWORD(p->wParam);
            if (activationState == WA_INACTIVE)
            {
                g_nppActive = false;
                log(L"FOCUS LOST: Notepad++ losing focus — applying FILE_ATTRIBUTE_READONLY");
                if (g_addReadOnly && !g_pendingRestorePaths.empty())
                {
                    for (const auto& path : g_pendingRestorePaths)
                    {
                        DWORD attrs = ::GetFileAttributesW(path.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_READONLY))
                            ::SetFileAttributesW(path.c_str(), attrs | FILE_ATTRIBUTE_READONLY);
                    }
                    log(L"WH_CALLWNDPROC WA_INACTIVE: FILE_ATTRIBUTE_READONLY set on %zu file(s)",
                        g_pendingRestorePaths.size());
                }
            }
            else
            {
                g_nppActive = true;
                log(L"FOCUS GAINED: Notepad++ gaining focus (WA=%u) — clearing FILE_ATTRIBUTE_READONLY", activationState);
                if (g_addReadOnly && !g_pendingRestorePaths.empty())
                {
                    for (const auto& path : g_pendingRestorePaths)
                    {
                        DWORD attrs = ::GetFileAttributesW(path.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                            ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                    }
                    log(L"WH_CALLWNDPROC WA_ACTIVE: FILE_ATTRIBUTE_READONLY cleared on %zu file(s)",
                        g_pendingRestorePaths.size());
                }
            }
        }

        // WM_NOTIFY (TCN_SELCHANGE) — user is switching tabs.
        // No longer touches FILE_ATTRIBUTE_READONLY here: the attribute is managed
        // purely by focus changes (WA_INACTIVE/WA_ACTIVE).  Tab switches happen
        // only while Notepad++ is focused, so the attribute is already clear.
        if (p->hwnd    == g_nppData._nppHandle
            && p->message == WM_NOTIFY
            && !g_enumerating
            && g_preRenameLockedPaths.empty())
        {
            const NMHDR* nm = reinterpret_cast<const NMHDR*>(p->lParam);
            if (nm && nm->code == TCN_SELCHANGE)
                log(L"TAB SWITCH: TCN_SELCHANGE fired");
        }

        // WM_COMMAND IDM_FILE_SAVE / IDM_FILE_SAVEALL (Ctrl+S — sent message path).
        // Strategy: buffer._isReadOnly=true blocks every save API including NPPM_SAVEFILE.
        // Fix: (1) clear FILE_ATTRIBUTE_READONLY from disk, (2) send the Edit > Set
        // Read-Only toggle to flip buffer._isReadOnly false, (3) let the original
        // IDM_FILE_SAVE command proceed normally — Notepad++ will write the file.
        // FILE_ATTRIBUTE_READONLY is restored by WM_SETTEXT after the write.
        if (p->hwnd == g_nppData._nppHandle && p->message == WM_COMMAND)
        {
            UINT cmdId = LOWORD(p->wParam);

            if ((cmdId == IDM_FILE_SAVE || cmdId == IDM_FILE_SAVEALL)
                && !g_saveInProgress && g_addReadOnly && !g_readOnlyOriginals.empty())
            {
                log(L"cmdHookProc WM_COMMAND id=%u (sent/Ctrl+S) addRO=%d originals=%zu pending=%zu",
                    cmdId, (int)g_addReadOnly, g_readOnlyOriginals.size(),
                    g_pendingRestorePaths.size());

                if (cmdId == IDM_FILE_SAVE)
                {
                    std::wstring path = getCurrentFilePath();
                    log(L"cmdHookProc IDM_FILE_SAVE: path='%s' inOriginals=%d inPending=%d",
                        path.empty() ? L"(empty)" : path.c_str(),
                        (int)(path.empty() ? 0 : g_readOnlyOriginals.count(path)),
                        (int)(path.empty() ? 0 : g_pendingRestorePaths.count(path)));

                    if (!path.empty() && g_readOnlyOriginals.count(path)
                        && g_pendingRestorePaths.count(path))
                    {
                        // Step 1: clear FILE_ATTRIBUTE_READONLY from disk so the OS
                        // allows Notepad++ to open the file for GENERIC_WRITE.
                        DWORD attrs = ::GetFileAttributesW(path.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                        {
                            ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                            g_saveClearedPaths.push_back(path);
                            log(L"cmdHookProc IDM_FILE_SAVE: FILE_ATTRIBUTE_READONLY cleared (attrs was 0x%lX)", attrs);
                        }
                        else
                        {
                            // Already clear (e.g. WM_ACTIVATE cleared it) — still track for restore.
                            g_saveClearedPaths.push_back(path);
                            log(L"cmdHookProc IDM_FILE_SAVE: attrs=0x%lX  FILE_ATTRIBUTE_READONLY already clear", attrs);
                        }

                        // Step 2: clear buffer._isReadOnly using "Clear Read-Only Flag".
                        // Notepad++ checks buffer._isReadOnly BEFORE attempting the write.
                        // If it is true, the save is silently skipped and NPPN_FILESAVED
                        // never fires.  The file monitor sets buffer._isReadOnly=true ~50ms
                        // after we apply FILE_ATTRIBUTE_READONLY.  WM_FL_CLEAR_RO clears it
                        // for the normal case, but as a safety net we also clear it here.
                        // "Clear Read-Only Flag" is one-directional (only clears), unlike
                        // "Set Read-Only" (42028) which is a toggle that can go either way.
                        UINT clearCmd = findClearReadOnlyCmdId();
                        if (clearCmd)
                        {
                            LRESULT cr = ::SendMessage(g_nppData._nppHandle,
                                                       WM_COMMAND,
                                                       MAKEWPARAM(clearCmd, 0), 0);
                            log(L"cmdHookProc IDM_FILE_SAVE: ClearReadOnly cmd=%u result=%d  (buffer._isReadOnly cleared)",
                                clearCmd, (int)cr);
                        }
                        else
                        {
                            log(L"cmdHookProc IDM_FILE_SAVE: ClearReadOnly cmd NOT FOUND — save may fail if buffer._isReadOnly=true");
                        }

                        // Step 3: return — let the original IDM_FILE_SAVE be processed.
                        // With FILE_ATTRIBUTE_READONLY clear (step 1) AND buffer._isReadOnly
                        // false (step 2), Notepad++ can write the file normally.
                        // FILE_ATTRIBUTE_READONLY is restored by WM_SETTEXT after the
                        // title bar updates (asterisk disappears post-save).
                        log(L"cmdHookProc IDM_FILE_SAVE: both blockers cleared — letting original save proceed");
                    }
                }
                else if (cmdId == IDM_FILE_SAVEALL)
                {
                    UINT roCmd = findSetReadOnlyCmdId();
                    log(L"cmdHookProc IDM_FILE_SAVEALL: roCmd=%u pending=%zu",
                        roCmd, g_pendingRestorePaths.size());

                    for (const auto& sp : g_pendingRestorePaths)
                    {
                        if (!g_readOnlyOriginals.count(sp)) continue;
                        DWORD attrs = ::GetFileAttributesW(sp.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                        {
                            ::SetFileAttributesW(sp.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                            g_saveClearedPaths.push_back(sp);
                        }
                    }
                    if (roCmd && !g_pendingRestorePaths.empty())
                    {
                        g_saveInProgress = true;
                        ::SendMessage(g_nppData._nppHandle, WM_COMMAND, MAKEWPARAM(roCmd, 0), 0);
                        g_saveInProgress = false;
                        log(L"cmdHookProc IDM_FILE_SAVEALL: buffer._isReadOnly toggled off");
                    }
                }
            }
        }
    }
    return ::CallNextHookEx(g_hCmdHook, nCode, wParam, lParam);
}

// ── sciSubclassProc ──────────────────────────────────────────────────────────
//
// Window subclass procedure installed on both Scintilla views.
// Intercepts SCI_SETREADONLY 1 and passes 0 to DefSubclassProc instead.
//
// Unlike WH_CALLWNDPROC (where MSDN says parameters cannot be modified), a
// subclass proc is directly in the call chain and passes its own wParam to the
// original window procedure via DefSubclassProc — the change is guaranteed to
// take effect.
//
// This prevents Notepad++ from setting pdoc->readonly=true via SCI_SETREADONLY 1,
// which cannot be undone via the Scintilla message API (SCI_SETREADONLY 0 only
// clears the view flag, not pdoc->readonly).
//
// Only intercepts when g_pendingRestorePaths is non-empty, meaning we have at
// least one file whose FILE_ATTRIBUTE_READONLY was set by this plugin (i.e., the
// file was originally writable).  Files that were read-only before being opened
// are not in g_pendingRestorePaths and are therefore unaffected.
// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic counters for sciSubclassProc — shown in Show Log.
static int g_subclassCallCount      = 0;  // total SCI_SETREADONLY calls seen
static int g_subclassInterceptCount = 0;  // times we changed 1 → 0

static LRESULT CALLBACK sciSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR /*uId*/,
                                         DWORD_PTR /*dwRef*/)
{
    if (msg == SCI_SETREADONLY)
    {
        ++g_subclassCallCount;
        dbg(L"FL: sciSubclass SCI_SETREADONLY wParam=%d addRO=%d pending=%zu\n",
            (int)wParam, (int)g_addReadOnly, g_pendingRestorePaths.size());

        if (g_addReadOnly && wParam == 1 && !g_pendingRestorePaths.empty())
        {
            ++g_subclassInterceptCount;
            wParam = 0;
            dbg(L"FL: sciSubclass intercepted → changed to 0\n");
        }
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ── getMsgHookProc ────────────────────────────────────────────────────────────
//
// WH_GETMESSAGE hook procedure.  Fires when a posted message is retrieved from
// the Notepad++ UI thread's message queue (inside GetMessage or PeekMessage),
// BEFORE DispatchMessage is called for that message.
//
// Menu commands (including right-click tab → Rename) arrive via PostMessage,
// not SendMessage, so they are invisible to WH_CALLWNDPROC/RET.  This hook
// is the correct place to intercept them.
//
// When WM_COMMAND / IDM_FILE_RENAME (41017) is retrieved, we:
//   1. Record the locked path in g_preRenameUnlockedPath.
//   2. Release our lock handle (CloseHandle).
// After the hook returns, DispatchMessage is called: Notepad++ shows the
// rename dialog, the user confirms, MoveFileExW succeeds (our handle is gone),
// and Notepad++ calls SetWindowText with the new title.
//
// The lock is then re-acquired in titleBarHookProc:
//   • WM_SETTEXT (title changed)  → rename succeeded → lock new path.
//   • WM_ACTIVATE (main window re-activated, old file on disk) → cancelled →
//     restore lock on original path.
// ─────────────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK getMsgHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    // wParam == PM_REMOVE means the message is being consumed from the queue.
    if (nCode == HC_ACTION && wParam == PM_REMOVE)
    {
        const MSG* msg = reinterpret_cast<const MSG*>(lParam);

        // Deferred startup lock-all: posted as a thread message (hwnd == nullptr)
        // so that enumerateOpenFilePaths() runs in the normal message loop context
        // rather than from within the WH_CALLWNDPROCRET hook proc where Notepad++'s
        // re-entrancy guard prevents title-bar updates during tab cycling.
        if (msg->hwnd == nullptr && msg->message == WM_FL_LOCK_ALL)
        {
            if (g_lockingEnabled)
            {
                std::vector<std::wstring> allPaths = enumerateOpenFilePaths();
                for (const auto& p : allPaths)
                    lockPath(p);

                log(L"WM_FL_LOCK_ALL: locked %zu files  addRO=%d  pending=%zu",
                    allPaths.size(), (int)g_addReadOnly, g_pendingRestorePaths.size());
                // Fix startup active tab via the proper API.
                std::wstring activePath = getCurrentFilePath();
                if (!activePath.empty()) clearSciReadOnly(activePath);
            }
        }

        // Deferred tab-close check.  Posted by the WM_SETTEXT handler when the
        // tab count drops, because enumerateOpenFilePaths() fails when called
        // from inside WH_CALLWNDPROCRET (Notepad++ re-entrancy guard).
        if (msg->hwnd == nullptr && msg->message == WM_FL_CHECK_CLOSE)
        {
            if (!g_lockMap.empty() || !g_foreignOpenPaths.empty())
            {
                std::vector<std::wstring> openPaths = enumerateOpenFilePaths();
                log(L"WM_FL_CHECK_CLOSE: open=%zu  locked=%zu",
                    openPaths.size(), g_lockMap.size());

                std::vector<std::wstring> toUnlock;
                for (const auto& kv : g_lockMap)
                {
                    bool found = false;
                    for (const auto& op : openPaths)
                        if (op == kv.first) { found = true; break; }
                    if (!found)
                        toUnlock.push_back(kv.first);
                }
                for (const auto& p : toUnlock)
                    unlockPath(p);

                for (auto it = g_foreignOpenPaths.begin();
                     it != g_foreignOpenPaths.end(); )
                {
                    bool found = false;
                    for (const auto& op : openPaths)
                        if (op == *it) { found = true; break; }
                    it = found ? ++it : g_foreignOpenPaths.erase(it);
                }
            }
        }

        // Deferred Scintilla read-only clear.  Posted by NPPN_READONLYCHANGED
        // because handling it synchronously inside the notification is re-entrant.
        // Here we are in the normal message loop, fully unwound.
        // wParam = buffer ID (used for path lookup only).
        if (msg->hwnd == nullptr && msg->message == WM_FL_CLEAR_RO)
        {
            uptr_t roBufferId = static_cast<uptr_t>(msg->wParam);
            if (g_addReadOnly && roBufferId)
            {
                std::wstring path;
                auto it = g_idToPath.find(roBufferId);
                if (it != g_idToPath.end()) path = it->second;
                else path = getCurrentFilePath();

                if (!path.empty() && g_pendingRestorePaths.count(path))
                {
                    // Clear Scintilla's pdoc->readonly so the editor remains editable.
                    // buffer._isReadOnly is handled by NPPN_BUFFERACTIVATED (active buf)
                    // and WA_ACTIVE handler (on focus regain) using ClearReadOnly command.
                    ::SendMessage(g_nppData._scintillaMainHandle,   SCI_SETREADONLY, 0, 0);
                    ::SendMessage(g_nppData._scintillaSecondHandle, SCI_SETREADONLY, 0, 0);
                    log(L"WM_FL_CLEAR_RO: SCI_SETREADONLY 0 applied for bufferId=%llu path='%s'",
                        static_cast<unsigned long long>(roBufferId), path.c_str());
                }
            }
        }

        if (msg->message == WM_COMMAND)
        {
            UINT cmdId = LOWORD(msg->wParam);

            // Log every WM_COMMAND ID so the correct delete command can be identified
            // in the event log when Enable Logging is on.
            log(L"WM_COMMAND id=%u  locked=%zu", cmdId, g_lockMap.size());

            if (cmdId == IDM_FILE_RENAME && !g_lockMap.empty())
            {
                // Release every lock so that whichever file Notepad++ renames
                // (active tab or not) is not blocked by our handle.
                g_preRenameLockedPaths.clear();
                g_renameOpDispatched = false;
                for (const auto& kv : g_lockMap)
                    g_preRenameLockedPaths.push_back(kv.first);
                for (const auto& p : g_preRenameLockedPaths)
                    unlockPath(p);
            }

            // IDM_FILE_RENAME_EXEC (22003) fires after the rename dialog closes,
            // just before Notepad++ calls MoveFileExW.  WM_ACTIVATE fires first
            // (dialog closed), which is why we must NOT re-lock on WM_ACTIVATE.
            // Re-release any locks that WM_ACTIVATE may have restored, and set
            // the flag so the next WM_SETTEXT resolves cancel vs success.
            if (cmdId == IDM_FILE_RENAME_EXEC && !g_preRenameLockedPaths.empty())
            {
                g_renameOpDispatched = true;
                for (const auto& p : g_preRenameLockedPaths)
                    if (g_lockMap.count(p)) unlockPath(p);
            }

            if (cmdId == IDM_FILE_DELETE && !g_lockMap.empty())
            {
                // Release ALL locks so the right-clicked file (which may not be
                // the active tab) is not blocked by our handle.  unlockPath()
                // calls restoreReadOnly(), which restores FILE_ATTRIBUTE_READONLY
                // to its pre-plugin state so the shell sees the file as it was
                // before we touched it.  The outcome is resolved in WM_SETTEXT
                // once IDM_FILE_DELETE_EXEC (22007) confirms the operation ran.
                g_preDeleteLockedPaths.clear();
                g_deleteOpDispatched = false;
                for (const auto& kv : g_lockMap)
                    g_preDeleteLockedPaths.push_back(kv.first);
                log(L"IDM_FILE_DELETE (41016): releasing %zu lock(s)",
                    g_preDeleteLockedPaths.size());
                for (const auto& p : g_preDeleteLockedPaths)
                    unlockPath(p);
            }

            // IDM_FILE_DELETE_EXEC (22007) fires after the confirmation dialog
            // closes, just before SHFileOperationW.  WM_ACTIVATE fires first
            // (dialog closed, Notepad++ regains focus) — do NOT re-lock there.
            // Re-release any locks accidentally re-acquired between 41016 and now,
            // and set the flag so the next WM_SETTEXT resolves cancel vs. success.
            if (cmdId == IDM_FILE_DELETE_EXEC && !g_preDeleteLockedPaths.empty())
            {
                g_deleteOpDispatched = true;
                for (const auto& p : g_preDeleteLockedPaths)
                    if (g_lockMap.count(p)) unlockPath(p);
                log(L"IDM_FILE_DELETE_EXEC (22007): delete operation dispatched");
            }

            // Menu-initiated saves (IDM_FILE_SAVE via File menu — posted message path).
            // Ctrl+S is handled by cmdHookProc (WH_CALLWNDPROC, sent messages).
            // Same strategy: clear FILE_ATTRIBUTE_READONLY, toggle buffer._isReadOnly,
            // then let the original IDM_FILE_SAVE be dispatched normally.
            if ((cmdId == IDM_FILE_SAVE || cmdId == IDM_FILE_SAVEALL)
                && !g_saveInProgress && g_addReadOnly && !g_readOnlyOriginals.empty())
            {
                log(L"getMsgHook WM_COMMAND id=%u (posted/menu) originals=%zu pending=%zu",
                    cmdId, g_readOnlyOriginals.size(), g_pendingRestorePaths.size());

                if (cmdId == IDM_FILE_SAVE)
                {
                    std::wstring path = getCurrentFilePath();
                    log(L"getMsgHook IDM_FILE_SAVE: path='%s' inOriginals=%d inPending=%d",
                        path.empty() ? L"(empty)" : path.c_str(),
                        (int)(path.empty() ? 0 : g_readOnlyOriginals.count(path)),
                        (int)(path.empty() ? 0 : g_pendingRestorePaths.count(path)));

                    if (!path.empty() && g_readOnlyOriginals.count(path)
                        && g_pendingRestorePaths.count(path))
                    {
                        // Step 1: clear FILE_ATTRIBUTE_READONLY from disk.
                        DWORD attrs = ::GetFileAttributesW(path.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                        {
                            ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                            g_saveClearedPaths.push_back(path);
                            log(L"getMsgHook IDM_FILE_SAVE: FILE_ATTRIBUTE_READONLY cleared (attrs was 0x%lX)", attrs);
                        }
                        else
                        {
                            g_saveClearedPaths.push_back(path);
                            log(L"getMsgHook IDM_FILE_SAVE: attrs=0x%lX  FILE_ATTRIBUTE_READONLY already clear", attrs);
                        }

                        // Step 2: clear buffer._isReadOnly using "Clear Read-Only Flag".
                        UINT clearCmd = findClearReadOnlyCmdId();
                        if (clearCmd)
                        {
                            LRESULT cr = ::SendMessage(g_nppData._nppHandle,
                                                       WM_COMMAND,
                                                       MAKEWPARAM(clearCmd, 0), 0);
                            log(L"getMsgHook IDM_FILE_SAVE: ClearReadOnly cmd=%u result=%d  (buffer._isReadOnly cleared)",
                                clearCmd, (int)cr);
                        }
                        else
                        {
                            log(L"getMsgHook IDM_FILE_SAVE: ClearReadOnly cmd NOT FOUND — save may fail");
                        }

                        // Step 3: let DispatchMessage handle the original IDM_FILE_SAVE.
                        log(L"getMsgHook IDM_FILE_SAVE: both blockers cleared — letting original save proceed");
                    }
                }
                else if (cmdId == IDM_FILE_SAVEALL)
                {
                    UINT roCmd = findSetReadOnlyCmdId();
                    log(L"getMsgHook IDM_FILE_SAVEALL: roCmd=%u pending=%zu",
                        roCmd, g_pendingRestorePaths.size());

                    for (const auto& sp : g_pendingRestorePaths)
                    {
                        if (!g_readOnlyOriginals.count(sp)) continue;
                        DWORD attrs = ::GetFileAttributesW(sp.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                        {
                            ::SetFileAttributesW(sp.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                            g_saveClearedPaths.push_back(sp);
                        }
                    }
                    if (roCmd && !g_pendingRestorePaths.empty())
                    {
                        g_saveInProgress = true;
                        ::SendMessage(g_nppData._nppHandle, WM_COMMAND, MAKEWPARAM(roCmd, 0), 0);
                        g_saveInProgress = false;
                        log(L"getMsgHook IDM_FILE_SAVEALL: buffer._isReadOnly toggled off");
                    }
                }
            }
        }
    }
    return ::CallNextHookEx(g_hGetMsgHook, nCode, wParam, lParam);
}

// ── titleBarHookProc ─────────────────────────────────────────────────────────
//
// WH_CALLWNDPROCRET hook procedure.  Fires after every message is fully
// processed by the target window.  We watch for WM_SETTEXT on Notepad++'s
// main window, which is sent (via SetWindowText) whenever the title bar
// changes — including on every tab switch and every new file open.
//
// After the hook fires, the title bar is up to date, so getCurrentFilePath()
// returns the correct path of the now-active file.  If locking is enabled and
// that file is not yet locked, we lock it immediately.
//
// Per MSDN: for same-process thread hooks, hMod passed to SetWindowsHookEx
// must be NULL.  CallNextHookEx must always be called to allow other hooks.
// ─────────────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK titleBarHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && !g_enumerating)
    {
        const CWPRETSTRUCT* p = reinterpret_cast<const CWPRETSTRUCT*>(lParam);

        // ── SCI_SETREADONLY intercept ─────────────────────────────────────────
        // When Add Read-only is enabled, applyReadOnly() sets FILE_ATTRIBUTE_READONLY
        // on locked files.  Notepad++ detects this (at tab-switch time and via its
        // async file-change monitor) and calls SCI_SETREADONLY 1 on the Scintilla
        // view, making the editor non-editable.  We intercept that call here:
        // WH_CALLWNDPROCRET fires synchronously after SCI_SETREADONLY 1 completes,
        // so immediately sending SCI_SETREADONLY 0 reverses the effect before any
        // user input is processed.  We only do this for files whose original attrs
        // did NOT include FILE_ATTRIBUTE_READONLY (i.e., files WE made read-only,
        // not files that were read-only before the user opened them).
        if (p->message == SCI_SETREADONLY
            && (p->hwnd == g_nppData._scintillaMainHandle
                || p->hwnd == g_nppData._scintillaSecondHandle))
        {
            dbg(L"FL: SCI_SETREADONLY intercepted  wParam=%d  addRO=%d  originals=%zu\n",
                (int)p->wParam, (int)g_addReadOnly, g_readOnlyOriginals.size());

            if (g_addReadOnly && p->wParam == 1)
            {
                bool weOwn = false;
                for (const auto& kv : g_readOnlyOriginals)
                    if (!(kv.second & FILE_ATTRIBUTE_READONLY)) { weOwn = true; break; }

                dbg(L"FL:   weOwn=%d — %s\n", (int)weOwn,
                    weOwn ? L"sending SCI_SETREADONLY 0" : L"NOT overriding");

                if (weOwn)
                {
                    ::SendMessage(p->hwnd, SCI_SETREADONLY, 0, 0);
                    LRESULT now = ::SendMessage(p->hwnd, SCI_GETREADONLY, 0, 0);
                    dbg(L"FL:   SCI_GETREADONLY after override = %d\n", (int)now);
                }
            }
        }

        if (p->hwnd == g_nppData._nppHandle && p->message == WM_SETTEXT)
        {
            // Lazily cache the tab bar handle if not yet found.
            if (!g_tabHwnd)
                ::EnumChildWindows(g_nppData._nppHandle, findTabBarCallback,
                                   reinterpret_cast<LPARAM>(&g_tabHwnd));

            if (g_tabHwnd)
            {
                int currentCount = static_cast<int>(
                    ::SendMessage(g_tabHwnd, TCM_GETITEMCOUNT, 0, 0));

                // Tab count decreased → a tab was closed → release any locks
                // whose files are no longer open.
                // NPPN_FILEBEFORECLOSE does not fire in this build, so this
                // is the only reliable close-detection mechanism.
                if (currentCount < g_prevTabCount
                    && (!g_lockMap.empty() || !g_foreignOpenPaths.empty()))
                {
                    // enumerateOpenFilePaths() fails when called from within
                    // WH_CALLWNDPROCRET — Notepad++'s re-entrancy guard prevents
                    // title-bar updates during hook execution, so every tab returns
                    // the same path and close detection unlocks everything else.
                    // Defer to the normal message loop where enumeration works.
                    log(L"TAB CLOSE DETECTED: currentCount=%d prevTabCount=%d — deferring",
                        currentCount, g_prevTabCount);
                    ::PostThreadMessageW(::GetCurrentThreadId(), WM_FL_CHECK_CLOSE, 0, 0);
                }
                g_prevTabCount = currentCount;
            }

            // Determine the path now shown in the (just-updated) title bar.
            std::wstring currentPath = getCurrentFilePath();

            if (!g_preDeleteLockedPaths.empty())
            {
                // Check whether any of the previously-locked files is now gone.
                std::wstring deletedPath;
                for (const auto& p : g_preDeleteLockedPaths)
                {
                    if (::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
                    {
                        deletedPath = p;
                        break;
                    }
                }

                if (!deletedPath.empty())
                {
                    // Delete succeeded — re-lock all other files.
                    log(L"Delete confirmed: '%s' moved to recycle bin",
                        deletedPath.c_str());
                    for (const auto& p : g_preDeleteLockedPaths)
                        if (p != deletedPath) lockPath(p);
                    g_preDeleteLockedPaths.clear();
                    g_deleteOpDispatched = false;
                    // Auto-lock the newly active file.
                    if (g_lockingEnabled && !currentPath.empty()
                        && !g_lockMap.count(currentPath))
                        lockPath(currentPath);
                }
                else if (g_deleteOpDispatched)
                {
                    // IDM_FILE_DELETE_EXEC fired but no file disappeared →
                    // delete was cancelled or failed.  Restore all locks.
                    log(L"Delete cancelled: re-locking %zu file(s)",
                        g_preDeleteLockedPaths.size());
                    for (const auto& p : g_preDeleteLockedPaths)
                        lockPath(p);
                    g_preDeleteLockedPaths.clear();
                    g_deleteOpDispatched = false;
                }
                // else: IDM_FILE_DELETE_EXEC hasn't fired yet (dialog still open
                // or WM_SETTEXT is a title refresh mid-dialog).  Wait.
            }
            else if (!g_preRenameLockedPaths.empty())
            {
                // Check whether any of the previously-locked files is now gone.
                std::wstring renamedFrom;
                for (const auto& p : g_preRenameLockedPaths)
                {
                    if (::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
                    {
                        renamedFrom = p;
                        break;
                    }
                }

                if (!renamedFrom.empty() && !currentPath.empty())
                {
                    // Rename succeeded — re-lock under new name.
                    for (auto& kv : g_idToPath)
                        if (kv.second == renamedFrom)
                            kv.second = currentPath;
                    for (const auto& p : g_preRenameLockedPaths)
                        lockPath(p == renamedFrom ? currentPath : p);
                    g_preRenameLockedPaths.clear();
                    g_renameOpDispatched = false;
                }
                else if (g_renameOpDispatched)
                {
                    // IDM_FILE_RENAME_EXEC fired but no file disappeared →
                    // rename was cancelled or failed.  Restore all locks.
                    for (const auto& p : g_preRenameLockedPaths)
                        lockPath(p);
                    g_preRenameLockedPaths.clear();
                    g_renameOpDispatched = false;
                }
                // else: IDM_FILE_RENAME_EXEC hasn't fired yet (dialog still open
                // or WM_SETTEXT is a title refresh mid-dialog).  Wait.
            }
            else if (g_pendingInitialLock && g_lockingEnabled)
            {
                // Defer the lock-all to the normal message loop via a posted thread
                // message.  Calling enumerateOpenFilePaths() directly from inside a
                // WH_CALLWNDPROCRET hook proc causes Notepad++ to skip title-bar
                // updates for all but the first TCN_SELCHANGE (re-entrancy guard),
                // so only tab 0 and the restored tab would be enumerated correctly.
                g_pendingInitialLock = false;
                ::PostThreadMessageW(::GetCurrentThreadId(), WM_FL_LOCK_ALL, 0, 0);
            }
            else if (g_lockingEnabled && !currentPath.empty()
                     && !g_lockMap.count(currentPath))
            {
                // Normal auto-lock on file open / tab switch.
                lockPath(currentPath);
            }

            if (g_addReadOnly && !currentPath.empty())
                clearSciReadOnly(currentPath);

            // Diagnostic: log the complete state after tab switch / title update
            {
                uptr_t bid = 0;
                for (const auto& kv : g_idToPath)
                    if (kv.second == currentPath) { bid = kv.first; break; }
                log(L"TAB SWITCH: WM_SETTEXT complete  path='%s'  RO=%d  bufferId=%llu",
                    currentPath.c_str(),
                    (int)::SendMessage(g_nppData._scintillaMainHandle, SCI_GETREADONLY, 0, 0),
                    (unsigned long long)bid);
            }

            // FILE_ATTRIBUTE_READONLY is managed exclusively by focus events (WA_INACTIVE /
            // WA_ACTIVE).  Do NOT restore it here: restoring after every WM_SETTEXT (tab
            // switch, save) triggers the file-monitor cycle which causes grey icons and
            // save failures for non-active buffers.  The attribute is already clear while
            // Notepad++ is focused, which is the only time WM_SETTEXT fires.
            g_saveClearedPaths.clear();  // no longer needed — no WM_SETTEXT restore
        }

        // NOTE: WM_ACTIVATE fires when the rename dialog closes, BEFORE
        // IDM_FILE_RENAME_EXEC (22003) and before MoveFileExW.  Re-locking
        // here would block the rename.  Cancel detection is handled in the
        // WM_SETTEXT block above once g_renameOpDispatched is set.

        // WM_ACTIVATE (WA_ACTIVE) — Notepad++ has processed the activation.
        // FILE_ATTRIBUTE_READONLY was already cleared in WH_CALLWNDPROC (before
        // WM_ACTIVATE ran).  Now call "Clear Read-Only Flag" to clear buffer._isReadOnly
        // which the file monitor set during our inactive period.
        // Do NOT restore FILE_ATTRIBUTE_READONLY here — it stays clear while Notepad++
        // is focused (managed by WA_INACTIVE only).
        if (g_addReadOnly
            && p->hwnd    == g_nppData._nppHandle
            && p->message == WM_ACTIVATE
            && LOWORD(p->wParam) != WA_INACTIVE
            && g_preRenameLockedPaths.empty())
        {
            std::wstring activePath = getCurrentFilePath();
            if (!activePath.empty()) clearSciReadOnly(activePath);

            // Clear buffer._isReadOnly for the newly active buffer.
            UINT clearCmd = findClearReadOnlyCmdId();
            if (clearCmd && !activePath.empty()
                && g_pendingRestorePaths.count(activePath))
            {
                LRESULT cr = ::SendMessage(g_nppData._nppHandle,
                                           WM_COMMAND, MAKEWPARAM(clearCmd, 0), 0);
                log(L"WH_CALLWNDPROCRET WA_ACTIVE: ClearReadOnly cmd=%u result=%d for '%s'",
                    clearCmd, (int)cr, activePath.c_str());
            }
            else
            {
                log(L"WH_CALLWNDPROCRET WA_ACTIVE: clearSciReadOnly done (no ClearReadOnly needed or cmd not found)");
            }
        }
    }
    return ::CallNextHookEx(g_hTitleHook, nCode, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 4 – Plugin lifecycle (called from DllMain and exported functions)
// ═══════════════════════════════════════════════════════════════════════════

// ── pluginInit ───────────────────────────────────────────────────────────────
//
// Called from DllMain(DLL_PROCESS_ATTACH).
// Perform only lightweight, DLL-safe initialisation here.
// ─────────────────────────────────────────────────────────────────────────────
void pluginInit(HANDLE /*hModule*/)
{
    // Nothing to initialise at this stage.
}

// ── pluginCleanUp ────────────────────────────────────────────────────────────
//
// Called from DllMain(DLL_PROCESS_DETACH).
// Safety net: ensure all Win32 file handles are closed.
// ─────────────────────────────────────────────────────────────────────────────
void pluginCleanUp()
{
    ::RemoveWindowSubclass(g_nppData._scintillaMainHandle,   sciSubclassProc, 0);
    ::RemoveWindowSubclass(g_nppData._scintillaSecondHandle, sciSubclassProc, 1);
    if (g_hGetMsgHook) { ::UnhookWindowsHookEx(g_hGetMsgHook); g_hGetMsgHook = nullptr; }
    if (g_hTitleHook)  { ::UnhookWindowsHookEx(g_hTitleHook);  g_hTitleHook  = nullptr; }
    if (g_hCmdHook)    { ::UnhookWindowsHookEx(g_hCmdHook);    g_hCmdHook    = nullptr; }
    releaseAllLocks();
}

// ── commandMenuInit ──────────────────────────────────────────────────────────
//
// Populates the funcItem[] array with menu-item definitions.
// Called once from the exported setInfo().
// ─────────────────────────────────────────────────────────────────────────────
void commandMenuInit()
{
    ::lstrcpy(funcItem[0]._itemName, _T("Enable File Locking"));
    funcItem[0]._pFunc      = toggleLocking;
    funcItem[0]._init2Check = g_lockingEnabled;
    funcItem[0]._pShKey     = nullptr;

    ::lstrcpy(funcItem[1]._itemName, _T(""));
    funcItem[1]._pFunc      = nullptr;
    funcItem[1]._init2Check = false;
    funcItem[1]._pShKey     = nullptr;

    ::lstrcpy(funcItem[2]._itemName, _T("Lock Current File"));
    funcItem[2]._pFunc      = lockCurrentFile;
    funcItem[2]._init2Check = false;
    funcItem[2]._pShKey     = nullptr;

    ::lstrcpy(funcItem[3]._itemName, _T("Unlock Current File"));
    funcItem[3]._pFunc      = unlockCurrentFile;
    funcItem[3]._init2Check = false;
    funcItem[3]._pShKey     = nullptr;

    ::lstrcpy(funcItem[4]._itemName, _T(""));
    funcItem[4]._pFunc      = nullptr;
    funcItem[4]._init2Check = false;
    funcItem[4]._pShKey     = nullptr;

    ::lstrcpy(funcItem[5]._itemName, _T("Show Status"));
    funcItem[5]._pFunc      = showLockStatus;
    funcItem[5]._init2Check = false;
    funcItem[5]._pShKey     = nullptr;

    ::lstrcpy(funcItem[6]._itemName, _T(""));
    funcItem[6]._pFunc      = nullptr;
    funcItem[6]._init2Check = false;
    funcItem[6]._pShKey     = nullptr;

    ::lstrcpy(funcItem[7]._itemName, _T("Add Read-only"));
    funcItem[7]._pFunc      = toggleAddReadOnly;
    funcItem[7]._init2Check = g_addReadOnly;
    funcItem[7]._pShKey     = nullptr;

    ::lstrcpy(funcItem[8]._itemName, _T(""));
    funcItem[8]._pFunc      = nullptr;
    funcItem[8]._init2Check = false;
    funcItem[8]._pShKey     = nullptr;

    ::lstrcpy(funcItem[9]._itemName, _T("Enable Logging"));
    funcItem[9]._pFunc      = toggleLogging;
    funcItem[9]._init2Check = g_loggingEnabled;
    funcItem[9]._pShKey     = nullptr;

    ::lstrcpy(funcItem[10]._itemName, _T("Show Log"));
    funcItem[10]._pFunc      = showLog;
    funcItem[10]._init2Check = false;
    funcItem[10]._pShKey     = nullptr;

    ::lstrcpy(funcItem[11]._itemName, _T(""));
    funcItem[11]._pFunc      = nullptr;
    funcItem[11]._init2Check = false;
    funcItem[11]._pShKey     = nullptr;

    ::lstrcpy(funcItem[12]._itemName, _T("About"));
    funcItem[12]._pFunc      = showAbout;
    funcItem[12]._init2Check = false;
    funcItem[12]._pShKey     = nullptr;
}

// ── commandMenuCleanUp ───────────────────────────────────────────────────────
//
// Called just before the plugin is unloaded.
// ─────────────────────────────────────────────────────────────────────────────
void commandMenuCleanUp()
{
    // Nothing to free in this plugin.
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 5 – Menu-item callback implementations  (STEP 4)
// ═══════════════════════════════════════════════════════════════════════════

// ── toggleLocking ────────────────────────────────────────────────────────────
//
// Flips g_lockingEnabled and updates the menu check-mark.
//
// Enabling:
//   Enumerates all currently-open files via NPPM_GETOPENFILENAMESPRIMARY/SECOND.
//   If any real files are not yet locked, offers to lock them all at once.
//   Files opened after this point are locked automatically via NPPN_FILEOPENED.
//
// Disabling:
//   Releases every held lock immediately so other processes can access the files.
//
// Check-mark update uses CheckMenuItem() directly — NPPM_SETMENUITEMCHECK (+40)
// caused a crash in this Notepad++ build.
// ─────────────────────────────────────────────────────────────────────────────
void toggleLocking()
{
    g_lockingEnabled = !g_lockingEnabled;

    HMENU hMenu = ::GetMenu(g_nppData._nppHandle);
    if (hMenu != nullptr)
    {
        UINT checkFlag = g_lockingEnabled ? MF_CHECKED : MF_UNCHECKED;
        ::CheckMenuItem(hMenu,
                        static_cast<UINT>(funcItem[0]._cmdID),
                        MF_BYCOMMAND | checkFlag);
    }

    saveSettings();

    if (!g_lockingEnabled)
    {
        releaseAllLocks();
        ::MessageBox(
            g_nppData._nppHandle,
            _T("File locking is now DISABLED.\r\nAll locks have been released."),
            _T("FileLock"),
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }

    // Locking just enabled — enumerate all open files by cycling through every
    // tab in the document tab bar (TCM_SETCURSEL + WM_NOTIFY/TCN_SELCHANGE),
    // reading getCurrentFilePath() after each switch.  This avoids all NPPM_
    // enumeration messages, which crash or return bad data in this build.
    // g_idToPath is merged in as a fallback for any tabs the cycling missed.
    std::vector<std::wstring> allPaths = enumerateOpenFilePaths();

    for (const auto& kv : g_idToPath)
    {
        if (kv.second.empty()) continue;
        bool seen = false;
        for (const auto& p : allPaths) if (p == kv.second) { seen = true; break; }
        if (!seen) allPaths.push_back(kv.second);
    }

    std::vector<std::wstring> toOffer;
    for (const auto& path : allPaths)
        if (!g_lockMap.count(path))
            toOffer.push_back(path);

    if (toOffer.empty())
    {
        ::MessageBox(
            g_nppData._nppHandle,
            _T("File locking is now ENABLED.\r\nFiles you open will be locked exclusively."),
            _T("FileLock"),
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }

    wchar_t prompt[320];
    ::_snwprintf_s(prompt, _countof(prompt), _TRUNCATE,
        L"File locking is now ENABLED.\r\n\r\n"
        L"%d file(s) are already open and not yet locked.\r\n"
        L"Lock them now?",
        static_cast<int>(toOffer.size()));

    int res = ::MessageBoxW(
        g_nppData._nppHandle, prompt, L"FileLock",
        MB_YESNO | MB_ICONQUESTION);

    if (res == IDYES)
        for (const auto& p : toOffer)
            lockPath(p);
}

// ── lockCurrentFile ──────────────────────────────────────────────────────────
//
// Manually acquires an exclusive lock on the currently active tab's file.
// Uses getCurrentFilePath() (title-bar parse) for the file path — this is the
// only reliable path-resolution method in this Notepad++ build.
// ─────────────────────────────────────────────────────────────────────────────
void lockCurrentFile()
{
    std::wstring path = getCurrentFilePath();

    if (path.empty())
    {
        ::MessageBox(g_nppData._nppHandle,
            _T("The active tab has no file on disk.\r\n"
               "(Unsaved or untitled document — save it first.)"),
            _T("FileLock"), MB_OK | MB_ICONWARNING);
        return;
    }

    if (g_lockMap.count(path))
    {
        std::wstring msg = L"This file is already locked by FileLock:\r\n" + path;
        ::MessageBoxW(g_nppData._nppHandle,
            msg.c_str(), L"FileLock", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Always do a fresh check on manual lock so the user sees current state.
    {
        std::vector<std::wstring> owners = getFileOwnerProcessNames(path);
        if (!owners.empty())
        {
            std::wstringstream ss;
            ss << L"Cannot lock — this file is already open in another application:\r\n\r\n"
               << path << L"\r\n\r\n"
                  L"Opened by:";
            for (const auto& n : owners)
                ss << L"\r\n  \x2022 " << n;
            ss << L"\r\n\r\n"
                  L"Close the other application first, then try again.";
            ::MessageBoxW(g_nppData._nppHandle, ss.str().c_str(),
                          L"FileLock \x2013 File In Use", MB_OK | MB_ICONWARNING);
            return;
        }
        // Other app closed — clear the suppression flag so auto-lock resumes.
        g_foreignOpenPaths.erase(path);
    }

    ::SetLastError(0);
    HANDLE h = acquireLock(path);
    DWORD  err = ::GetLastError();

    if (h == INVALID_HANDLE_VALUE)
    {
        wchar_t msg[640];
        ::_snwprintf_s(msg, _countof(msg), _TRUNCATE,
            L"CreateFile() failed — could not acquire the lock.\r\n\r\n"
            L"File      : %s\r\n"
            L"Win32 error: %lu\r\n\r\n"
            L"Possible causes:\r\n"
            L"  • Another process holds a conflicting write lock\r\n"
            L"  • Insufficient permissions (try running Notepad++ as Administrator)\r\n"
            L"  • File is on a read-only volume or network share",
            path.c_str(), err);
        ::MessageBoxW(g_nppData._nppHandle, msg, L"FileLock", MB_OK | MB_ICONERROR);
        return;
    }

    g_lockMap[path] = h;
    if (g_addReadOnly)
        applyReadOnly(path);

    std::wstring msg = L"File locked successfully:\r\n" + path;
    ::MessageBoxW(g_nppData._nppHandle,
        msg.c_str(), L"FileLock", MB_OK | MB_ICONINFORMATION);
}

// ── unlockCurrentFile ────────────────────────────────────────────────────────
//
// Manually releases the exclusive lock held on the currently active tab's file.
// Uses getCurrentFilePath() (title-bar parse) — the only reliable path method.
// ─────────────────────────────────────────────────────────────────────────────
void unlockCurrentFile()
{
    std::wstring path = getCurrentFilePath();

    if (path.empty())
    {
        ::MessageBox(g_nppData._nppHandle,
            _T("The active tab has no file on disk."),
            _T("FileLock"), MB_OK | MB_ICONWARNING);
        return;
    }

    if (!g_lockMap.count(path))
    {
        ::MessageBox(g_nppData._nppHandle,
            _T("This file is not currently locked by FileLock."),
            _T("FileLock"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    unlockPath(path);

    std::wstring msg = L"Lock released for:\r\n" + path;
    ::MessageBoxW(g_nppData._nppHandle,
        msg.c_str(), L"FileLock", MB_OK | MB_ICONINFORMATION);
}

// ── showLockStatus ───────────────────────────────────────────────────────────
//
// Shows a concise summary of the current lock state: which files are locked,
// whether automatic locking is on, and current option settings.
// Diagnostic detail is available via Show Log.
// ─────────────────────────────────────────────────────────────────────────────
void showLockStatus()
{
    std::wstringstream ss;
    ss << L"Automatic locking: " << (g_lockingEnabled ? L"ON" : L"OFF") << L"\r\n";
    ss << L"Add Read-only:     " << (g_addReadOnly    ? L"ON" : L"OFF") << L"\r\n";
    ss << L"Logging:           " << (g_loggingEnabled ? L"ON" : L"OFF") << L"\r\n";

    // Locked files — all files with an active Win32 lock handle
    ss << L"\r\nLocked files (" << g_lockMap.size() << L"):\r\n";
    if (g_lockMap.empty())
        ss << L"  (none)\r\n";
    else
        for (const auto& kv : g_lockMap)
            ss << L"  \x2022 " << kv.first << L"\r\n";

    // Read-only files — files whose FILE_ATTRIBUTE_READONLY was already set
    // before the plugin touched them; we track but do not change their attribute.
    {
        std::vector<std::wstring> roFiles;
        for (const auto& kv : g_readOnlyOriginals)
            if (kv.second & FILE_ATTRIBUTE_READONLY)
                roFiles.push_back(kv.first);
        ss << L"\r\nRead-only files (" << roFiles.size() << L"):\r\n";
        if (roFiles.empty())
            ss << L"  (none)\r\n";
        else
            for (const auto& p : roFiles)
                ss << L"  \x2022 " << p << L"\r\n";
    }

    // Pseudo Read-only files — originally writable files on which this plugin
    // has set FILE_ATTRIBUTE_READONLY; attribute is restored on unlock.
    ss << L"\r\nPseudo Read-only files (" << g_pendingRestorePaths.size() << L"):\r\n";
    if (g_pendingRestorePaths.empty())
        ss << L"  (none)\r\n";
    else
        for (const auto& p : g_pendingRestorePaths)
            ss << L"  \x2022 " << p << L"\r\n";

    if (!g_foreignOpenPaths.empty())
    {
        ss << L"\r\nFiles skipped — open in another process ("
           << g_foreignOpenPaths.size() << L"):\r\n";
        for (const auto& p : g_foreignOpenPaths)
            ss << L"  \x2022 " << p << L"\r\n";
    }

    ::MessageBoxW(g_nppData._nppHandle,
        ss.str().c_str(), L"FileLock \x2013 Status", MB_OK | MB_ICONINFORMATION);
}

// ── Log dialog window procedure ───────────────────────────────────────────────
//
// Backs the scrollable log window shown by showLog().  The window contains a
// full-client multiline read-only EDIT control (with both scrollbars) and a
// centred OK button at the bottom.  The window is resizable so the user can
// expand it if needed.
// ─────────────────────────────────────────────────────────────────────────────
static const int IDC_LOG_EDIT = 101;
static const int IDC_LOG_OK   = 102;
static const int IDC_LOG_COPY = 103;

static const wchar_t* g_logWndText   = nullptr; // set before CreateWindowEx
static bool           g_logWndClosed = false;
static HFONT          g_hLogFont     = nullptr;

static LRESULT CALLBACK logWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static const int BTN_H = 28, BTN_W = 80, MARGIN = 8;

    switch (msg)
    {
    case WM_CREATE:
    {
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        HINSTANCE hi = reinterpret_cast<HINSTANCE>(
            ::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

        HWND hEdit = ::CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            MARGIN, MARGIN,
            rc.right  - 2 * MARGIN,
            rc.bottom - BTN_H - 3 * MARGIN,
            hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LOG_EDIT)), hi, nullptr);

        ::SendMessageW(hEdit, EM_SETLIMITTEXT, 0, 0); // remove 32 KB cap

        g_hLogFont = ::CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (g_hLogFont)
            ::SendMessageW(hEdit, WM_SETFONT,
                           reinterpret_cast<WPARAM>(g_hLogFont), FALSE);

        if (g_logWndText)
            ::SetWindowTextW(hEdit, g_logWndText);

        // Two buttons centred as a group: [Copy Log] [OK]
        static const int BTN_W_COPY = 90, BTN_GAP = 8;
        int groupW = BTN_W_COPY + BTN_GAP + BTN_W;
        int bx = (rc.right - groupW) / 2;
        int by = rc.bottom - BTN_H - MARGIN;

        ::CreateWindowExW(
            0, L"BUTTON", L"Copy Log",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, by, BTN_W_COPY, BTN_H,
            hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LOG_COPY)), hi, nullptr);

        ::CreateWindowExW(
            0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            bx + BTN_W_COPY + BTN_GAP, by, BTN_W, BTN_H,
            hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LOG_OK)), hi, nullptr);

        return 0;
    }

    case WM_SIZE:
    {
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        HWND hEdit = ::GetDlgItem(hwnd, IDC_LOG_EDIT);
        HWND hCopy = ::GetDlgItem(hwnd, IDC_LOG_COPY);
        HWND hOk   = ::GetDlgItem(hwnd, IDC_LOG_OK);
        if (hEdit)
            ::SetWindowPos(hEdit, nullptr,
                MARGIN, MARGIN,
                rc.right  - 2 * MARGIN,
                rc.bottom - BTN_H - 3 * MARGIN,
                SWP_NOZORDER);
        static const int BTN_W_COPY = 90, BTN_GAP = 8;
        int groupW = BTN_W_COPY + BTN_GAP + BTN_W;
        int bx = (rc.right - groupW) / 2;
        int by = rc.bottom - BTN_H - MARGIN;
        if (hCopy)
            ::SetWindowPos(hCopy, nullptr, bx, by, BTN_W_COPY, BTN_H, SWP_NOZORDER);
        if (hOk)
            ::SetWindowPos(hOk, nullptr, bx + BTN_W_COPY + BTN_GAP, by,
                BTN_W, BTN_H, SWP_NOZORDER);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_LOG_OK || LOWORD(wParam) == IDCANCEL)
        {
            ::DestroyWindow(hwnd);
        }
        else if (LOWORD(wParam) == IDC_LOG_COPY)
        {
            HWND hEdit = ::GetDlgItem(hwnd, IDC_LOG_EDIT);
            if (hEdit)
            {
                int len = ::GetWindowTextLengthW(hEdit);
                if (len > 0)
                {
                    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE,
                        static_cast<SIZE_T>(len + 1) * sizeof(wchar_t));
                    if (hMem)
                    {
                        wchar_t* ptr = static_cast<wchar_t*>(::GlobalLock(hMem));
                        if (ptr)
                        {
                            ::GetWindowTextW(hEdit, ptr, len + 1);
                            ::GlobalUnlock(hMem);
                            if (::OpenClipboard(hwnd))
                            {
                                ::EmptyClipboard();
                                ::SetClipboardData(CF_UNICODETEXT, hMem);
                                ::CloseClipboard();
                                // hMem is now owned by the clipboard — do not free
                            }
                            else
                                ::GlobalFree(hMem);
                        }
                        else
                            ::GlobalFree(hMem);
                    }
                }
            }
        }
        return 0;

    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_hLogFont) { ::DeleteObject(g_hLogFont); g_hLogFont = nullptr; }
        g_logWndClosed = true;
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── showLog ──────────────────────────────────────────────────────────────────
//
// Displays the in-memory event log and a snapshot of live diagnostic state
// in a scrollable, resizable window.  The window is sized to 80% of the
// screen working area height so it never goes off-screen.
// Enable logging first via "Enable Logging" in the plugin menu.
// ─────────────────────────────────────────────────────────────────────────────
void showLog()
{
    std::wstringstream ss;

    ss << L"Logging: " << (g_loggingEnabled ? L"ENABLED" : L"DISABLED") << L"\r\n";

    // ── Live Scintilla state ──────────────────────────────────────────────
    ss << L"\r\n─── Live state ───\r\n";
    LRESULT roMain = ::SendMessage(g_nppData._scintillaMainHandle,
                                   SCI_GETREADONLY, 0, 0);
    LRESULT roSec  = ::SendMessage(g_nppData._scintillaSecondHandle,
                                   SCI_GETREADONLY, 0, 0);
    ss << L"SCI_GETREADONLY  main=" << roMain << L"  second=" << roSec
       << L"  (0=editable, 1=read-only)\r\n";

    ss << L"sciSubclassProc: SCI_SETREADONLY calls seen=" << g_subclassCallCount
       << L"  intercepted(1\x2192""0)=" << g_subclassInterceptCount << L"\r\n";

    ss << L"Scintilla handles  main="
       << (g_nppData._scintillaMainHandle ? L"valid" : L"NULL")
       << L"  second="
       << (g_nppData._scintillaSecondHandle ? L"valid" : L"NULL") << L"\r\n";

    // ── Attribute tracking tables ─────────────────────────────────────────
    ss << L"\r\ng_readOnlyOriginals (" << g_readOnlyOriginals.size() << L"):\r\n";
    for (const auto& kv : g_readOnlyOriginals)
    {
        bool origRO = (kv.second & FILE_ATTRIBUTE_READONLY) != 0;
        ss << L"  " << kv.first
           << (origRO ? L"  [originally RO]" : L"  [originally writable — weOwn=true]")
           << L"\r\n";
    }

    ss << L"\r\ng_pendingRestorePaths (" << g_pendingRestorePaths.size() << L"):\r\n";
    for (const auto& p : g_pendingRestorePaths)
        ss << L"  " << p << L"\r\n";

    // ── Scintilla writability test ────────────────────────────────────────
    ::SendMessage(g_nppData._scintillaMainHandle, SCI_SETREADONLY, 0, 0);
    LRESULT roForced = ::SendMessage(g_nppData._scintillaMainHandle,
                                     SCI_GETREADONLY, 0, 0);
    ss << L"\r\nAfter SCI_SETREADONLY 0: SCI_GETREADONLY=" << roForced << L"\r\n";

    LRESULT lenBefore = ::SendMessage(g_nppData._scintillaMainHandle, 2006, 0, 0);
    const char testCh = ' ';
    ::SendMessage(g_nppData._scintillaMainHandle, 2001, 1,
                  reinterpret_cast<LPARAM>(&testCh));
    LRESULT lenAfter = ::SendMessage(g_nppData._scintillaMainHandle, 2006, 0, 0);
    bool immOk = (lenAfter == lenBefore + 1);
    if (immOk)
        ::SendMessage(g_nppData._scintillaMainHandle, 2279, 0, 0);

    ss << L"SCI_ADDTEXT test: " << (immOk ? L"SUCCEEDED" : L"FAILED")
       << L"  (lenBefore=" << lenBefore << L"  lenAfter=" << lenAfter << L")\r\n";
    if (immOk)
        ss << L"  \x2192 Scintilla is fully writable.\r\n";
    else
        ss << L"  \x2192 Scintilla refused insertion — pdoc->readonly may still be set.\r\n";

    // ── Event log (last) ─────────────────────────────────────────────────
    ss << L"\r\n─── Event log (" << g_log.size() << L" entries) ───\r\n";
    if (!g_loggingEnabled)
        ss << L"  Enable logging from the menu to start capturing events.\r\n";
    else if (g_log.empty())
        ss << L"  (no events recorded yet)\r\n";
    else
        for (const auto& line : g_log)
            ss << L"  " << line << L"\r\n";

    // ── Register window class (once per process) ──────────────────────────
    static bool s_classReg = false;
    if (!s_classReg)
    {
        WNDCLASSEXW wc    = {};
        wc.cbSize         = sizeof(wc);
        wc.lpfnWndProc    = logWndProc;
        wc.hInstance      = g_hDllInstance;
        wc.hbrBackground  = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.hCursor        = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName  = L"FileLockLogWnd";
        s_classReg = (::RegisterClassExW(&wc) != 0);
    }

    // ── Size: 80% of working-area height; cap width at 900 px ────────────
    RECT wa = {};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int wkW  = wa.right  - wa.left;
    int wkH  = wa.bottom - wa.top;
    int dlgH = wkH * 80 / 100;
    int dlgW = min(900, wkW * 80 / 100);
    int x    = wa.left + (wkW - dlgW) / 2;
    int y    = wa.top  + (wkH - dlgH) / 2;

    std::wstring content = ss.str();
    g_logWndText   = content.c_str();
    g_logWndClosed = false;

    HWND hDlg = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"FileLockLogWnd",
        L"FileLock \x2013 Log",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, dlgW, dlgH,
        g_nppData._nppHandle, nullptr, g_hDllInstance, nullptr);

    if (!hDlg) return;

    ::ShowWindow(hDlg, SW_SHOW);
    ::UpdateWindow(hDlg);
    ::EnableWindow(g_nppData._nppHandle, FALSE); // block input to Notepad++

    // Run a modal message loop until the log window is closed.
    // IsDialogMessageW handles Tab (edit → button), Enter (OK), Escape (cancel).
    // If WM_QUIT arrives, re-post it so Notepad++'s own loop sees it on exit.
    MSG msg = {};
    while (!g_logWndClosed)
    {
        BOOL bRet = ::GetMessageW(&msg, nullptr, 0, 0);
        if (bRet == 0) { ::PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        if (bRet < 0)  break;
        // Ctrl+A: select all in the EDIT control.
        // Handled here rather than relying on IsDialogMessageW, which does not
        // forward Ctrl+A to multiline EDIT controls on all Windows versions.
        if (msg.message == WM_KEYDOWN && msg.wParam == 'A'
            && (::GetKeyState(VK_CONTROL) & 0x8000))
        {
            HWND hEdit = ::GetDlgItem(hDlg, IDC_LOG_EDIT);
            if (hEdit) ::SendMessageW(hEdit, EM_SETSEL, 0, -1);
            continue;
        }
        if (!::IsDialogMessageW(hDlg, &msg))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
    }

    ::EnableWindow(g_nppData._nppHandle, TRUE);
    ::SetForegroundWindow(g_nppData._nppHandle);
    g_logWndText = nullptr;
}

// ── toggleAddReadOnly ────────────────────────────────────────────────────────
//
// Flips g_addReadOnly and updates the menu check-mark.
//
// Enabling:
//   Calls applyReadOnly() for every file currently in g_lockMap so all locked
//   files immediately get FILE_ATTRIBUTE_READONLY.  Notepad++ is kept editable
//   via the SCI_SETREADONLY intercept in titleBarHookProc.
//
// Disabling:
//   Calls restoreReadOnly() for every path in g_readOnlyOriginals, restoring
//   each file to the attribute state it had before the plugin touched it.
// ─────────────────────────────────────────────────────────────────────────────
void toggleAddReadOnly()
{
    g_addReadOnly = !g_addReadOnly;

    HMENU hMenu = ::GetMenu(g_nppData._nppHandle);
    if (hMenu != nullptr)
    {
        UINT checkFlag = g_addReadOnly ? MF_CHECKED : MF_UNCHECKED;
        ::CheckMenuItem(hMenu,
                        static_cast<UINT>(funcItem[7]._cmdID),
                        MF_BYCOMMAND | checkFlag);
    }

    saveSettings();

    if (g_addReadOnly)
    {
        for (const auto& kv : g_lockMap)
            applyReadOnly(kv.first);

        ::MessageBox(
            g_nppData._nppHandle,
            _T("\"Add Read-only\" is now ENABLED.\r\n"
               "FILE_ATTRIBUTE_READONLY is set on locked files while\r\n"
               "Notepad++ is not the active window.\r\n\r\n"
               "Files remain fully editable within Notepad++."),
            _T("FileLock"),
            MB_OK | MB_ICONINFORMATION
        );
    }
    else
    {
        // Iterate over a copy because restoreReadOnly() modifies g_readOnlyOriginals.
        std::vector<std::wstring> paths;
        for (const auto& kv : g_readOnlyOriginals)
            paths.push_back(kv.first);
        for (const auto& p : paths)
            restoreReadOnly(p);

        ::MessageBox(
            g_nppData._nppHandle,
            _T("\"Add Read-only\" is now DISABLED.\r\n"
               "File attributes have been restored."),
            _T("FileLock"),
            MB_OK | MB_ICONINFORMATION
        );
    }
}

// ── toggleLogging ────────────────────────────────────────────────────────────
//
// Flips g_loggingEnabled.  When turned on, clears the old log and resets the
// timestamp base so the new session starts fresh.  The state is written to the
// registry so it survives Notepad++ restarts.
// ─────────────────────────────────────────────────────────────────────────────
void toggleLogging()
{
    g_loggingEnabled = !g_loggingEnabled;

    if (g_loggingEnabled)
    {
        g_log.clear();
        g_logBase = 0;
    }

    HMENU hMenu = ::GetMenu(g_nppData._nppHandle);
    if (hMenu != nullptr)
    {
        UINT checkFlag = g_loggingEnabled ? MF_CHECKED : MF_UNCHECKED;
        ::CheckMenuItem(hMenu,
                        static_cast<UINT>(funcItem[9]._cmdID),
                        MF_BYCOMMAND | checkFlag);
    }

    saveLoggingRegistry();
}

// ── getPluginVersion ─────────────────────────────────────────────────────────
//
// Reads the plugin's FILEVERSION from the DLL's own VERSIONINFO resource and
// returns it as a "MAJOR.MINOR.PATCH" wide string.
// The version is defined once in resource.h (VERSION_MAJOR / MINOR / PATCH /
// BUILD) and embedded into the DLL by FileLockPlugin.rc at build time.
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring getPluginVersion()
{
    wchar_t dllPath[MAX_PATH] = {};
    ::GetModuleFileNameW(g_hDllInstance, dllPath, MAX_PATH);

    DWORD dummy = 0;
    DWORD infoSize = ::GetFileVersionInfoSizeW(dllPath, &dummy);
    if (!infoSize)
        return L"unknown";

    std::vector<BYTE> buf(infoSize);
    if (!::GetFileVersionInfoW(dllPath, 0, infoSize, buf.data()))
        return L"unknown";

    VS_FIXEDFILEINFO* pfi = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\",
                          reinterpret_cast<LPVOID*>(&pfi), &len) || !pfi)
        return L"unknown";

    wchar_t ver[32] = {};
    ::_snwprintf_s(ver, _countof(ver), _TRUNCATE,
                   L"%u.%u.%u",
                   HIWORD(pfi->dwFileVersionMS),
                   LOWORD(pfi->dwFileVersionMS),
                   HIWORD(pfi->dwFileVersionLS));
    return ver;
}

// ── aboutDlgProc ─────────────────────────────────────────────────────────────
//
// Dialog procedure for IDD_ABOUT.
//
// WM_INITDIALOG: reads the DLL version and populates IDC_VERSION.
// WM_NOTIFY / NM_CLICK or NM_RETURN: a SysLink was clicked; opens the URL
//   from NMLINK.item.szUrl in the user's default browser via ShellExecuteW.
// ─────────────────────────────────────────────────────────────────────────────
static INT_PTR CALLBACK aboutDlgProc(HWND hDlg, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        std::wstring ver = getPluginVersion();
        ::SetDlgItemTextW(hDlg, IDC_VERSION, ver.c_str());
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            ::EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    case WM_NOTIFY:
    {
        const NMHDR* pnm = reinterpret_cast<const NMHDR*>(lParam);
        if (pnm->code == NM_CLICK || pnm->code == NM_RETURN)
        {
            const NMLINK* pnml = reinterpret_cast<const NMLINK*>(lParam);
            ::ShellExecuteW(nullptr, L"open",
                            pnml->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ── showAbout ─────────────────────────────────────────────────────────────────
//
// Opens the About dialog (IDD_ABOUT from FileLockPlugin.rc).
// Shows plugin version (read from DLL VERSIONINFO at runtime), developer,
// and three clickable SysLink controls for website, GitHub, and licence.
// ─────────────────────────────────────────────────────────────────────────────
void showAbout()
{
    ::DialogBoxParamW(g_hDllInstance,
                      MAKEINTRESOURCEW(IDD_ABOUT),
                      g_nppData._nppHandle,
                      aboutDlgProc,
                      0);
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 6 – Mandatory exported Notepad++ plugin interface
// ═══════════════════════════════════════════════════════════════════════════

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData notepadPlusData)
{
    g_nppData = notepadPlusData;
    crashRecoveryRestore(); // clear FILE_ATTRIBUTE_READONLY on files from a crashed session
    loadSettings();         // must precede commandMenuInit() so _init2Check reflects saved state
    commandMenuInit();

    // Install both message hooks here rather than in NPPN_READY (which never
    // fires in this build).
    //   g_hGetMsgHook – WH_GETMESSAGE: fires when a posted message (e.g. a
    //                   menu command) is retrieved from the queue, before
    //                   DispatchMessage.  Releases the lock for IDM_FILE_RENAME.
    //   g_hTitleHook  – WH_CALLWNDPROCRET: fires after sent messages are
    //                   processed.  Handles WM_SETTEXT (re-lock new path) and
    //                   WM_ACTIVATE (cancel detection).
    if (!g_hTitleHook)
    {
        DWORD tid = ::GetWindowThreadProcessId(g_nppData._nppHandle, nullptr);
        g_hGetMsgHook = ::SetWindowsHookEx(WH_GETMESSAGE,
                                            getMsgHookProc, nullptr, tid);
        g_hTitleHook  = ::SetWindowsHookEx(WH_CALLWNDPROCRET,
                                            titleBarHookProc, nullptr, tid);
        g_hCmdHook    = ::SetWindowsHookEx(WH_CALLWNDPROC,
                                            cmdHookProc, nullptr, tid);
        // Subclass both Scintilla views to intercept SCI_SETREADONLY 1 before
        // Scintilla processes it.  Unlike WH_CALLWNDPROC (which cannot reliably
        // modify the parameters), the subclass proc calls DefSubclassProc with
        // our own wParam, so the change is guaranteed.
        ::SetWindowSubclass(g_nppData._scintillaMainHandle,
                            sciSubclassProc, 0, 0);
        ::SetWindowSubclass(g_nppData._scintillaSecondHandle,
                            sciSubclassProc, 1, 0);
    }
}

extern "C" __declspec(dllexport) const TCHAR* getName()
{
    return PLUGIN_NAME;
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nbF)
{
    *nbF = nbFunc;
    return funcItem;
}

// ── beNotified ───────────────────────────────────────────────────────────────
//
// Called by Notepad++ for every editor and application event.
//
//   NPPN_FILEOPENED
//     No-op.  This notification fires before the title bar is updated, so
//     getCurrentFilePath() returns the wrong path here.  All tracking and
//     auto-locking are deferred to NPPN_BUFFERACTIVATED.
//
//   NPPN_BUFFERACTIVATED
//     Fires after every buffer switch (new file open or tab change), once the
//     title bar reflects the newly-active file.  If this buffer ID is not yet
//     in g_idToPath it is a newly-opened file: record its path and auto-lock
//     if locking is enabled.  If it is already known, this is a tab switch and
//     no action is needed (unless the path changed, which indicates a Save-As
//     that occurred while the buffer was not visible).
//
//   NPPN_FILESAVED
//     A file was saved (including Save-As).  On Save-As the path changes;
//     detect this by comparing the path in g_idToPath against the current
//     title-bar path.  Re-acquire the lock on the new path if needed.
//
//   NPPN_FILEBEFORERENAME
//     Notepad++ is about to rename the active file.  Release our lock so that
//     our handle (which omits FILE_SHARE_DELETE) does not block MoveFileExW.
//     The old path is saved in g_renamePendingLocks so it can be restored if
//     the rename is cancelled.
//
//   NPPN_FILERENAMED
//     Rename succeeded.  The title bar has already been updated.  Re-acquire
//     the lock on the new path and update g_idToPath.  (The titleBarHookProc
//     fires on the same WM_SETTEXT and may lock first; lockPath() is idempotent.)
//
//   NPPN_FILERENAMECANCEL
//     Rename was cancelled.  Restore the lock on the original path.
//
//   NPPN_FILEBEFORECLOSE
//     A tab is about to close.  Look up its path in g_idToPath and release
//     the lock.  Falls back to getCurrentFilePath() for files not in g_idToPath
//     (e.g., session-restored files opened before the plugin loaded).
//
//   NPPN_SHUTDOWN
//     Release all locks before DLL unload.
// ─────────────────────────────────────────────────────────────────────────────
extern "C" __declspec(dllexport) void beNotified(SCNotification* notifyCode)
{
    uptr_t bufferId = static_cast<uptr_t>(notifyCode->nmhdr.idFrom);

    switch (notifyCode->nmhdr.code)
    {
        case NPPN_READY:
            // NPPN_READY does not fire in this Notepad++ build.
            // All hooks are installed in setInfo() instead.
            break;

        case NPPN_FILEOPENED:
            // NPPN_FILEOPENED fires before the title bar reflects the new file,
            // so getCurrentFilePath() returns the wrong path here.
            // All tracking and auto-locking are done in NPPN_BUFFERACTIVATED,
            // which fires after the tab switch and title bar update are complete.
            break;

        case NPPN_BUFFERACTIVATED:
        {
            // Fires after every buffer switch — tab change OR new file open.
            // getCurrentFilePath() reliably returns the now-active file's path.
            std::wstring path = getCurrentFilePath();
            log(L"NPPN_BUFFERACTIVATED: bufferId=%llu  path='%s'",
                (unsigned long long)bufferId, path.c_str());
            if (path.empty()) break;

            auto it = g_idToPath.find(bufferId);
            if (it == g_idToPath.end())
            {
                // First activation of this buffer — file was just opened.
                g_idToPath[bufferId] = path;
                if (g_lockingEnabled)
                    lockPath(path);
            }
            else if (it->second != path)
            {
                // Path changed (Save-As while buffer was inactive).
                // Update tracking and refresh the lock.
                if (g_lockingEnabled)
                {
                    unlockPath(it->second);
                    lockPath(path);
                }
                it->second = path;
            }
            // else: known buffer re-activated (tab switch) — no action needed.

            // If Add Read-only is on and this file is managed by us, clear both
            // Scintilla's pdoc->readonly AND Notepad++'s buffer._isReadOnly.
            // This fixes the non-active-buffer problem: a buffer that was set
            // buffer._isReadOnly=true while not active (by the file monitor during
            // our inactive period) gets correctly cleared when the user switches to it.
            if (g_addReadOnly && !path.empty() && g_pendingRestorePaths.count(path))
            {
                LRESULT roBefore = ::SendMessage(g_nppData._scintillaMainHandle,
                                                 SCI_GETREADONLY, 0, 0);
                ::SendMessage(g_nppData._scintillaMainHandle,   SCI_SETREADONLY, 0, 0);
                ::SendMessage(g_nppData._scintillaSecondHandle, SCI_SETREADONLY, 0, 0);
                LRESULT roAfter = ::SendMessage(g_nppData._scintillaMainHandle,
                                                SCI_GETREADONLY, 0, 0);
                log(L"BUFFERACTIVATED: RO_before=%d  RO_after=%d", (int)roBefore, (int)roAfter);

                // Clear buffer._isReadOnly so the tab icon is not grey and saves work.
                UINT clearCmd = findClearReadOnlyCmdId();
                if (clearCmd)
                {
                    LRESULT cr = ::SendMessage(g_nppData._nppHandle,
                                               WM_COMMAND, MAKEWPARAM(clearCmd, 0), 0);
                    log(L"BUFFERACTIVATED: ClearReadOnly cmd=%u result=%d", clearCmd, (int)cr);
                }
            }
            break;
        }

        case NPPN_FILEBEFORESAVE:
        {
            // Fires just before Notepad++ writes the file to disk.
            // If this appears in the log after IDM_FILE_SAVE, the save is proceeding.
            // If this is absent, buffer._isReadOnly=true still blocked the write.
            std::wstring path = getCurrentFilePath();
            log(L"NPPN_FILEBEFORESAVE: bufferId=%llu  path='%s'",
                (unsigned long long)bufferId,
                path.empty() ? L"(empty)" : path.c_str());
            break;
        }

        case NPPN_FILESAVED:
        {
            // Handles Save-As path changes and first-save of an unsaved tab.
            // NPPN_BUFFERACTIVATED also catches path changes, but NPPN_FILESAVED
            // covers the case where the file is saved without switching away first.
            std::wstring newPath = getCurrentFilePath();
            if (newPath.empty()) break;

            auto it = g_idToPath.find(bufferId);
            std::wstring oldPath = (it != g_idToPath.end()) ? it->second : L"";
            g_idToPath[bufferId] = newPath;

            if (g_lockingEnabled)
            {
                if (oldPath.empty())
                    lockPath(newPath);           // first save of an unsaved tab
                else if (oldPath != newPath)
                {
                    unlockPath(oldPath);         // Save-As: release stale handle
                    lockPath(newPath);           // acquire on new path
                }
                // Normal save: path unchanged, existing handle remains valid.
            }

            // Do NOT restore FILE_ATTRIBUTE_READONLY here.  Notepad++ runs a
            // post-save check after NPPN_FILESAVED returns: if it finds
            // FILE_ATTRIBUTE_READONLY set it reports "Save failed" even though
            // the write succeeded.  The existing WM_SETTEXT handler restores
            // the attribute naturally when Notepad++ updates the title bar
            // (removes the "*") after the post-save checks complete.
            for (auto sc = g_saveClearedPaths.begin();
                 sc != g_saveClearedPaths.end(); ++sc)
                if (*sc == newPath) { g_saveClearedPaths.erase(sc); break; }
            log(L"NPPN_FILESAVED: save complete for '%s' — RO restore deferred to WM_SETTEXT",
                newPath.c_str());
            break;
        }

        case NPPN_FILEBEFORERENAME:
        {
            // Release our lock so MoveFileExW can proceed.
            // (Our handle lacks FILE_SHARE_DELETE; Notepad++'s own handle grants it,
            // so the rename succeeds once ours is closed.)
            std::wstring path;
            auto idIt = g_idToPath.find(bufferId);
            path = (idIt != g_idToPath.end()) ? idIt->second : getCurrentFilePath();

            if (!path.empty() && g_lockMap.count(path))
            {
                g_renamePendingLocks[bufferId] = path;
                unlockPath(path);
            }
            break;
        }

        case NPPN_FILERENAMED:
        {
            // Rename succeeded.  Title bar already shows the new path.
            std::wstring newPath = getCurrentFilePath();
            if (!newPath.empty())
                g_idToPath[bufferId] = newPath;

            auto it = g_renamePendingLocks.find(bufferId);
            if (it != g_renamePendingLocks.end())
            {
                g_renamePendingLocks.erase(it);
                if (!newPath.empty())
                    lockPath(newPath); // idempotent if titleBarHookProc already locked
            }
            break;
        }

        case NPPN_FILERENAMECANCEL:
        {
            // Rename was cancelled — restore the lock on the original path.
            auto it = g_renamePendingLocks.find(bufferId);
            if (it != g_renamePendingLocks.end())
            {
                lockPath(it->second);
                g_renamePendingLocks.erase(it);
            }
            break;
        }

        case NPPN_FILEBEFORECLOSE:
        {
            auto it = g_idToPath.find(bufferId);
            log(L"NPPN_FILEBEFORECLOSE: bufferId=%llu  known=%s",
                (unsigned long long)bufferId,
                (it != g_idToPath.end()) ? it->second.c_str() : L"(not in g_idToPath)");
            if (it != g_idToPath.end())
            {
                unlockPath(it->second);
                g_idToPath.erase(it);
            }
            // Do NOT fall back to getCurrentFilePath() for unknown buffers.
            // An unknown bufferId means the close happened before we tracked it
            // (e.g., a temporary buffer during session restore).  The deferred
            // WM_FL_CHECK_CLOSE will clean up any orphaned locks instead.
            break;
        }

        case NPPN_READONLYCHANGED:
        {
            // For NPPN_READONLYCHANGED, the Notepad++ API puts the real buffer ID
            // in nmhdr.hwndFrom and the DOCSTATUS_* flags in nmhdr.idFrom.
            // The global 'bufferId' (from idFrom) is DOCSTATUS_READONLY=1, not a
            // buffer ID — use hwndFrom instead.
            uptr_t roBufferId = reinterpret_cast<uptr_t>(notifyCode->nmhdr.hwndFrom);
            UINT   docStatus  = static_cast<UINT>(notifyCode->nmhdr.idFrom);
            log(L"NPPN_READONLYCHANGED fired: realBufferId=%llu  docStatus=%u",
                static_cast<unsigned long long>(roBufferId), docStatus);

            // During our controlled toggle (save handler), capture the result and
            // do nothing else.  The save handler checks g_lastToggleDocStatus to
            // detect a wrong-direction toggle and correct it before the save runs.
            if (g_toggleInProgress)
            {
                g_lastToggleDocStatus = docStatus;
                log(L"NPPN_READONLYCHANGED: captured toggle result docStatus=%u", docStatus);
                break;
            }

            // Only act when the buffer just became read-only (DOCSTATUS_READONLY=1).
            // When it became writable we don't need to do anything.
            if (!(docStatus & DOCSTATUS_READONLY)) break;

            if (g_addReadOnly)
            {
                // Resolve path via g_idToPath rather than getCurrentFilePath(),
                // because 50-100ms may have passed since the attribute was set.
                std::wstring path;
                auto it = g_idToPath.find(roBufferId);
                if (it != g_idToPath.end())
                    path = it->second;
                else
                    path = getCurrentFilePath();

                if (!path.empty() && g_pendingRestorePaths.count(path))
                {
                    // Do NOT call NPPM_SETBUFFERREADONLY here directly.
                    // This notification fires from inside Notepad++'s
                    // buf->setReadOnly(true) call.  A re-entrant
                    // NPPM_SETBUFFERREADONLY call gets overridden when the
                    // outer buf->setReadOnly(true) resumes after our handler
                    // returns.  Post to the normal message loop instead.
                    log(L"NPPN_READONLYCHANGED: posting WM_FL_CLEAR_RO for bufferId=%llu",
                        static_cast<unsigned long long>(roBufferId));
                    ::PostThreadMessageW(::GetCurrentThreadId(),
                                         WM_FL_CLEAR_RO,
                                         static_cast<WPARAM>(roBufferId), 0);
                }
            }
            break;
        }

        case NPPN_SHUTDOWN:
            ::RemoveWindowSubclass(g_nppData._scintillaMainHandle,   sciSubclassProc, 0);
            ::RemoveWindowSubclass(g_nppData._scintillaSecondHandle, sciSubclassProc, 1);
            if (g_hGetMsgHook) { ::UnhookWindowsHookEx(g_hGetMsgHook); g_hGetMsgHook = nullptr; }
            if (g_hTitleHook)  { ::UnhookWindowsHookEx(g_hTitleHook);  g_hTitleHook  = nullptr; }
            if (g_hCmdHook)    { ::UnhookWindowsHookEx(g_hCmdHook);    g_hCmdHook    = nullptr; }
            commandMenuCleanUp();
            releaseAllLocks();  // restores r/o attributes and clears g_readOnlyOriginals
            g_idToPath.clear();
            g_renamePendingLocks.clear();
            g_preRenameLockedPaths.clear();
            g_renameOpDispatched = false;
            g_preDeleteLockedPaths.clear();
            g_deleteOpDispatched = false;
            g_saveClearedPaths.clear();
            g_foreignOpenPaths.clear();
            g_pendingInitialLock = false;
            break;

        default:
            break;
    }
}

// ── messageProc ──────────────────────────────────────────────────────────────
extern "C" __declspec(dllexport) LRESULT messageProc(UINT /*msg*/,
                                                      WPARAM /*wParam*/,
                                                      LPARAM /*lParam*/)
{
    return FALSE;
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 7 – DLL entry point
// ═══════════════════════════════════════════════════════════════════════════

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID /*lpReserved*/)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            g_hDllInstance = static_cast<HINSTANCE>(hModule);
            pluginInit(hModule);
            break;

        case DLL_PROCESS_DETACH:
            pluginCleanUp();
            break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
    }

    return TRUE;
}
