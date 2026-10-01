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
#include "packet_overlay.h"
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

	// Mirrors the useful half of ImGui_ImplWin32_UpdateMouseData().
	//
	// The backend only falls back to GetCursorPos() when its own
	// MouseTrackedArea is 0, i.e. while WM_MOUSELEAVE has told it the cursor
	// left. We cannot see that private state, so the rule we follow here is
	// narrower: feed the cursor position ONLY when the client area is not the
	// window that last reported a WM_MOUSEMOVE - which is exactly the case
	// where the backend has already stopped receiving WM_MOUSEMOVE and the
	// position would otherwise go stale (alt-tab back into the game, or the
	// cursor parked on the non-client frame).
	//
	// The reason this used to break clicking: it unconditionally pushed
	// AddMousePosEvent() on top of the position the backend had queued from
	// WM_MOUSEMOVE. ImGui resolves the position by taking the LAST event in
	// the frame's queue, so our polling always won. When the two disagreed by
	// even a pixel (GetCursorPos is sampled at a different instant than the
	// message's lParam), ImGui computed hover against our stale sample while
	// the click's ButtonDown had been queued against the message position -
	// so the press landed on "no window" and nothing was ever hovered,
	// activated or dragged.
	void UpdateMouseData(ImGuiIO& io)
	{
		if (!g_hwnd) return;

		HWND foreground = ::GetForegroundWindow();
		if (!foreground) return;

		// The game is a multi-window app (root frame + a child render window),
		// so "focused" must accept any window belonging to our process, not
		// just the one we render into.
		DWORD foregroundPid = 0;
		::GetWindowThreadProcessId(foreground, &foregroundPid);
		if (foregroundPid != ::GetCurrentProcessId()) return;

		if (io.WantSetMousePos)
		{
			POINT pos = { (int)io.MousePos.x, (int)io.MousePos.y };
			if (::ClientToScreen(g_hwnd, &pos))
				::SetCursorPos(pos.x, pos.y);
			return;
		}

		// Do not touch the position while a button is held: during a drag the
		// message-driven stream is authoritative and must not be overwritten,
		// otherwise ImGui's MouseDelta jumps and the drag misfires.
		if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2])
			return;

		// Only poll when the cursor is inside our client area. Outside it, the
		// backend's own WM_MOUSELEAVE handling owns the "no position" state and
		// forcing a position back in would make the panel hover while the
		// cursor is somewhere else entirely.
		if (::GetCapture() != nullptr) return;

		POINT pos;
		if (!::GetCursorPos(&pos)) return;
		::ScreenToClient(g_hwnd, &pos);

		// Skip if the backend already has this position queued from a
		// WM_MOUSEMOVE this frame - re-adding it is harmless but pointless.
		RECT client;
		if (!::GetClientRect(g_hwnd, &client)) return;
		if (pos.x < client.left || pos.x >= client.right ||
			pos.y < client.top  || pos.y >= client.bottom)
			return;

		if (pos.x == (LONG)io.MousePos.x && pos.y == (LONG)io.MousePos.y)
			return;

		io.AddMousePosEvent((float)pos.x, (float)pos.y);
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

	// While the overlay is hidden it must not touch ImGui's input state at all.
	// Feeding it events for an invisible window would leave MouseDown bits set
	// with nothing drawing, and a later Show would start with a stale capture.
	if (!ImGui::GetCurrentContext()) return false;
	const bool overlayVisible = PacketOverlay::IsVisible();

	// The subclass is installed on TWO windows (the root frame for keyboard,
	// the render child for mouse), and ImGui's Win32 backend keeps exactly one
	// MouseHwnd / MouseTrackedArea / MouseButtonsDown triple. Feeding it
	// messages from both windows interleaves TrackMouseEvent() registration
	// against two different HWNDs, which desynchronises the tracked area and
	// makes hover - and therefore every click - unreliable.
	//
	// Mouse input is therefore accepted ONLY from the window the backend was
	// initialised with; the other window still gets keyboard/character routing
	// below so a focused root frame can type into an ImGui input box.
	const bool fromRenderWindow = (g_hwnd != nullptr && window == g_hwnd);

	if (!overlayVisible || !fromRenderWindow)
	{
		// Keyboard and focus still have to reach the backend from whichever
		// window owns them: the root frame is where keystrokes land, so typing
		// into the filter boxes would be dead if we filtered these by window.
		// Only mouse messages are window-sensitive (see above).
		const bool isKeyboard = (message == WM_KEYDOWN || message == WM_KEYUP ||
			message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
			message == WM_CHAR || message == WM_SETFOCUS ||
			message == WM_KILLFOCUS || message == WM_ACTIVATEAPP);

		// Keep the backend's button state honest even while we are not
		// consuming input, so a capture taken before the panel was hidden (or
		// under the other window) is released instead of leaking into the game.
		const bool isButtonUp = (message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
			message == WM_MBUTTONUP || message == WM_XBUTTONUP);

		if (isKeyboard || isButtonUp)
		{
			// Only feed ImGui when the panel is actually up and interactive;
			// otherwise just clear any stuck Win32 capture.
			if (overlayVisible)
				ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);

			if (isButtonUp && ::GetCapture() == window)
				::ReleaseCapture();
		}

		// Keyboard the overlay wants must not reach the game (typing "0x" in a
		// filter box should not fire the game's chat / hotkey bindings).
		if (isKeyboard && overlayVisible)
			return ImGui::GetIO().WantCaptureKeyboard;

		return false;
	}

	// --- mouse move ------------------------------------------------------
	// WM_MOUSEMOVE must ALWAYS reach the backend, even when ImGui wants the
	// mouse (WantCaptureMouse == true). The backend uses these messages to arm
	// TrackMouseEvent() and to emit WM_MOUSELEAVE; swallowing them while hovered
	// means the backend never learns the cursor left, MouseTrackedArea sticks
	// and the window stays "hovered" forever - which then makes every later
	// click land on the wrong place (or on nothing). The message is still
	// passed on to the game (return false) so its own camera look keeps working.
	if (message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE ||
		message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE ||
		message == WM_SETCURSOR)
	{
		ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
		return false;
	}

	ImGuiIO& io = ImGui::GetIO();

	// A button-up that belongs to an in-progress drag (window move, column
	// resize, slider) can arrive after the cursor has already left the panel.
	// Consume it unconditionally so the game does not also act on it, and make
	// sure we do not keep a Win32 capture the backend has already dropped.
	const bool dragOwnsMouse = io.WantCaptureMouse || io.MouseDown[0] || io.MouseDown[1];
	if (dragOwnsMouse &&
		(message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
		 message == WM_MBUTTONUP || message == WM_XBUTTONUP))
	{
		ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
		if (::GetCapture() == window)
			::ReleaseCapture();
		return true;
	}

	// Let the official handler queue the event (buttons, wheel, keys,
	// characters, IME). It returns 1 for events it understood.
	ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);

	switch (message)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
		// io.WantCaptureMouse is the value NewFrame() computed at the END of the
		// previous frame, i.e. it reflects whether the cursor was over the panel
		// then. That is the right question to ask for "should the game see this
		// click" - the panel swallows clicks that land on it and lets everything
		// else through to the game.
		return io.WantCaptureMouse;

	case WM_LBUTTONUP: case WM_RBUTTONUP:
	case WM_MBUTTONUP: case WM_XBUTTONUP:
		// A release is consumed only when the overlay owns the click sequence.
		// If the press was passed to the game (WantCaptureMouse was false) the
		// release must go to the game too, otherwise the game ends up with a
		// button stuck down.
		return io.WantCaptureMouse;

	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		// The wheel is only the overlay's while the cursor is over it; outside
		// the panel the game keeps its zoom.
		return io.WantCaptureMouse;

	case WM_KEYDOWN: case WM_KEYUP:
	case WM_SYSKEYDOWN: case WM_SYSKEYUP:
	case WM_CHAR:
		// The overlay's own hotkeys (Insert/F8/F9/F10/Esc) are polled with
		// GetAsyncKeyState and are deliberately NOT handled here - the client
		// drives its keyboard through DirectInput, so these messages are not
		// reliably delivered, and handling them twice would toggle twice.
		return io.WantCaptureKeyboard;

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
	//
	// Coordinate-space contract: ImGui hit-tests with io.MousePos expressed in
	// the SAME space as io.DisplaySize. We feed MousePos in client coordinates
	// (ScreenToClient), so DisplaySize must be the CLIENT size - NOT the
	// backbuffer size. The two differ whenever the game is windowed, or run at
	// a backbuffer resolution the client area does not match (very common: a
	// 1024x768 backbuffer in a larger or smaller window). Getting this wrong
	// shifts every widget's hit rectangle by the difference, which presents
	// exactly as "I can see the tabs but clicking them does nothing" and "I
	// can't grab the title bar".
	//
	// Fall back to the backbuffer size only if the client rect is unavailable.
	IDirect3DSwapChain9* swapChain = nullptr;
	D3DPRESENT_PARAMETERS params;
	bool havePresent = false;
	if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain &&
		SUCCEEDED(swapChain->GetPresentParameters(&params)))
	{
		havePresent = true;
	}

	HWND presentWindow = havePresent ? params.hDeviceWindow : g_hwnd;

	float clientWidth = 0.0f;
	float clientHeight = 0.0f;
	if (presentWindow)
	{
		RECT client = { 0, 0, 0, 0 };
		if (::GetClientRect(presentWindow, &client))
		{
			clientWidth = (float)(client.right - client.left);
			clientHeight = (float)(client.bottom - client.top);
		}
	}

	if (clientWidth > 0.0f && clientHeight > 0.0f)
	{
		io.DisplaySize = ImVec2(clientWidth, clientHeight);
	}
	else if (havePresent)
	{
		io.DisplaySize = ImVec2((float)params.BackBufferWidth, (float)params.BackBufferHeight);
	}
	else if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
	{
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
	}

	{
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		viewport->Pos = ImVec2(0.0f, 0.0f);
		viewport->Size = io.DisplaySize;
		viewport->FramebufferScale = ImVec2(1.0f, 1.0f);
		viewport->PlatformHandle = viewport->PlatformHandleRaw = (void*)presentWindow;
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
