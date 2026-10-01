#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "MinHook.h"
#include "common.h"
#include "packet_overlay.h"
#include "log.h"

extern GameWindowInfo g_gameWindow;
extern EndSceneFunc g_originalEndSceneFunction;
extern ResetFunc g_originalResetFunction;
extern LPVOID g_originalEndSceneAddress;
extern LPVOID g_originalResetAddress;
extern HWND FindGameWindowHandle();

// Original window procedure of the render window. The root window's
// original procedure lives in g_gameWindow.originalWindowProcedure.
static WNDPROC g_originalChildWindowProcedure = NULL;

LRESULT CALLBACK HookedWindowProcedure(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam);

// Hooks the exact window the D3D9 device renders into (plus its root
// window for keyboard input). Far more reliable than searching window
// titles, which can match a launcher/updater window instead.
void InstallInputHooksFromDevice(LPDIRECT3DDEVICE9 device)
{
	if (!device)
		return;

	HWND renderWindow = NULL;

	// Preferred: the swap chain's device window (the actual render target).
	IDirect3DSwapChain9* swapChain = NULL;
	if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain)
	{
		D3DPRESENT_PARAMETERS presentParams;
		if (SUCCEEDED(swapChain->GetPresentParameters(&presentParams)))
			renderWindow = presentParams.hDeviceWindow;
		swapChain->Release();
	}

	// Fallback: the focus window from creation parameters.
	if (!renderWindow)
	{
		D3DDEVICE_CREATION_PARAMETERS creationParams;
		if (SUCCEEDED(device->GetCreationParameters(&creationParams)))
			renderWindow = creationParams.hFocusWindow;
	}

	if (!renderWindow)
		return;

	g_gameWindow.gameWindowHandle = renderWindow;
	g_gameWindow.parentWindowHandle = GetAncestor(renderWindow, GA_ROOT);

	// The overlay maps the cursor through this window.
	PacketOverlay::SetRenderWindow(renderWindow);

	// Hook the root window: keyboard input (WM_CHAR/WM_KEYDOWN) goes to the
	// top-level window with focus.
	if (g_gameWindow.parentWindowHandle &&
		g_gameWindow.parentWindowHandle != renderWindow &&
		!g_gameWindow.originalWindowProcedure)
	{
		g_gameWindow.originalWindowProcedure = (WNDPROC)SetWindowLongPtrA(
			g_gameWindow.parentWindowHandle, GWLP_WNDPROC, (LONG_PTR)HookedWindowProcedure);
	}

	// Hook the render window: mouse input goes to it.
	if (!g_originalChildWindowProcedure)
	{
		g_originalChildWindowProcedure = (WNDPROC)SetWindowLongPtrA(
			renderWindow, GWLP_WNDPROC, (LONG_PTR)HookedWindowProcedure);
	}

	HookLog("[Input] render=%p root=%p childProc=%p rootProc=%p",
		(void*)renderWindow, (void*)g_gameWindow.parentWindowHandle,
		(void*)g_originalChildWindowProcedure, (void*)g_gameWindow.originalWindowProcedure);
}

LRESULT CALLBACK HookedWindowProcedure(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
	// One-shot diagnostic: tells us whether keyboard messages reach this hook
	// at all. The overlay's hotkeys are polled rather than handled here
	// (DirectInput usually swallows them), so a silent absence is expected -
	// but it is worth being able to confirm it from the log.
	static bool loggedFirstKey = false;
	if (!loggedFirstKey && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN))
	{
		loggedFirstKey = true;
		HookLog("[Input] first keyboard message seen: vk=%u hwnd=%p",
			(unsigned)wParam, (void*)windowHandle);
	}

	// Forward to the correct original procedure for THIS window.
	WNDPROC originalProcedure = NULL;
	if (windowHandle == g_gameWindow.gameWindowHandle && g_originalChildWindowProcedure)
		originalProcedure = g_originalChildWindowProcedure;
	else
		originalProcedure = g_gameWindow.originalWindowProcedure;

	// Let the packet overlay eat its own mouse input first.
	if (PacketOverlay::OnWindowMessage(windowHandle, message, wParam, lParam))
		return 0;

	if (!originalProcedure)
		return DefWindowProcA(windowHandle, message, wParam, lParam);

	return CallWindowProcA(originalProcedure, windowHandle, message, wParam, lParam);
}

HRESULT WINAPI HookedEndScene(LPDIRECT3DDEVICE9 device)
{
	if (!g_gameWindow.direct3DDevice)
	{
		g_gameWindow.direct3DDevice = device;
	}

	// Discover the render/root window once, the first frame we see a device.
	if (!g_gameWindow.gameWindowHandle)
	{
		InstallInputHooksFromDevice(device);

		if (!g_gameWindow.gameWindowHandle)
			g_gameWindow.gameWindowHandle = FindGameWindowHandle();
	}

	// Draw the packet window on top of the frame, before the scene closes.
	PacketOverlay::OnEndScene(device);

	return g_originalEndSceneFunction(device);
}


HRESULT WINAPI HookedReset(LPDIRECT3DDEVICE9 device, D3DPRESENT_PARAMETERS* presentationParameters)
{
	MH_DisableHook(g_originalEndSceneAddress);

	PacketOverlay::OnLostDevice();

	HRESULT result = g_originalResetFunction(device, presentationParameters);

	if (SUCCEEDED(result))
	{
		PacketOverlay::OnResetDevice();
		MH_EnableHook(g_originalEndSceneAddress);
	}

	return result;
}