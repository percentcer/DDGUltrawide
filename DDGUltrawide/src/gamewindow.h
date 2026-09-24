#pragma once

// Lets the game's window grow larger than the screen: Windows normally limits a
// window to about the desktop's size, which would stop the game's 3840x2160
// window on smaller monitors. Hooks CreateWindowExW so the game's windows are
// adjusted from the moment they're created.
bool InstallGameWindowHook();
