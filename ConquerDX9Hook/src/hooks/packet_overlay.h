#pragma once

// ============================================================================
// PacketOverlay - in-game packet viewer for Conquer.exe
// ----------------------------------------------------------------------------
// Draws a Dear ImGui window on top of the game (inside the D3D9 EndScene hook)
// listing every captured packet, with a hex/ASCII detail pane for the selected
// one.
//
// Rendering goes through imgui_impl_dx9 with imgui_impl_win32 as the platform
// backend; the lifecycle and input plumbing live in imgui_bridge.cpp. The
// vendored ImGui sources are under libs/imgui (see docs/overlay-imgui.md).
//
// Controls
//   F8         show / hide the window
//   F9         pause / resume capture
//   F10        clear the list
//   Esc        hide the window
//   wheel      scroll the list
//   click row  select a packet (detail pane at the bottom)
//   drag title move the window
// ============================================================================

#include <windows.h>
#include <d3d9.h>

namespace PacketOverlay {

	// Called every frame from HookedEndScene, before the original EndScene.
	void OnEndScene(LPDIRECT3DDEVICE9 device);

	// Called from HookedReset. ImGui's DX9 backend keeps its vertex/index
	// buffers and font texture in D3DPOOL_DEFAULT, so they must be released
	// before the reset and rebuilt after it.
	void OnLostDevice();
	void OnResetDevice();

	// Window procedure hook entry point. Returns true when the overlay
	// consumed the message (the caller must then not forward it to the game).
	bool OnWindowMessage(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam);

	// The window the D3D9 device renders into; used to map the cursor into
	// backbuffer coordinates and as ImGui's platform handle.
	void SetRenderWindow(HWND windowHandle);

	bool IsVisible();
	void SetVisible(bool visible);
}
