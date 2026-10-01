#pragma once

// ============================================================================
// ImGuiBridge - Dear ImGui lifecycle for the Conquer.exe D3D9 overlay
// ----------------------------------------------------------------------------
// Initialises ImGui (core + DX9 renderer + Win32 platform) on the game's
// device, clones the two backend NewFrame functions so no separate
// ImGui_ImplWin32_NewFrame()/ImGui_ImplDX9_NewFrame() calls are needed, and
// hands the ImGui input queue the swap-chain viewport.
//
// The ImGui sources are vendored under libs\imgui (see libs\imgui\LICENSE.txt).
//
//   Init(device, window)   create the context and both backends
//   NewFrame()             io.DisplaySize/DeltaTime/inputs + frame begin
//   RenderDrawData()       submit the frame through imgui_impl_dx9
//   Shutdown()             release everything
//
// The Win32 backend's ImGui_ImplWin32_WndProcHandler() must also be called
// from the window procedure; see imgui_bridge.cpp for how the overlay forwards
// messages and swallows input it consumes.
// ============================================================================

#include <windows.h>
#include <d3d9.h>

namespace ImGuiBridge
{
	// Creates the ImGui context and initialises both backends against the
	// game's device. Returns false if they are already up, if the device is
	// null, or if a backend init fails.
	bool Init(LPDIRECT3DDEVICE9 device, HWND window);

	// Feeds one window message to ImGui (position, buttons, wheel, keys,
	// characters, focus). Returns true when the overlay should swallow the
	// message so the game does not also react to it.
	bool ProcessMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

	// One call per frame: refreshes display size / delta time / keyboard /
	// mouse, re-points the main viewport at the current backbuffer (the game
	// recreates that surface on every device reset), then calls
	// ImGui::NewFrame(). Safe to call every frame; initialises lazily.
	void NewFrame(LPDIRECT3DDEVICE9 device);

	// Submits the built frame through imgui_impl_dx9. Safe when there is
	// nothing to draw.
	void RenderDrawData();

	// Device-lost / device-created plumbing for the D3D9 Reset hook.
	void InvalidateDeviceObjects();
	bool CreateDeviceObjects();

	void Shutdown();

	bool IsInitialised();
}
