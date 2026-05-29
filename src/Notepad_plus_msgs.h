// Notepad_plus_msgs.h
//
// Message and notification constants for communicating with Notepad++.
// Plugins use these values as the 'msg' parameter in SendMessage() calls
// to Notepad++, and as the 'code' field in SCNotification.nmhdr received
// by beNotified().
//
// This is a plugin-focused subset of the full header.
// The authoritative, up-to-date version is maintained at:
//   https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h
//
// Copyright (C)  2019 Don HO <don.h@free.fr>
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#ifndef NOTEPAD_PLUS_MSGS_H
#define NOTEPAD_PLUS_MSGS_H

#include <windows.h>    // HWND, WPARAM, LPARAM, WM_USER

// ── Base offset ──────────────────────────────────────────────────────────────
//
// NOTEPADPLUS_USER is the base from which all Notepad++ WM_USER messages are
// numbered.  All NPPM_* and NPPN_* values are derived from this offset.
//
// The correct value is WM_USER + 0x0400 (= 0x0800).  An earlier version of
// this header used 0x0F00, which shifted every message number by 0x0B00 above
// what Notepad++ actually sends and receives — causing all SendMessage() calls
// and all beNotified() switch cases to silently miss, which is why NPPM_
// queries returned 0 and NPPN_ notifications were never matched.
// ─────────────────────────────────────────────────────────────────────────────
#define NOTEPADPLUS_USER (WM_USER + 0x0400)

// ── Plugin message constants (NPPM_*) ────────────────────────────────────────
//
// These are used as the 'msg' parameter in:
//   SendMessage(nppData._nppHandle, NPPM_*, wParam, lParam)
//
// NPPM_GETCURRENTSCINTILLA
//   Tells you which Scintilla view (0=main, 1=secondary) is currently active.
//   wParam = 0
//   lParam = (LPARAM)(int*) pointer to receive the view index
//   Returns: 0
//
// NPPM_SETMENUITEMCHECK
//   Sets or clears the check-mark on a plugin menu item.
//   wParam = (WPARAM)(int) cmdID    (from FuncItem._cmdID after registration)
//   lParam = (LPARAM) TRUE to check, FALSE to uncheck
//   Returns: 0
//   Correct offset is +40.  An earlier version of this stripped header used +6,
//   which is NPPM_SETCURRENTLANGTYPE — so the menu check-mark toggle never worked.
//
// NPPM_GETFULLCURRENTPATH
//   Fills a caller-supplied TCHAR buffer with the absolute file path of the
//   currently active tab.  Does NOT require a buffer ID.
//   wParam = 0  (MUST be 0 — passing MAX_PATH is misinterpreted as a file or
//               view index by some Notepad++ versions and causes a spurious
//               "Cannot open file" error dialog inside Notepad++)
//   lParam = (LPARAM)(TCHAR*) pathBuffer of MAX_PATH characters (caller allocates)
//   Returns: 0 ALWAYS, even on success.  The Notepad++ source writes into
//            lParam via lstrcpy() and then returns 0 unconditionally.
//            Test path[0] != '\0' to detect success, not the return value.
//   Use this for manual lock/unlock commands where the current tab's path is
//   needed and no buffer ID is available (or the buffer ID is a v8+ integer
//   that NPPM_GETFULLPATHFROMBUFFERID cannot resolve).
//
// NPPM_GETCURRENTDIRECTORY
//   Fills a caller-supplied TCHAR buffer with the directory portion only of
//   the currently active tab's file path (no trailing backslash, no filename).
//   wParam = 0
//   lParam = (LPARAM)(TCHAR*) buffer of MAX_PATH characters (caller allocates)
//   Returns: 0 always.  Test buf[0] != '\0' for success.
//   Used together with NPPM_GETCURRENTFILENAME as a fallback when
//   NPPM_GETFULLCURRENTPATH returns an empty buffer.
//
// NPPM_GETCURRENTFILENAME
//   Fills a caller-supplied TCHAR buffer with the filename only (no directory)
//   of the currently active tab.
//   wParam = 0
//   lParam = (LPARAM)(TCHAR*) buffer of MAX_PATH characters (caller allocates)
//   Returns: 0 always.  Test buf[0] != '\0' for success.
//   Combined with NPPM_GETCURRENTDIRECTORY to reconstruct the full path as a
//   fallback when NPPM_GETFULLCURRENTPATH does not work.
//
// NPPM_GETCURRENTBUFFERID
//   Retrieves the buffer ID of whichever tab is currently active.
//   wParam = 0, lParam = 0
//   Returns: uptr_t buffer ID  (0 = no file active)
//   Note: in Notepad++ v8 and later buffer IDs are sequential integers, not
//   heap pointers.  A value such as 16 is therefore normal and valid.
//
// NPPM_GETFULLPATHFROMBUFFERID
//   Fills a caller-supplied TCHAR buffer with the absolute file path for a
//   given buffer ID.
//   wParam = (WPARAM)(uptr_t) bufferID
//   lParam = (LPARAM)(TCHAR*) pathBuffer  (caller allocates MAX_PATH chars)
//   Returns: number of characters written, or -1 if the buffer ID is not found.
//   Use this in beNotified() where the buffer ID comes directly from nmhdr.idFrom
//   (which IS the real internal buffer ID).  Do NOT use the value returned by
//   NPPM_GETCURRENTBUFFERID here — that value is only reliable with
//   NPPM_GETFULLCURRENTPATH; passing it to GETFULLPATHFROMBUFFERID returns -1
//   on some Notepad++ versions due to internal ID format differences.
// ─────────────────────────────────────────────────────────────────────────────
#define NPPM_GETCURRENTSCINTILLA     (NOTEPADPLUS_USER + 4)

