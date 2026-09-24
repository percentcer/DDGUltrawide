#pragma once
#include <string>

// Appends extra options to the command line the game reads at startup
// (hooks GetCommandLineW). Must be installed before the game's own code runs.
bool InstallCommandLineHook(const std::wstring& extra);
