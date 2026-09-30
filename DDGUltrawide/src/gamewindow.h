#pragma once
#include <windows.h>

// Lets the game's window grow larger than the screen: Windows normally limits a
// window to about the desktop's size, which would stop the game's 3840x2160
// window on smaller monitors. Hooks CreateWindowExW so the game's windows are
// adjusted from the moment they're created.
bool InstallGameWindowHook();

// Moves the game's window so the part of its frame holding the touch screens
// is on the desktop (and keeps it there): the game reads the cursor's desktop
// position while driving, and clamps it to the desktop, so a touch screen
// hanging off it wouldn't take touches below the desktop's edge. It's behind
// our output window, so where it sits doesn't show.
void PlaceGameWindow(HWND game);

// Makes the game's window the foreground window (on its own thread, soon), so
// keys go to the game: Alt+F4 otherwise closes whatever had the focus before.
void FocusGameWindow(HWND game);
