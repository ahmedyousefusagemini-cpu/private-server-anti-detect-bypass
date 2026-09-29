#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "MinHook.h"
#include "common.h"
#include "anti_detect.h"

extern GameWindowInfo g_gameWindow;
extern EndSceneFunc g_originalEndSceneFunction;
extern ResetFunc g_originalResetFunction;
extern LPVOID g_originalEndSceneAddress;
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
}

LRESULT CALLBACK HookedWindowProcedure(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
	// Forward to the correct original procedure for THIS window.
	WNDPROC originalProcedure = NULL;
	if (windowHandle == g_gameWindow.gameWindowHandle && g_originalChildWindowProcedure)
		originalProcedure = g_originalChildWindowProcedure;
	else
		originalProcedure = g_gameWindow.originalWindowProcedure;

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

	// Per-frame anti-detection pass (stub for now).
	AntiDetect::PerFrame();

	return g_originalEndSceneFunction(device);
}


HRESULT WINAPI HookedReset(LPDIRECT3DDEVICE9 device, D3DPRESENT_PARAMETERS* presentationParameters) 
{
	MH_DisableHook(g_originalEndSceneAddress);

	HRESULT result = g_originalResetFunction(device, presentationParameters);

	if (SUCCEEDED(result)) 
	{
		MH_EnableHook(g_originalEndSceneAddress);
	}

	return result;
}
