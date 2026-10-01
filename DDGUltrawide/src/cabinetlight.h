#pragma once
#include <d3d11.h>

// The arcade cabinet drawn behind the screens (ArcadeCabinet), lit by the game's
// own screens: each screen is a grid of area lights, their light bounces off the
// booth around the cabinet, and both light the cabinet's surfaces (config.cpp,
// GetCabinetScene) according to their depth and material.

// Creates the shaders and states. False if that fails (the cabinet then isn't drawn).
bool CabinetLightInit(ID3D11Device* dev);

// Draws the lit cabinet for an outWidth x height main window into target, a
// width x height canvas with the window's content shifted right by offset (the
// same as the window when there's no perspective warp). frame is the game's
// frame (ideally with a full mip chain). False, drawing nothing, when the
// cabinet is off. Changes the pipeline state; the caller sets its own afterwards.
bool CabinetLightRender(ID3D11DeviceContext* ctx, int outWidth, int height, int width, int offset,
                        ID3D11ShaderResourceView* frame, ID3D11RenderTargetView* target);

// Draws the acrylic sheets over the screens (after they're drawn into target):
// the screens and cabinet show through, and the booth, lit by the screens,
// reflects off both faces. Call after CabinetLightRender, with the same target.
// False when there's nothing to draw. Changes the pipeline state.
// Without sheets (the flat view: the side screens are drawn straight on, so
// reflections off their turned faces wouldn't match), only the chrome screws
// reflect, but what they and the hood reflect is still copied.
bool CabinetLightReflect(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, bool sheets);

// The acrylic's reflections alone (linear, the canvas's size), from the last
// CabinetLightReflect: for drawing the screens again, sharper, with them on top
// (the perspective warp does). Null when there are none.
ID3D11ShaderResourceView* CabinetLightReflections();

// Draws the hood under the center screen over everything, in 3D, into target
// (a window x height output; call last). With perspective, it's seen from the
// perspective camera, and target is the output (after the warp; the canvas is
// offset to the right of it by offset); otherwise straight on, and target is
// the canvas itself (offset 0). Needs CabinetLightReflect first (its copy of the
// screens is what the hood reflects). Changes the pipeline state.
bool CabinetLightHood(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, int width, int height,
                      int offset, bool perspective);

// With perspective: draws the screw heads again over the warped view (target,
// the output, width x height; the canvas offset to its right by offset), traced
// from the camera at their true size, so they're as sharp at the window's edges
// as in the middle. Needs CabinetLightReflect first. Changes the pipeline state.
bool CabinetLightScrews(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, int width, int height, int offset);

// Rebuilds the cabinet on the next render, after a setting it's built from changed.
void CabinetLightInvalidate();
