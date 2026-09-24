#pragma once
#include <string>

// Controls the command line the game sees. Launchers such as TeknoParrot hook
// GetCommandLineW and replace the game's command line with their own, including
// a fixed -ResX/-ResY. So instead of an inline hook (which may end up behind
// theirs), this patches the game executable's own import table: the game's
// calls come to us first, we fetch the command line (through any launcher
// hooks) and edit it:
//   - if renderW/renderH are set, remove existing resolution and window-mode
//     options and add -ResX/-ResY/-windowed/-ForceRes for that size
//   - append `extra`
bool InstallCommandLineHook(int renderW, int renderH, const std::wstring& extra);
