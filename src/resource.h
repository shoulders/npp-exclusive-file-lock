// resource.h
// Pre-compile configuration for ExclusiveFileLock Notepad++ Plugin.
//
// ── VERSION ──────────────────────────────────────────────────────────────────
// Edit the four VERSION_* numbers here to update the plugin version everywhere:
//   • FILEVERSION / PRODUCTVERSION in FileLockPlugin.rc  (Windows file
//     Properties > Details tab)
//   • The About dialog  (reads from the DLL at runtime via GetFileVersionInfoW)
//
// VERSION_BUILD is typically 0 for releases and is omitted from the displayed
// version string in the About dialog (shown as MAJOR.MINOR.PATCH only).
#define VERSION_MAJOR   1
#define VERSION_MINOR   0
#define VERSION_PATCH   0
#define VERSION_BUILD   0

// String forms of the four numbers above.
// The resource compiler cannot derive strings from integers at compile time,
// so both numeric and string forms must be kept in sync manually.
#define VERSION_MAJOR_S "1"
#define VERSION_MINOR_S "0"
#define VERSION_PATCH_S "0"
#define VERSION_BUILD_S "0"
#define VERSION_STR     VERSION_MAJOR_S "." VERSION_MINOR_S "." VERSION_PATCH_S "." VERSION_BUILD_S

// ── ABOUT DIALOG RESOURCE IDs ─────────────────────────────────────────────────
#define IDD_ABOUT       101     // dialog resource identifier
#define IDC_VERSION     1001    // version label (set dynamically at WM_INITDIALOG)
#define IDC_WEBSITE     1002    // SysLink — website URL
#define IDC_GITHUB      1003    // SysLink — GitHub repository URL
#define IDC_LICENSE     1004    // SysLink — licence URL

// Standard placeholder ID for static-text controls that are never looked up
// by ID.  Defined here so the .rc file does not need to include <winuser.h>.
#ifndef IDC_STATIC
#define IDC_STATIC      (-1)
#endif

// ── Win32 constants used in the .rc dialog template ──────────────────────────
// The resource compiler does not automatically inherit the Windows SDK include
// path, so these constants must be defined here rather than via <windows.h>.
// The #ifndef guards ensure they are silently skipped in PluginDefinition.cpp
// where <windows.h> has already defined them.
#ifndef VS_VERSION_INFO
#define VS_VERSION_INFO 1       // resource name for VERSIONINFO blocks (verrsrc.h)
#endif
#ifndef IDOK
#define IDOK            1
#endif
#ifndef IDCANCEL
#define IDCANCEL        2
#endif
#ifndef DS_SETFONT
#define DS_SETFONT      0x0040L
#endif
#ifndef DS_MODALFRAME
#define DS_MODALFRAME   0x0080L
#endif
#ifndef DS_CENTER
#define DS_CENTER       0x0800L
#endif
#ifndef WS_POPUP
#define WS_POPUP        0x80000000L
#endif
#ifndef WS_CAPTION
#define WS_CAPTION      0x00C00000L
#endif
#ifndef WS_SYSMENU
#define WS_SYSMENU      0x00080000L
#endif
