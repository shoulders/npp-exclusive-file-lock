// PluginDefinition.h
//
// ExclusiveFileLock – Notepad++ Plugin
//
// This header follows the layout of the official Notepad++ Plugin Template
// (https://github.com/npp-plugins/plugintemplate).  It is the single place
// where plugin-specific identity and the command count are declared, and where
// the four-step comment guides used by the template are preserved.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  STEP 1  Define the plugin name                                         │
// │  STEP 2  Define the number of plugin commands (menu items)              │
// │  STEP 3  Forward-declare the callback function for each command         │
// │  STEP 4  (In PluginDefinition.cpp) Implement each callback              │
// └─────────────────────────────────────────────────────────────────────────┘

#ifndef PLUGIN_DEFINITION_H
#define PLUGIN_DEFINITION_H

// <tchar.h> must be included HERE (not just in the .cpp) because this header
// uses the _T() macro directly in the PLUGIN_NAME definition below.
// _T() maps string literals to wchar_t* in Unicode builds (TCHAR = wchar_t)
// or char* in ANSI builds.  Without this include, any translation unit that
// includes PluginDefinition.h before pulling in <tchar.h> itself will fail
// with "error C3861: '_T': identifier not found".
#include <tchar.h>

#include "PluginInterface.h"    // NppData, FuncItem, PFUNCPLUGINCMD, exports

//-------------------------------------//
//-- STEP 1. DEFINE YOUR PLUGIN NAME --//
//-------------------------------------//
//
// PLUGIN_NAME is the text that appears in the Notepad++ Plugins top-level menu
// and is returned by the exported getName() function.
// Use the _T() macro so it compiles correctly in both Unicode and ANSI builds.
//
const TCHAR PLUGIN_NAME[] = _T("ExclusiveFileLock");

//-----------------------------------------------//
//-- STEP 2. DEFINE YOUR PLUGIN COMMAND NUMBER --//
//-----------------------------------------------//
//
// nbFunc is the total number of entries in the funcItem[] array, including
// separator lines.  Update this constant whenever you add or remove a command.
//
// Current menu layout:
//   [0] Toggle File Locking (On/Off)   ← master on/off toggle
//   [1] ---                            ← separator
//   [2] Lock Current File              ← manually lock active tab
//   [3] Unlock Current File            ← manually unlock active tab
//   [4] Show Lock Status               ← show all currently-locked files
//   [5] ---                            ← separator
//   [6] Add Read-only                  ← set FILE_ATTRIBUTE_READONLY on locked files
//
const int nbFunc = 7;

//--------------------------------------------//
//-- STEP 3. CUSTOMIZE YOUR PLUGIN COMMANDS --//
//--------------------------------------------//
//
// Forward-declare each callback function that will be assigned to a menu item.
// The implementations live in PluginDefinition.cpp.
// All callbacks must match the PFUNCPLUGINCMD signature: void fn(void).

// Toggles automatic file locking on or off (with a menu check-mark).
void toggleLocking();

// Manually acquires an exclusive lock on the currently active tab's file.
void lockCurrentFile();

// Manually releases the lock on the currently active tab's file.
void unlockCurrentFile();

// Displays a message box listing every file currently locked by this plugin.
void showLockStatus();

// Toggles the "add read-only attribute" option on locked files.
void toggleAddReadOnly();

//----------------------------------------------//
//-- STEP 4. DEFINE YOUR ASSOCIATED FUNCTIONS --//
//----------------------------------------------//
//
// The function bodies for the commands above are written in PluginDefinition.cpp.
// Plugin initialisation and cleanup called from DllMain live there too.

// Called once from DllMain(DLL_PROCESS_ATTACH): one-time plugin setup.
void pluginInit(HANDLE hModule);

// Called once from DllMain(DLL_PROCESS_DETACH): release all resources.
void pluginCleanUp();

// Called from the exported setInfo(): stores NppData and sets up menu items.
void commandMenuInit();

// Called from the exported beNotified(): reacts to Notepad++ events.
// (The dispatcher itself is in the exported beNotified(); this helper
//  contains the plugin-specific logic.)
void commandMenuCleanUp();

#endif  // PLUGIN_DEFINITION_H