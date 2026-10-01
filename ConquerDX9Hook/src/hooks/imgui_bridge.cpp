// ============================================================================
// ImGuiBridge - Dear ImGui lifecycle for the Conquer.exe D3D9 overlay
// ----------------------------------------------------------------------------
// The two official backends (imgui_impl_win32, imgui_impl_dx9) are used, but
// their NewFrame entry points are *cloned* here rather than called directly:
//
//   * the overlay renders inside the game's EndScene, so there is no
//     application-supplied HWND and no per-frame tick to drive
//     ImGui_ImplWin32_NewFrame(); we fill io manually
//   * on a D3D9 device reset the game destroys the swap chain, so the
//     RenderViewports / DisplaySize the backends cache go stale; we refresh
//     them every frame instead
//
// Everything else - input queueing, keyboard mapping, DPI, the wrapped DX9
// state block, the shader and font texture - is straight from the backends.
//
// References (v1.92.9b):
//   backends/imgui_impl_win32.cpp  -> NewFrame, UpdateMouseData, key mapping
//   backends/imgui_impl_dx9.cpp    -> RenderDrawData (state block + viewport)
//
// Input flow: HookedWindowProcedure (directx_hooks.cpp) routes messages to
// ImGuiBridge::ProcessMessage(). We let ImGui_ImplWin32_WndProcHandler queue
// the event and then decide, per message, whether the overlay should consume
// it (so the game does not also react to a click on the panel).
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cfloat>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include "imgui_bridge.h"
#include "log.h"

// imgui_impl_win32.h deliberately leaves this declaration inside an `#if 0`
// block (to avoid pulling <windows.h> in). Its official guidance is to copy
// the declaration into the file that calls it - so, here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ImGuiBridge
{
namespace {

	bool  g_initialised = false;
	HWND  g_hwnd = nullptr;

	// Local mirror of the platform backend's timing state. The Win32 backend
	// keeps its own copy inside ImGui_ImplWin32_Data; we cannot reach it from
	// here, so we keep our own QueryPerformanceCounter pair.
	INT64 g_ticksPerSecond = 0;
	INT64 g_lastCounter = 0;

	// Small helpers -------------------------------------------------------

	bool IsKeyDown(int virtualKey)
	{
		return (::GetKeyState(virtualKey) & 0x8000) != 0;
	}

	void UpdateKeyModifiers(ImGuiIO& io)
	{
		io.AddKeyEvent(ImGuiMod_Ctrl, IsKeyDown(VK_CONTROL));
		io.AddKeyEvent(ImGuiMod_Shift, IsKeyDown(VK_SHIFT));
		io.AddKeyEvent(ImGuiMod_Alt, IsKeyDown(VK_MENU));
		io.AddKeyEvent(ImGuiMod_Super, IsKeyDown(VK_LWIN) || IsKeyDown(VK_RWIN));
	}

	// Mirrors ImGui_ImplWin32_UpdateMouseData(): while the game owns the
	// foreground, feed ImGui the OS cursor position (it already streams while
	// the mouse is over the window, but this also fills gaps after alt-tab).
	void UpdateMouseData(ImGuiIO& io)
	{
		if (!g_hwnd) return;
		if (::GetForegroundWindow() != g_hwnd) return;

		if (io.WantSetMousePos)
		{
			POINT pos = { (int)io.MousePos.x, (int)io.MousePos.y };
			if (::ClientToScreen(g_hwnd, &pos))
				::SetCursorPos(pos.x, pos.y);
		}
		else
		{
			POINT pos;
			if (::GetCursorPos(&pos) && ::ScreenToClient(g_hwnd, &pos))
				io.AddMousePosEvent((float)pos.x, (float)pos.y);
		}
	}

	void UpdateMouseCursor(ImGuiIO& io)
	{
		// The game owns the cursor (we pass ImGuiConfigFlags_NoMouseCursorChange
		// at Init), so the OS cursor is never touched. Kept as a named hook so
		// the behaviour is explicit and easy to change.
		IM_UNUSED(io);
	}

} // namespace

// ---------------------------------------------------------------------------
// Per-message input, called from the window procedure BEFORE the game sees it.
// Returns true when the overlay should swallow the message.
// ---------------------------------------------------------------------------
bool ProcessMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (!g_initialised) return false;

	// Has the mouse already been captured by ImGui's input (dragging a
	// slider, resizing a column, moving the window)? If so, a button-up that
	// happens outside the panel must still reach it, so we consume early.
	const bool wantCaptureMouse = ImGui::GetIO().WantCaptureMouse;
	if (wantCaptureMouse &&
		(message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
		 message == WM_MBUTTONUP || message == WM_XBUTTONUP))
	{
		::ReleaseCapture();
		ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
		return true;
	}

	// Let the official handler queue the event (position, buttons, wheel,
	// keys, characters, IME, focus). It returns 1 for events it understood.
	ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);

	switch (message)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
		return ImGui::GetIO().WantCaptureMouse;

	case WM_LBUTTONUP: case WM_RBUTTONUP:
	case WM_MBUTTONUP: case WM_XBUTTONUP:
		return ImGui::GetIO().WantCaptureMouse;

	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
	case WM_MOUSEMOVE:
		return ImGui::GetIO().WantCaptureMouse;

	case WM_KEYDOWN: case WM_KEYUP:
	case WM_SYSKEYDOWN: case WM_SYSKEYUP:
	case WM_CHAR:
		// NOTE: the packet overlay's own hotkeys (F8/F9/F10/Esc) are polled
		// with GetAsyncKeyState and are deliberately NOT handled here - the
		// client drives its keyboard through DirectInput, so these messages
		// are not reliably delivered, and handling them twice would toggle
		// twice per press.
		return ImGui::GetIO().WantCaptureKeyboard;

	default:
		break;
	}

	return false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool IsInitialised()
{
	return g_initialised;
}

