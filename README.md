<img width="5119" height="1439" alt="image" src="https://github.com/user-attachments/assets/57c3c7c6-e355-482f-9065-cc6c590e0a74" />

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
  are hooked so the game sees the cursor where it expects it. Mouse wheel input
  (throttle and brake) over any of the DLL's windows is passed to the game too.

## Build

1. Open `DDGUltrawide.sln` in Visual Studio 2022, pick **Release | x64**, and
   use **Build -> Rebuild Solution**.
2. The result is `bin\Release\version.dll`. It uses the static runtime, so no
   Visual C++ Redistributable is needed. A Release build also packages it with
   `DDGUltrawide.ini` into `DDGUltrawide.zip` in the repo root, ready to share.

The shaders (`src\shaders\*.hlsl`) are compiled during the build with the
Windows SDK's `fxc` (`src\shaders\compile.cmd`) and embedded in the DLL as
bytecode, so nothing is compiled or written to disk at runtime.

## Install

1. Copy `version.dll` and `DDGUltrawide.ini` into `<densha 5.80.02 root>\TG4AC\Binaries\Win64`.
2. TeknoParrot's resolution and windowed settings don't matter: the DLL makes
   the game render in a 3840x2160 window regardless (`[Game]` in the ini).
3. By default the output fills your main monitor at its resolution; set
   `[Output]` in the ini to use another monitor or a different size.

A log is written to `DDGUltrawide.log` next to the DLL to help with troubleshooting.

### Antivirus warnings

Some antivirus software may flag `version.dll`. It's a false positive: the DLL
works the way game mods do, which heuristics often mistake for malware. It
stands in for a Windows DLL so that the game loads it, hooks a few of the
game's functions (presenting frames, the cursor, window creation and its
command line), and redraws the game's screens. It doesn't download, install or
change anything outside the game, and only writes its own log. The source is
all here to read and build yourself.

If it's quarantined, restore it and add an exclusion for the game's
`TG4AC\Binaries\Win64` folder (with Microsoft Defender: Windows Security >
Virus & threat protection > Manage settings > Exclusions). With Defender, you
can also report the false positive at https://www.microsoft.com/wdsi/filesubmission.

## Configuration

See the comments in `DDGUltrawide.ini`. In short:

- `[Output]`: size and position of the output window (by default, the whole
  main monitor), vsync, and pixel snapping.
- `[Layout]`: by default (`ArcadeLayout=1`) the screens are sized like the
  cabinet (smaller 43" side screens beside the 55" center, flush along the
  bottom, with an `ArcadeGap` between them, 2" by default), computed
  automatically for your output size, and drawn inside a picture of the
  cabinet (`ArcadeCabinet`: its walls in `ArcadeCabinetColor`, a metal frame
  with screws around each screen, acrylic sheets that reflect the booth, and
  the black hood under the center screen), lit by the game's own
  screens (`ArcadeCabinetScreenNits`, `ArcadeCabinetRoomLux`) and the booth's
  roof lights (`ArcadeCabinetRoofLights`, `ArcadeCabinetRoofLightGlow`; the
  hood's paint with `ArcadeCabinetHoodGloss`), with the side
  cabinets turned toward you in perspective (`ArcadeCameraDistance`, how far
  you stand from the center screen in mm, 2250 by default, 0 = flat; and
  `ArcadeFov`, the horizontal field of view in degrees, 0 = the center screen
  fills the window, cutting off what doesn't fit; with `[Hotkeys] Enabled=1`,
  Ctrl+Alt+arrows adjust both while playing). `ArcadeTouchPanelScaling`
  (0.75 by default) sets the touch panel's size, trading it against the forward
  screens', and `ArcadeTouchPanelAllowOverlap=1` lets the screens use the full
  width by drawing the touch panel over the bottom edge of the center screen.
  With `ArcadeLayout=0`, `[Layout]` and `[Source]` give each screen's position
  in the output and in the game's frame, as fractions (e.g. `1/3`).
- `[TouchPanelWindow]`: shows the touch panel full screen on a separate monitor
  (e.g. a touchscreen) instead of in the main output. Pick the monitor by the
  number Windows shows for it, or place the window explicitly.
- `[Touch]`: whether clicks on the touch panel reach the game, and whether to
  hide the cursor.
- `[Hotkeys]`: Ctrl+Alt+arrows to adjust the perspective while playing (off by
  default).
- `[Game]`: the size the game renders at (3840x2160, the cabinet's), and extra
  command line options.

### Using a separate touchscreen

Set `Enabled=1` under `[TouchPanelWindow]`. If taps on the touchscreen move the
cursor on the wrong monitor, tell Windows which display the touchscreen belongs
to: Control Panel -> Tablet PC Settings -> Setup -> Touch input.

## Uninstall

Delete `version.dll`, `DDGUltrawide.ini` and `DDGUltrawide.log`.

## Known Incompatibilities

- PowerToys FancyZones needs an exclusion for `TG4AC-Win64-Shipping.exe` (by default it will force the base canvas to resize which breaks the compositor. DDGUltrawide will try to detect and fix this but... no promises)

## License

DDGUltrawide is under the MIT License (`LICENSE`). Third-party code is under
its own license: MinHook (`third_party\minhook\LICENSE.txt`, BSD 2-clause).
Its notice is reproduced at the end of `DDGUltrawide.ini`, so it goes along
with every build; keep it there when sharing one.
