// PluginInterface.h
//
// Defines the core types, structures, and exported function signatures that
// every Notepad++ plugin DLL must provide.
//
// This corresponds to the PluginInterface.h used in the official Notepad++
// plugin template (https://github.com/npp-plugins/plugintemplate).
//
// HOW NOTEPAD++ DISCOVERS AND LOADS PLUGINS
// ──────────────────────────────────────────
// 1. Notepad++ scans  %PROGRAMFILES%\Notepad++\plugins\<Name>\<Name>.dll
// 2. It calls GetProcAddress() for each of the six exported functions below.
//    If any are missing the DLL is silently ignored.
// 3. Startup sequence:
//      a. isUnicode()       — must return TRUE or plugin is rejected
//      b. setInfo()         — Npp passes its window handles to the plugin
//      c. getName()         — Npp reads the plugin's display name
//      d. getFuncsArray()   — Npp reads the plugin's menu-item definitions
//      e. beNotified()      — called for every subsequent Npp/Scintilla event
//      f. messageProc()     — called for Windows messages directed at plugin

#ifndef PLUGIN_INTERFACE_H
#define PLUGIN_INTERFACE_H

#include <windows.h>            // Win32 types: HWND, BOOL, TCHAR, etc.
#include "Scintilla.h"          // uptr_t, sptr_t, SCNotification
#include "Notepad_plus_msgs.h"  // NPPM_* messages and NPPN_* notifications

// ── PFUNCPLUGINCMD ────────────────────────────────────────────────────────────
//
// The function-pointer type for every plugin menu-item callback.
// When the user clicks a menu item, Notepad++ calls the associated function
// through this pointer.  The function must take no arguments and return void.
// ─────────────────────────────────────────────────────────────────────────────
typedef void (*PFUNCPLUGINCMD)();

// ── ShortcutKey ──────────────────────────────────────────────────────────────
//
// Optional keyboard shortcut associated with a menu item.
// Pass nullptr for FuncItem._pShKey to register no shortcut.
//
// Example – Ctrl+Alt+L:
//   ShortcutKey sk = { true, true, false, 'L' };
// ─────────────────────────────────────────────────────────────────────────────
struct ShortcutKey {
    bool  _isCtrl;      // true = Ctrl modifier required
    bool  _isAlt;       // true = Alt modifier required
    bool  _isShift;     // true = Shift modifier required
    UCHAR _key;         // Virtual-key code (e.g. 'L', VK_F1, VK_DELETE)
};

// ── FuncItem ─────────────────────────────────────────────────────────────────
//
// Describes one entry in the plugin's sub-menu under the Plugins top-level
// menu.  An array of these is returned from getFuncsArray().
//
// Members:
//   _itemName   Text shown in the menu.  64-character limit including null.
//               An empty string ("") is rendered by Notepad++ as a separator.
//   _pFunc      Callback invoked when the user clicks this item.
//               Must match the PFUNCPLUGINCMD signature (void fn()).
//   _cmdID      Filled in by Notepad++ after getFuncsArray() returns.
//               Do NOT set this yourself; read it back to use NPPM_SETMENUITEMCHECK.
//   _init2Check Whether this item starts with a visible check-mark.
//               Typically false; toggleable items are checked via NPPM_SETMENUITEMCHECK.
//   _pShKey     Pointer to a ShortcutKey, or nullptr for no keyboard shortcut.
// ─────────────────────────────────────────────────────────────────────────────
struct FuncItem {
    TCHAR          _itemName[64];   // Menu label (Unicode wide chars)
    PFUNCPLUGINCMD _pFunc;          // Callback function pointer
    int            _cmdID;          // Assigned by Notepad++ — read-only after registration
    bool           _init2Check;     // Initial check-mark state
    ShortcutKey*   _pShKey;         // Keyboard shortcut, or nullptr
};

// ── NppData ──────────────────────────────────────────────────────────────────
//
// Passed to setInfo() once at plugin load time.  Store this globally so that
// any function in the plugin can call SendMessage(nppData._nppHandle, ...).
//
// Members:
//   _nppHandle              Main Notepad++ application window.
//                           Use for all NPPM_* SendMessage calls.
//   _scintillaMainHandle    Primary (left/top) Scintilla editor pane.
//   _scintillaSecondHandle  Secondary (right/bottom) pane (split-view mode).
//
// To find out which pane is currently active, use NPPM_GETCURRENTSCINTILLA.
// ─────────────────────────────────────────────────────────────────────────────
struct NppData {
    HWND _nppHandle;
    HWND _scintillaMainHandle;
    HWND _scintillaSecondHandle;
};

// ── Exported plugin interface ─────────────────────────────────────────────────
//
// These six functions MUST be exported by name from the plugin DLL.
// Notepad++ locates them via GetProcAddress() using the undecorated names.
// The companion .def file ensures MSVC does not apply C++ name-mangling.
//
//  isUnicode()
//      Return TRUE unconditionally.  A FALSE return causes Notepad++ to
//      reject the plugin as ANSI-only (no longer supported).
//
//  setInfo(NppData)
//      Store the NppData struct in a global variable for later use.
//      Do NOT perform heavy initialisation here; wait for NPPN_READY.
//
//  getName()
//      Return the plugin's display name as a const TCHAR*.  This string
//      appears in the Plugins top-level menu.
//
//  getFuncsArray(int* nbF)
//      Write the number of menu items into *nbF and return a pointer to the
//      FuncItem array.  The array must remain valid for the lifetime of the
//      DLL (typically a static or global array).
//
//  beNotified(SCNotification*)
//      Called for every Notepad++/Scintilla event notification.  Check
//      notifyCode->nmhdr.code against NPPN_* and SCN_* constants.
//      Keep this function fast — it is called very frequently.
//
//  messageProc(UINT, WPARAM, LPARAM)
//      Called when Notepad++ forwards a Windows message to the plugin.
//      Return FALSE to allow Notepad++ to continue its own processing.
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {
    __declspec(dllexport) BOOL         isUnicode();
    __declspec(dllexport) void         setInfo(NppData notepadPlusData);
    __declspec(dllexport) const TCHAR* getName();
    __declspec(dllexport) FuncItem*    getFuncsArray(int* nbF);
    __declspec(dllexport) void         beNotified(SCNotification* notifyCode);
    __declspec(dllexport) LRESULT      messageProc(UINT msg, WPARAM wParam, LPARAM lParam);
}

#endif  // PLUGIN_INTERFACE_H
