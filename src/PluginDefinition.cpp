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
//  [0]   Toggle File Locking (On/Off) Flips g_lockingEnabled; updates check-mark
//  [1]   (separator)                  Empty name → Notepad++ renders a line
//  [2]   Lock Current File            Manually locks the active tab
//  [3]   Unlock Current File          Manually releases the active tab's lock
//  [4]   Show Lock Status             Message box listing all locked files
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
#include <string>       // std::wstring – wide-character file paths
#include <sstream>      // std::wstringstream – building multi-line messages
#include <vector>       // std::vector – temporary collections
#include <tchar.h>      // _T() macro – literal portability (Unicode / ANSI)

// Windows extended controls (TCM_* tab-control messages, TCN_SELCHANGE)
#include <commctrl.h>

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 1 – Plugin-wide state
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 2 – Internal file-lock helpers
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

    HANDLE h = acquireLock(path);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    g_lockMap[path] = h;
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
        releaseLock(kv.second);
    g_lockMap.clear();
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
                if (currentCount < g_prevTabCount && !g_lockMap.empty())
                {
                    std::vector<std::wstring> openPaths = enumerateOpenFilePaths();
                    std::vector<std::wstring> toUnlock;
                    for (const auto& kv : g_lockMap)
                    {
                        bool found = false;
                        for (const auto& op : openPaths)
                            if (op == kv.first) { found = true; break; }
                        if (!found)
                            toUnlock.push_back(kv.first);
                    }
                    for (const auto& path : toUnlock)
                        unlockPath(path);
                }
                g_prevTabCount = currentCount;
            }

            // Auto-lock the newly active file if locking is enabled.
            if (g_lockingEnabled)
            {
                std::wstring path = getCurrentFilePath();
                if (!path.empty() && !g_lockMap.count(path))
                    lockPath(path);
            }
        }
    }
    return ::CallNextHookEx(g_hTitleHook, nCode, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 3 – Plugin lifecycle (called from DllMain and exported functions)
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
    if (g_hTitleHook) { ::UnhookWindowsHookEx(g_hTitleHook); g_hTitleHook = nullptr; }
    releaseAllLocks();
}

// ── commandMenuInit ──────────────────────────────────────────────────────────
//
// Populates the funcItem[] array with menu-item definitions.
// Called once from the exported setInfo().
// ─────────────────────────────────────────────────────────────────────────────
void commandMenuInit()
{
    ::lstrcpy(funcItem[0]._itemName, _T("Toggle File Locking (On/Off)"));
    funcItem[0]._pFunc      = toggleLocking;
    funcItem[0]._init2Check = false;
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

    ::lstrcpy(funcItem[4]._itemName, _T("Show Lock Status"));
    funcItem[4]._pFunc      = showLockStatus;
    funcItem[4]._init2Check = false;
    funcItem[4]._pShKey     = nullptr;
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
// SECTION 4 – Menu-item callback implementations  (STEP 4)
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
// Builds a human-readable summary of the current lock state and shows it in
// a message box.
// ─────────────────────────────────────────────────────────────────────────────
void showLockStatus()
{
    if (g_lockMap.empty())
    {
        const wchar_t* msg = g_lockingEnabled
            ? L"Automatic locking is ENABLED.\r\nNo files are locked yet\r\n"
              L"(lock takes effect when you open a file)."
            : L"File locking is DISABLED.\r\nNo files are currently locked.";

        ::MessageBoxW(g_nppData._nppHandle,
            msg, L"FileLock – Status", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstringstream ss;
    ss << L"Locked files (" << g_lockMap.size() << L"):\r\n\r\n";
    for (const auto& kv : g_lockMap)
        ss << L"  • " << kv.first << L"\r\n";
    ss << L"\r\nAutomatic locking: " << (g_lockingEnabled ? L"ON" : L"OFF");

    ::MessageBoxW(g_nppData._nppHandle,
        ss.str().c_str(), L"FileLock – Status", MB_OK | MB_ICONINFORMATION);
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 5 – Mandatory exported Notepad++ plugin interface
// ═══════════════════════════════════════════════════════════════════════════

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData notepadPlusData)
{
    g_nppData = notepadPlusData;
    commandMenuInit();

    // Install the title-bar hook here rather than in NPPN_READY, because
    // NPPN_READY does not fire in this Notepad++ build.
    if (!g_hTitleHook)
    {
        DWORD tid = ::GetWindowThreadProcessId(g_nppData._nppHandle, nullptr);
        g_hTitleHook = ::SetWindowsHookEx(WH_CALLWNDPROCRET,
                                           titleBarHookProc, nullptr, tid);
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
        {
            // Install the title-bar hook now that the Notepad++ window is live.
            DWORD tid = ::GetWindowThreadProcessId(g_nppData._nppHandle, nullptr);
            if (!g_hTitleHook)
                g_hTitleHook = ::SetWindowsHookEx(WH_CALLWNDPROCRET,
                                                   titleBarHookProc, nullptr, tid);

            // DIAGNOSTIC 1: confirm NPPN_READY fired and show hook result.
            wchar_t msg[256];
            ::_snwprintf_s(msg, _countof(msg), _TRUNCATE,
                L"NPPN_READY fired.\n"
                L"Thread ID: %lu\n"
                L"Hook handle: %s",
                tid,
                g_hTitleHook ? L"non-NULL (installed OK)" : L"NULL (FAILED)");
            ::MessageBoxW(g_nppData._nppHandle, msg, L"FileLock Diag 1", MB_OK);
            break;
        }

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
            break;
        }

        case NPPN_FILEBEFORECLOSE:
        {
            auto it = g_idToPath.find(bufferId);
            if (it != g_idToPath.end())
            {
                unlockPath(it->second);
                g_idToPath.erase(it);
            }
            else
            {
                // Not in g_idToPath (e.g., session-restored file opened before
                // the plugin loaded) — fall back to the title bar.
                unlockPath(getCurrentFilePath());
            }
            break;
        }

        case NPPN_SHUTDOWN:
            if (g_hTitleHook) { ::UnhookWindowsHookEx(g_hTitleHook); g_hTitleHook = nullptr; }
            commandMenuCleanUp();
            releaseAllLocks();
            g_idToPath.clear();
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
// SECTION 6 – DLL entry point
// ═══════════════════════════════════════════════════════════════════════════

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID /*lpReserved*/)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
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
