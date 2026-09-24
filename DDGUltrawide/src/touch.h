#pragma once
#include <windows.h>

// Mouse/touch forwarding from the output window to the game window.
// Clicks on the configured regions of the output are mapped to the matching
// point in the game's own frame, and GetCursorPos / WindowFromPoint are hooked
// so the game sees the cursor at that point.
bool InstallTouchHooks();

void TouchSetOutputWindow(HWND out);
void TouchSetGameWindow(HWND game);

// Called from the output window's WndProc. Returns true if the message was handled.
bool TouchHandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
