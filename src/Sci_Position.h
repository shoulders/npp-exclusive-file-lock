// Sci_Position.h
// Scintilla source code edit control
//
// This file defines the Sci_Position type used in Scintilla's API.
// It is a dependency of Scintilla.h and must be included before it.
//
// Sourced from the Notepad++ repository:
//   https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Sci_Position.h
//
// Copyright 1998-2017 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#ifndef SCI_POSITION_H
#define SCI_POSITION_H

// Sci_Position is a type wide enough to store a position in a document.
// Currently defined as ptrdiff_t so it is 32 bits on 32-bit platforms and
// 64 bits on 64-bit platforms which may allow documents larger than 2 GB.

#include <stddef.h>

#ifdef _WIN32
    // On Windows, ptrdiff_t is defined in <stddef.h>
    typedef ptrdiff_t Sci_Position;
    typedef size_t    Sci_PositionU;      // Unsigned variant used for lengths
    typedef ptrdiff_t Sci_PositionCR;    // Variant used in CellBuffer
#else
    typedef ptrdiff_t Sci_Position;
    typedef size_t    Sci_PositionU;
    typedef ptrdiff_t Sci_PositionCR;
#endif

// Sci_Line is a line number, wide enough to hold any line number in a large document.
typedef ptrdiff_t Sci_Line;

#endif  // SCI_POSITION_H
