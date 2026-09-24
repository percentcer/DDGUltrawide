#pragma once

// Makes the game start at the given resolution (windowed). The game reads its
// resolution from Saved\Config\WindowsNoEditor\GameUserSettings.ini, which
// launchers such as TeknoParrot rewrite before the game starts. This hooks
// CreateFileW and corrects the resolution in that file at the moment the game
// opens it to read it.
bool InstallSettingsFileHook(int width, int height);
