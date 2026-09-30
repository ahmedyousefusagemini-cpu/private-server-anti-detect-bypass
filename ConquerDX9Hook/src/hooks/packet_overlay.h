#pragma once

// ============================================================================
// PacketOverlay - in-game packet viewer for Conquer.exe
// ----------------------------------------------------------------------------
// Draws a window on top of the game (inside the D3D9 EndScene hook) listing
// every captured packet, with a hex/ASCII detail pane for the selected one.
//
// Rendering is done with plain GDI into a 32-bit DIB, which is uploaded to a
// D3DPOOL_MANAGED texture and drawn as a screen-space quad. No ImGui, no
// D3DX - nothing beyond d3d9.dll, so it builds against the same toolset as
// the rest of the proxy.
//
// Controls
//   F8         show / hide the window
//   F9         pause / resume capture
//   F10        clear the list
//   Esc        hide the window
//   wheel      scroll the list (scrolling up detaches auto-follow)
//   click row  select a packet (detail pane at the bottom)
//   drag title move the window
// ============================================================================

#include <windows.h>
#include <d3d9.h>

namespace PacketOverlay {

	// Called every frame from HookedEndScene, before the original EndScene.
	void OnEndScene(LPDIRECT3DDEVICE9 device);

	// Called from HookedReset. The overlay's resources are D3DPOOL_MANAGED so
	// they survive a device reset; these exist so a pool change stays a
	// one-line edit.
	void OnLostDevice();
	void OnResetDevice();

	// Window procedure hook entry point. Returns true when the overlay
	// consumed the message (the caller must then not forward it to the game).
	bool OnWindowMessage(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam);

	// The window the D3D9 device renders into; used to map the cursor into
	// backbuffer coordinates.
	void SetRenderWindow(HWND windowHandle);

	bool IsVisible();
	void SetVisible(bool visible);
}
