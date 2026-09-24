#pragma once

// Hooks UUserInterfaceSettings::GetDPIScaleBasedOnSize so the engine's UI scale
// makes the reference player's tile exactly DesignHeight UI units tall.
bool InstallUIScaleHook();
