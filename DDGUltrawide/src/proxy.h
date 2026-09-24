#pragma once

// Loads the real System32 version.dll and resolves every export our thunks forward to.
bool LoadOriginalVersionDll();
