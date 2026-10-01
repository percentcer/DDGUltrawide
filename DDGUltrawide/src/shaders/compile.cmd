@echo off
rem Compiles the shaders to Direct3D bytecode (DXBC, which any GPU's driver
rem takes), as C++ headers the DLL embeds: one per entry point, in %2.
rem   compile.cmd <path to fxc.exe> <output folder>
setlocal
set "FXC=%~1"
set "OUT=%~2"
set "SRC=%~dp0"
if not exist "%OUT%" mkdir "%OUT%"

for %%E in (VSFull VSShape) do call :one cabinetlight %%E vs_4_0 || exit /b 1
for %%E in (PSShape PSCells PSPatches PSBounce PSCube PSGloss PSShade PSBoothMap PSAcrylic PSHood PSChrome PSAO PSScrew) do call :one cabinetlight %%E ps_4_0 || exit /b 1
for %%E in (VSMain) do call :one compositor %%E vs_4_0 || exit /b 1
for %%E in (PSMain PSWarp PSPanel) do call :one compositor %%E ps_4_0 || exit /b 1
exit /b 0

:one
"%FXC%" /nologo /O1 /T %3 /E %2 /Vn g_%1_%2 /Fh "%OUT%\%1_%2.h" "%SRC%%1.hlsl" >nul 2>"%OUT%\%1_%2.err"
if errorlevel 1 goto failed
del "%OUT%\%1_%2.err" 2>nul
exit /b 0
:failed
echo %SRC%%1.hlsl: entry point %2 failed to compile:
type "%OUT%\%1_%2.err"
exit /b 1