// NPPM_GETNBOPENFILES
//   Returns the number of open files in one or both views.
//   wParam: 0 (unused)
//   lParam: 0 = all open files, 1 = primary view only, 2 = secondary view only
#define NPPM_GETNBOPENFILES          (NOTEPADPLUS_USER + 7)


#define NPPM_SETMENUITEMCHECK        (NOTEPADPLUS_USER + 40)
#define NPPM_GETFULLCURRENTPATH      (NOTEPADPLUS_USER + 53)
#define NPPM_GETCURRENTDIRECTORY     (NOTEPADPLUS_USER + 54)
#define NPPM_GETCURRENTFILENAME      (NOTEPADPLUS_USER + 55)
#define NPPM_GETFULLPATHFROMBUFFERID (NOTEPADPLUS_USER + 58)
#define NPPM_GETCURRENTBUFFERID      (NOTEPADPLUS_USER + 60)

// NPPM_GETBUFFERIDFROMPOS
//   Returns the internal buffer ID for a tab identified by view and position.
//   wParam: (WPARAM)(int) zero-based tab index within the view
//   lParam: (LPARAM)(int) view — 0 = primary, 1 = secondary
//   Returns: uptr_t buffer ID usable with NPPM_GETFULLPATHFROMBUFFERID,
//            or (uptr_t)-1 if the position is out of range.
#define NPPM_GETBUFFERIDFROMPOS      (NOTEPADPLUS_USER + 66)

// ── Notification codes (NPPN_*) ──────────────────────────────────────────────
//
// Notepad++ sends these to every plugin's beNotified() function via a
// SCNotification whose nmhdr.code field equals the NPPN_* value.
//
// For file-related notifications (FILEOPENED, FILESAVED, FILEBEFORECLOSE):
//   nmhdr.idFrom = buffer ID of the affected file (cast to UINT_PTR)
//   nmhdr.hwndFrom = Notepad++ main window handle
//
// For NPPN_READY and NPPN_SHUTDOWN:
//   nmhdr.idFrom = 0 (unused)
// ─────────────────────────────────────────────────────────────────────────────