bool Init(LPDIRECT3DDEVICE9 device, HWND window)
{
	if (g_initialised) return true;
	if (!device || !window) return false;

	// The ImGui_Impl* sources are only compiled by the DLL, never by the
	// host's DllMain-time static initialisers.
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // arrow keys / tab / enter
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // the game owns the cursor

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 4.0f;
	style.FrameRounding = 3.0f;

	if (!::QueryPerformanceFrequency((LARGE_INTEGER*)&g_ticksPerSecond))
		g_ticksPerSecond = 0;
	if (!::QueryPerformanceCounter((LARGE_INTEGER*)&g_lastCounter))
		g_lastCounter = 0;

	if (!ImGui_ImplWin32_Init((void*)window))
	{
		HookLog("[ImGui] ImGui_ImplWin32_Init failed");
		ImGui::DestroyContext();
		return false;
	}

	if (!ImGui_ImplDX9_Init(device))
	{
		HookLog("[ImGui] ImGui_ImplDX9_Init failed");
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		return false;
	}

	// The platform backend normally sets this from its own HWND; do it here
	// for the viewport so multi-viewport / DPI code has a handle to consult.
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	viewport->PlatformHandle = viewport->PlatformHandleRaw = (void*)window;

	g_hwnd = window;
	g_initialised = true;

	HookLog("[ImGui] initialised (window %p, device %p)", (void*)window, (void*)device);
	return true;
}

void NewFrame(LPDIRECT3DDEVICE9 device)
{
	// Init() is called from OnEndScene (where both the device and the window
	// are known); this is only a guard so a stray call cannot crash.
	if (!g_initialised) return;
	if (!device) return;

	ImGuiIO& io = ImGui::GetIO();

	// --- refresh the main viewport ---------------------------------------
	// The game recreates the swap chain on reset; the backend's cached size
	// would then be stale, so re-derive both DisplaySize and the viewport's
	// framebuffer dimensions every frame.
	IDirect3DSwapChain9* swapChain = nullptr;
	D3DPRESENT_PARAMETERS params;
	if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain &&
		SUCCEEDED(swapChain->GetPresentParameters(&params)))
	{
		io.DisplaySize = ImVec2((float)params.BackBufferWidth, (float)params.BackBufferHeight);

		ImGuiViewport* viewport = ImGui::GetMainViewport();
		viewport->Pos = ImVec2(0.0f, 0.0f);
		viewport->Size = io.DisplaySize;
		viewport->FramebufferScale = ImVec2(1.0f, 1.0f);
		viewport->PlatformHandle = viewport->PlatformHandleRaw = (void*)params.hDeviceWindow;
	}
	else if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
	{
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
	}

	if (swapChain)
		swapChain->Release();

	// --- timing -----------------------------------------------------------
	INT64 counter = 0;
	if (::QueryPerformanceCounter((LARGE_INTEGER*)&counter) && g_ticksPerSecond > 0)
	{
		io.DeltaTime = (float)((double)(counter - g_lastCounter) / (double)g_ticksPerSecond);
		g_lastCounter = counter;
	}
	else
	{
		io.DeltaTime = 1.0f / 60.0f;
	}
	// A paused / backgrounded game can deliver huge deltas; clamp so ImGui's
	// animations do not jump.
	if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
	if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;

	// --- input ------------------------------------------------------------
	// The Win32 backend normally does this in ImGui_ImplWin32_NewFrame(); we
	// cannot run that (no per-frame tick and no OS-driven idle), so replicate
	// the parts that are safe to poll from here. Message-driven state (mouse
	// position, buttons, wheel, characters, key down/up) arrives through
	// ProcessMessage().
	UpdateKeyModifiers(io);
	UpdateMouseData(io);
	UpdateMouseCursor(io);

	ImGui::NewFrame();
}

void RenderDrawData()
{
	if (!g_initialised) return;
	ImGui::Render();
	if (ImGui::GetDrawData())
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void InvalidateDeviceObjects()
{
	if (!g_initialised) return;
	ImGui_ImplDX9_InvalidateDeviceObjects();
}

bool CreateDeviceObjects()
{
	if (!g_initialised) return false;
	return ImGui_ImplDX9_CreateDeviceObjects();
}

void Shutdown()
{
	if (!g_initialised) return;
	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	g_initialised = false;
	g_hwnd = nullptr;
}

} // namespace ImGuiBridge
