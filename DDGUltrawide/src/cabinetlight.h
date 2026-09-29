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

// Rebuilds the cabinet on the next render, after a setting it's built from changed.
void CabinetLightInvalidate();