// Base offset for all NPPN_* notification codes.
// NPPN_FIRST == NOTEPADPLUS_USER == 0x0800.
//
// The sequential offsets below match the canonical Notepad++ source exactly.
// An earlier version of this stripped header had NPPN_FILEOPENED at +7 and
// NPPN_FILESAVED at +6; both were wrong.  The correct offsets are +5 and +9
// respectively.  Using the wrong values caused the plugin to match
// NPPN_FILEBEFOREOPEN (+7) instead of NPPN_FILEOPENED, and NPPN_FILECLOSED
// (+6) instead of NPPN_FILESAVED — so auto-locking and save-path refresh
// never fired on the right events.
#define NPPN_FIRST (NOTEPADPLUS_USER + 000)

// Notepad++ has finished loading and is ready to be used.
// Safe point to perform any one-time plugin initialisation that needs the
// editor window to already exist.
#define NPPN_READY (NPPN_FIRST + 1)

// Notepad++ is about to shut down.
// Release all resources and handles here; DLL unload follows shortly after.
#define NPPN_SHUTDOWN (NPPN_FIRST + 2)

// A file tab is about to be closed.
// nmhdr.idFrom = buffer ID of the tab being closed.
// Plugins MUST release any handles to this file here to allow Notepad++ to
// complete the close operation.
#define NPPN_FILEBEFORECLOSE (NPPN_FIRST + 4)

// A file has just been opened in a new tab.
// nmhdr.idFrom = buffer ID of the newly-opened file.
// Correct offset is +5.  The earlier stripped header incorrectly used +7
// (which is NPPN_FILEBEFOREOPEN), so this notification was never received.
#define NPPN_FILEOPENED (NPPN_FIRST + 5)

// A file tab has been closed (after-close counterpart to NPPN_FILEBEFORECLOSE).
// nmhdr.idFrom = buffer ID of the closed file.
// Listed here because its numeric value (+6 = 0x0806) coincides with
// NPPM_SETMENUITEMCHECK; they travel in opposite directions so there is no
// conflict, but naming it here prevents future confusion.
#define NPPN_FILECLOSED (NPPN_FIRST + 6)

// A file has just been saved (including Save-As to a new path).
// nmhdr.idFrom = buffer ID of the saved file.
// After a Save-As the buffer ID remains the same but the file path changes,
// so any path-based operations (e.g. file locks) must be refreshed here.
// Correct offset is +9.  The earlier stripped header incorrectly used +6
// (which is NPPN_FILECLOSED), so this notification was never received.
#define NPPN_FILESAVED (NPPN_FIRST + 9)

// A buffer has become the active (visible) buffer — fires after the tab switch
// and title bar update are complete.  nmhdr.idFrom = buffer ID of the newly
// active buffer.  Unlike NPPN_FILEOPENED (which fires before the title bar
// reflects the new file), this notification is safe to use with
// getCurrentFilePath().
#define NPPN_BUFFERACTIVATED (NPPN_FIRST + 10)

// Notepad++ is about to rename the currently active file.
// nmhdr.idFrom = buffer ID of the file being renamed.
// Release any handles that deny FILE_SHARE_DELETE here so that
// MoveFileExW (called by Notepad++) can proceed.
#define NPPN_FILEBEFORERENAME  (NPPN_FIRST + 21)

// The rename was cancelled (e.g. user dismissed the rename dialog or the
// MoveFileExW call failed).  nmhdr.idFrom = buffer ID.
// Restore any handles that were released in NPPN_FILEBEFORERENAME.
#define NPPN_FILERENAMECANCEL  (NPPN_FIRST + 22)

// The file has been renamed successfully.
// nmhdr.idFrom = buffer ID.  The title bar now reflects the new filename.
// Re-acquire handles / update path tracking for the new path.
#define NPPN_FILERENAMED       (NPPN_FIRST + 23)

#endif  // NOTEPAD_PLUS_MSGS_H
