# DDGUltrawide

A drop-in `version.dll` for Densha de Go!! AC (TG4AC, 5.80.02) that shows the
cabinet's four screens on a single ultrawide monitor.

## How it works

The game runs completely unmodified, rendering the same 3840x2160 frame as the
cabinet: four 1920x1080 screens in a 2x2 grid (three forward windows and the
touch panel). The DLL:

- **Composites the output.** Each frame, just before the game presents it, the
  DLL copies it and redraws the four screens into its own borderless window in
  the configured layout, with smooth (mipmapped) downscaling. The output window
  stays above the game's window and never takes focus from it.
- **Sets the render size.** The game takes its resolution from its command
  line, and TeknoParrot replaces that command line with its own, including
  `-ResX=1920 -ResY=1080`. The DLL patches the game executable's import table so
  the game's `GetCommandLineW` calls come to it first: it fetches the command
  line (through TeknoParrot's hook), replaces the resolution and window-mode
  options with `-ResX=3840 -ResY=2160 -windowed -ForceRes`, and keeps the rest
  (such as `-UserDir`). Windows' limit on window size is lifted for the game's
  window so it can be larger than the monitor.
- **Forwards touch.** Clicks on the touch panel in the output window are mapped
  to the matching point in the game's panel and passed to the game, which treats
  mouse clicks as touches. `GetCursorPos`, `WindowFromPoint` and `SetCursorPos`
  are hooked so the game sees the cursor where it expects it, and the real
  cursor stays where you put it.

Because the game renders exactly as on the cabinet, every screen's UI looks as
designed.

## Build

1. Put MinHook's `include` and `src` folders in `third_party\minhook`
   (see `third_party\minhook\PUT_MINHOOK_HERE.txt`).
2. Open `DDGUltrawide.sln` in Visual Studio 2022, pick **Release | x64**, and
   use **Build -> Rebuild Solution**.
3. The result is `bin\Release\version.dll`. It uses the static runtime, so no
   Visual C++ Redistributable is needed.

## Install

1. Copy `version.dll` and `DDGUltrawide.ini` into `TG4AC\Binaries\Win64`.
2. TeknoParrot's resolution and windowed settings don't matter: the DLL makes
   the game render in a 3840x2160 window regardless (`[Game]` in the ini).
3. Adjust `[Output]` in the ini if your monitor isn't 5120x1440 at the top-left
   of the desktop.

A log is written to `DDGUltrawide.log` next to the DLL.

## Configuration

See the comments in `DDGUltrawide.ini`. In short:

- `[Output]`: size and position of the output window, vsync, pixel snapping, and
  how to keep the game's own window out of sight.
- `[Layout]` / `[Source]`: for each screen, where to draw it in the output and
  where it is in the game's frame, as fractions (e.g. `1/3`).
- `[Touch]`: which screens accept clicks, and whether to hide the cursor.
- `[Game]`: the size the game renders at (3840x2160, the cabinet's), and extra
  command line options.

## Uninstall

Delete `version.dll`, `DDGUltrawide.ini` and `DDGUltrawide.log`.
