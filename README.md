<img width="5120" height="1440" alt="image" src="https://github.com/user-attachments/assets/89840846-e50e-43d1-a209-5699d463b5e2" />

# DDGUltrawide

A drop-in `version.dll` for Densha de Go!! AC (TG4AC, 5.80.02) that shows the
cabinet's four screens on a single ultrawide monitor.

## How it works

The game runs unmodified, rendering the same 3840x2160 frame as the
cabinet: four 1920x1080 screens in a 2x2 grid (three forward windows and the
touch panel). The DLL:

- **Composites the output.** Each panel of the 2x2 base grid is copied to a new
  widescreen target each frame.
- **Sets the render size.** Some loaders (e.g. TeknoParrot) try to override the 
  resolution settings, but we need it to be at the base 3840x2160, so the .dll
  hooks the command line (`GetCommandLineW`) to ensure that the correct resolution
  is being used for launch.
- **Forwards touch.** Clicks on the touch panel in the output window are mapped
  to the matching point in the game's panel and passed to the game, which treats
  mouse clicks as touches. `GetCursorPos`, `WindowFromPoint` and `SetCursorPos`
  are hooked so the game sees the cursor where it expects it.

## Build

1. Open `DDGUltrawide.sln` in Visual Studio 2022, pick **Release | x64**, and
   use **Build -> Rebuild Solution**.
2. The result is `bin\Release\version.dll`. It uses the static runtime, so no
   Visual C++ Redistributable is needed.

## Install

1. Copy `version.dll` and `DDGUltrawide.ini` into `<densha 5.80.02 root>\TG4AC\Binaries\Win64`.
2. TeknoParrot's resolution and windowed settings don't matter: the DLL makes
   the game render in a 3840x2160 window regardless (`[Game]` in the ini).
3. Adjust `[Output]` in the ini if your monitor isn't 5120x1440.

A log is written to `DDGUltrawide.log` next to the DLL to help with troubleshooting.

## Configuration

See the comments in `DDGUltrawide.ini`. In short:

- `[Output]`: size and position of the output window, vsync, pixel snapping, and
  how to keep the game's own window out of sight.
- `[Layout]` / `[Source]`: for each screen, where to draw it in the output and
  where it is in the game's frame, as fractions (e.g. `1/3`). Or set
  `ArcadeLayout=1` to size the screens like the cabinet (smaller 42" side
  screens beside the 55" center, flush along the bottom, with an `ArcadeGap`
  between them, 2" by default), computed automatically for your output size.
- `[TouchPanelWindow]`: shows the touch panel full screen on a separate monitor
  (e.g. a touchscreen) instead of in the main output. Pick the monitor by the
  number Windows shows for it, or place the window explicitly.
- `[Touch]`: which screens accept clicks, and whether to hide the cursor.

### Using a separate touchscreen

Set `Enabled=1` under `[TouchPanelWindow]`. If taps on the touchscreen move the
cursor on the wrong monitor, tell Windows which display the touchscreen belongs
to: Control Panel -> Tablet PC Settings -> Setup -> Touch input.
- `[Game]`: the size the game renders at (3840x2160, the cabinet's), and extra
  command line options.

## Uninstall

Delete `version.dll`, `DDGUltrawide.ini` and `DDGUltrawide.log`.

## Known Incompatibilities

- Currently does not work with `UE4SS` if the inspection windows are shown.
