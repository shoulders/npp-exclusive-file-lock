// Scintilla.h
// Scintilla source code edit control
//
// Interface to the Scintilla editing component.  This header defines the
// message constants, type aliases (uptr_t / sptr_t), and SCNotification
// structure that every Notepad++ plugin uses when communicating with Scintilla.
//
// This is a trimmed, plugin-focused version of the full Scintilla.h.
// The full version is maintained at:
//   https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Scintilla.h
//
// Copyright 1998-2021 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#ifndef SCINTILLA_H
#define SCINTILLA_H

// Sci_Position.h must be included before this file to define Sci_Position.
#include "Sci_Position.h"

// ── Scintilla integer types ──────────────────────────────────────────────────
//
// uptr_t  Unsigned pointer-sized integer.  Used for buffer IDs, line numbers,
//         and any value that must be at least pointer-width.
//         Maps to UINT_PTR on Windows (32-bit or 64-bit depending on target).
//
// sptr_t  Signed pointer-sized integer.  Used for return values and signed
//         parameters in the Scintilla message API.
//         Maps to INT_PTR on Windows.
//
// These are the canonical types used throughout the Notepad++ plugin API.
// They appear in NppData, FuncItem, SCNotification, and all SendMessage calls.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
    #include <windows.h>            // UINT_PTR, INT_PTR, HWND
    typedef UINT_PTR uptr_t;
    typedef INT_PTR  sptr_t;
#else
    #include <stdint.h>
    typedef uintptr_t uptr_t;
    typedef intptr_t  sptr_t;
#endif

// ── SCNotification ───────────────────────────────────────────────────────────
//
// The structure Notepad++ passes to beNotified() for every event.
//
// The nmhdr member is a standard Windows NMHDR structure:
//   nmhdr.hwndFrom = HWND of the Notepad++ or Scintilla window that fired it.
//   nmhdr.idFrom   = extra data; for Notepad++ file-events, this is the
//                    buffer ID (uptr_t) cast to UINT_PTR.
//   nmhdr.code     = notification code (NPPN_* for Npp events, SCN_* for
//                    Scintilla events).
//
// All fields below nmhdr are Scintilla-specific and are zero for pure Npp
// notifications (NPPN_FILEOPENED, NPPN_FILESAVED, etc.).  They only carry
// meaningful data for low-level Scintilla events (SCN_MODIFIED, SCN_CHARADDED,
// etc.) which this plugin does not use.
// ─────────────────────────────────────────────────────────────────────────────
struct SCNotification {
    NMHDR           nmhdr;               // Standard Windows notification header

    Sci_Position    position;            // SCN_*: character position of event
    int             ch;                  // SCN_CHARADDED: the character added
    int             modifiers;           // SCN_KEY: modifier-key flags
    int             modificationType;   // SCN_MODIFIED: what changed (bitmask)
    const char*     text;               // SCN_MODIFIED: pointer to changed text
    Sci_Position    length;             // SCN_MODIFIED: length of changed text
    Sci_Position    linesAdded;         // SCN_MODIFIED: lines added (negative = removed)
    int             message;            // SCN_MACRORECORD: message recorded
    uptr_t          wParam;             // SCN_MACRORECORD: wParam of recorded msg
    sptr_t          lParam;             // SCN_MACRORECORD: lParam of recorded msg
    Sci_Position    line;               // SCN_*: line number related to event
    int             foldLevelNow;       // SCN_MODIFIED: new fold level
    int             foldLevelPrev;      // SCN_MODIFIED: previous fold level
    int             margin;             // SCN_MARGINCLICK: margin index clicked
    int             listType;           // SCN_USERLISTSELECTION: list type
    int             x;                  // SCN_DWELLSTART/END: x-coordinate
    int             y;                  // SCN_DWELLSTART/END: y-coordinate
    int             token;              // SCN_MODIFIED (SC_MOD_CONTAINER only)
    Sci_Position    annotationLinesAdded; // SCN_MODIFIED: annotation delta
    int             updated;            // SCN_UPDATEUI: bitmask of what changed
    int             listCompletionMethod; // SCN_AUTOCSELECTION: how item chosen
    int             characterSource;    // SCN_CHARADDED: how character was added
};

#endif  // SCINTILLA_H
