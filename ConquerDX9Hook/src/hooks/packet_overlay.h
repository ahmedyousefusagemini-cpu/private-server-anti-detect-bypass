#pragma once

// ============================================================================
// PacketOverlay - in-game bot panel for Conquer.exe
// ----------------------------------------------------------------------------
// Draws a Dear ImGui window on top of the game (inside the D3D9 EndScene hook).
// The window is a tabbed bot-panel shell (Player / Map / Packets / Misc /
// Plugins); the packet logger - the live capture table plus the hex/ASCII dump
// of the selected packet - lives in the "Packets" tab.
//
// Rendering goes through imgui_impl_dx9 with imgui_impl_win32 as the platform
// backend; the lifecycle and input plumbing live in imgui_bridge.cpp. The
// vendored ImGui sources are under libs/imgui (see docs/overlay-imgui.md).
//
// Controls
//   Insert     show / hide the window
//   F8         show / hide the window (alias)
//   F9         pause / resume capture
//   F10        clear the list
//   Esc        hide the window
//   wheel      scroll the packet list / hex dump
//   click row  select a packet (hex dump below the table)
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

	// True when the user clicked "Save Settings" since the last call; the flag
	// is cleared on read, so the caller sees each click exactly once. The
	// click already persisted the panel state to overlay.ini - this is only
	// for a caller that wants to mirror the settings elsewhere.
	bool ConsumeSaveSettingsRequest();

	// Writes the panel's filter/tab state to overlay.ini next to the game exe.
	// Called by the Save Settings button and by the autosave tick; exposed so
	// the shutdown path can flush too.
	void SaveSettings();
}
