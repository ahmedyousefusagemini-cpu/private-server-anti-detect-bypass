// ============================================================================
// PacketOverlay - in-game packet viewer for Conquer.exe
// ----------------------------------------------------------------------------
// An ImGui window rendered inside the game's D3D9 EndScene. It lists every
// captured packet (sequence, time, direction, length, id, recovered message
// name and a hex preview) and shows a hex/ASCII dump of the selected one.
//
// Backend plumbing lives in imgui_bridge.cpp; the packet ring lives in
// packet_capture.cpp. This file is only the UI.
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

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cfloat>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include "packet_overlay.h"
#include "packet_capture.h"
#include "packet_names.h"
#include "imgui_bridge.h"
#include "log.h"

namespace PacketOverlay
{

using namespace PacketCapture;

namespace {

	// -----------------------------------------------------------------------
	// State
	// -----------------------------------------------------------------------
	bool  g_visible = true;
	bool  g_visibleInitialised = false;
	bool  g_autoScroll = true;                 // follow the newest packet
	bool  g_positionSet = false;               // window placed on the first frame
	ImVec2 g_initialPos = ImVec2(40.0f, 40.0f);

	HWND  g_renderWindow = nullptr;            // set by SetRenderWindow()
	uint32_t g_startTick = 0;

	uint32_t g_selectedSeq = 0;
	bool     g_hasSelection = false;

	// Table column widths. MESSAGE and BYTES are user-resizable; the last
	// column stretches to fill the window.
	float g_colSeq = 62.0f;
	float g_colTime = 76.0f;
	float g_colDir = 52.0f;
	float g_colLen = 56.0f;
	float g_colId = 68.0f;
	float g_colName = 172.0f;

	const float kDetailHeight = 168.0f;
	const float kPanelWidth = 1040.0f;
	const float kPanelHeight = 640.0f;
	const int   kBytesPerRow = 16;

	// -----------------------------------------------------------------------
	// Helpers
	// -----------------------------------------------------------------------
	void FormatClock(uint32_t tick, char* out, size_t outSize)
	{
		uint32_t elapsed = tick - g_startTick;
		uint32_t minutes = elapsed / 60000u;
		uint32_t seconds = (elapsed / 1000u) % 60u;
		uint32_t millis = elapsed % 1000u;
		_snprintf_s(out, outSize, _TRUNCATE, "%02u:%02u.%03u", minutes, seconds, millis);
	}

	ImVec4 DirectionColour(uint8_t direction)
	{
		return (direction == DirectionSend)
			? ImVec4(1.00f, 0.67f, 0.23f, 1.00f)     // SEND - amber
			: ImVec4(0.37f, 0.86f, 0.56f, 1.00f);    // RECV - green
	}

	// "08 00 23 04 4A 33 03 5B", truncated to maxBytes.
	void BuildHexPreview(const Entry& entry, char* out, size_t outSize, size_t maxBytes)
	{
		size_t bytes = (size_t)entry.storedBytes;
		if (bytes > maxBytes) bytes = maxBytes;

		size_t written = 0;
		for (size_t i = 0; i < bytes && written + 4 < outSize; ++i)
		{
			written += (size_t)_snprintf_s(out + written, outSize - written, _TRUNCATE,
				(i + 1 < bytes) ? "%02X " : "%02X", entry.data[i]);
		}
		if (written == 0) out[0] = '\0';
	}

	void FormatAscii(const uint8_t* data, int bytes, char* out, size_t outSize)
	{
		size_t written = 0;
		for (int i = 0; i < bytes && written + 2 < outSize; ++i)
		{
			uint8_t c = data[i];
			out[written++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
		}
		out[written] = '\0';
	}

	// -----------------------------------------------------------------------
	// Panels
	// -----------------------------------------------------------------------
	void DrawTitleBar(float windowWidth)
	{
		ImGui::TextUnformatted("Conquer Packet Monitor");
		ImGui::SameLine();
		ImGui::TextDisabled("| SEND %ld  RECV %ld  dropped %ld",
			TotalSend(), TotalRecv(), TotalDropped());

		if (IsPaused())
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f), "| PAUSED");
		}

		const char* controls = "[F8] hide  [F9] pause  [F10] clear";
		float controlsWidth = ImGui::CalcTextSize(controls).x;
		float x = windowWidth - controlsWidth - ImGui::GetStyle().WindowPadding.x;
		if (x > ImGui::GetCursorPosX())
		{
			ImGui::SameLine(x);
			ImGui::TextDisabled("%s", controls);
		}

		ImGui::Separator();
	}

	void DrawStatusBar(const Snapshot& snapshot)
	{
		ImGui::Separator();
		ImGui::TextDisabled("showing %ld of %ld retained  |  %s  |  click a row for the hex dump",
			snapshot.count, (long)snapshot.total,
			g_autoScroll ? "following newest" : "scroll paused");
	}

	void DrawDetailPane(const Snapshot& snapshot)
	{
		const Entry* entry = nullptr;
		if (g_hasSelection)
		{
			long index = IndexOfSeq(snapshot, g_selectedSeq);
			if (index >= 0) entry = &snapshot.ring[index];
		}

		ImGui::Separator();

		if (!entry)
		{
			ImGui::TextDisabled("Select a packet to inspect its bytes.");
			return;
		}

		char clock[32];
		FormatClock(entry->tick, clock, sizeof(clock));

		ImGui::TextColored(DirectionColour(entry->direction),
			"#%06u  %s  len=%u  id=0x%04X  %s  t=%s",
			entry->seq,
			entry->direction == DirectionSend ? "SEND" : "RECV",
			(unsigned)entry->length, (unsigned)entry->messageId,
			PacketNames::Lookup(entry->messageId), clock);

		if (entry->storedBytes < (int)entry->length)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%u of %u bytes stored)",
				(unsigned)entry->storedBytes, (unsigned)entry->length);
		}

		// A scrollable child keeps large packets readable.
		ImGui::BeginChild("##hexdump", ImVec2(0.0f, kDetailHeight), true,
			ImGuiWindowFlags_HorizontalScrollbar);

		int bytes = entry->storedBytes;
		for (int offset = 0; offset < bytes; offset += kBytesPerRow)
		{
			char line[256];
			size_t written = (size_t)_snprintf_s(line, _TRUNCATE, "%04X  ", offset);

			for (int i = 0; i < kBytesPerRow; ++i)
			{
				if (offset + i < bytes)
					written += (size_t)_snprintf_s(line + written, sizeof(line) - written, _TRUNCATE,
						"%02X ", entry->data[offset + i]);
				else
					written += (size_t)_snprintf_s(line + written, sizeof(line) - written, _TRUNCATE, "   ");
				if (i == 7 && written + 2 < sizeof(line))
				{
					line[written++] = ' ';
					line[written] = '\0';
				}
			}

			int asciiBytes = bytes - offset;
			if (asciiBytes > kBytesPerRow) asciiBytes = kBytesPerRow;
			char ascii[32];
			FormatAscii(entry->data + offset, asciiBytes, ascii, sizeof(ascii));

			_snprintf_s(line + written, sizeof(line) - written, _TRUNCATE, " |%s|", ascii);
			ImGui::TextUnformatted(line);
		}

		ImGui::EndChild();
	}

	void DrawPacketTable(const Snapshot& snapshot, float listHeight)
	{
		const ImGuiTableFlags tableFlags =
			ImGuiTableFlags_RowBg |
			ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_ScrollY |
			ImGuiTableFlags_Resizable |
			ImGuiTableFlags_SizingFixedFit;

		if (!ImGui::BeginTable("##packets", 7, tableFlags, ImVec2(0.0f, listHeight)))
			return;

		ImGui::TableSetupScrollFreeze(0, 1);   // keep the header visible
		ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed,   g_colSeq);
		ImGui::TableSetupColumn("TIME",    ImGuiTableColumnFlags_WidthFixed,   g_colTime);
		ImGui::TableSetupColumn("DIR",     ImGuiTableColumnFlags_WidthFixed,   g_colDir);
		ImGui::TableSetupColumn("LEN",     ImGuiTableColumnFlags_WidthFixed,   g_colLen);
		ImGui::TableSetupColumn("ID",      ImGuiTableColumnFlags_WidthFixed,   g_colId);
		ImGui::TableSetupColumn("MESSAGE", ImGuiTableColumnFlags_WidthFixed,   g_colName);
		ImGui::TableSetupColumn("BYTES",   ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		// Oldest-first walk so the newest entry sits at the bottom.
		long count = snapshot.count;
		for (long i = 0; i < count; ++i)
		{
			long index = (snapshot.head - count + i + kRingSize * 2) % kRingSize;
			const Entry& entry = snapshot.ring[index];

			ImGui::TableNextRow();

			const bool selected = g_hasSelection && entry.seq == g_selectedSeq;
			if (selected)
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(48, 66, 108, 255));

			ImGui::TableSetColumnIndex(0);
			char label[64];
			_snprintf_s(label, _TRUNCATE, "%06u", entry.seq);
			// AllowOverlap + SpanAllColumns makes the whole row clickable.
			if (ImGui::Selectable(label, selected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			{
				g_selectedSeq = entry.seq;
				g_hasSelection = true;
			}

			ImGui::TableSetColumnIndex(1);
			char clock[32];
			FormatClock(entry.tick, clock, sizeof(clock));
			ImGui::TextDisabled("%s", clock);

			ImGui::TableSetColumnIndex(2);
			ImGui::TextColored(DirectionColour(entry.direction),
				"%s", entry.direction == DirectionSend ? "SEND" : "RECV");

			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%u", (unsigned)entry.length);

			ImGui::TableSetColumnIndex(4);
			ImGui::TextColored(ImVec4(0.47f, 0.71f, 1.0f, 1.0f), "0x%04X", (unsigned)entry.messageId);

			ImGui::TableSetColumnIndex(5);
			ImGui::TextUnformatted(PacketNames::Lookup(entry.messageId));

			ImGui::TableSetColumnIndex(6);
			char preview[256];
			float charWidth = ImGui::CalcTextSize("00 ").x;
			size_t maxBytes = (charWidth > 0.0f)
				? (size_t)(ImGui::GetContentRegionAvail().x / charWidth) : 8;
			BuildHexPreview(entry, preview, sizeof(preview), maxBytes);
			ImGui::TextDisabled("%s", preview);
		}

		// Auto-follow the tail. Wheel input and the scroll position both only
		// mean anything once the table has been submitted at least once.
		ImGuiIO& io = ImGui::GetIO();
		if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f)
		{
			// Scrolling up detaches; scrolling down re-attaches only if the
			// user lands back at the bottom.
			if (io.MouseWheel > 0.0f) g_autoScroll = false;
			else if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) g_autoScroll = true;
		}

		const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
		if (g_autoScroll && atBottom)
			ImGui::SetScrollHereY(1.0f);

		ImGui::EndTable();
	}

	void DrawPanel()
	{
		if (!g_positionSet)
		{
			ImGui::SetNextWindowPos(g_initialPos, ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kPanelHeight), ImGuiCond_Always);
			g_positionSet = true;
		}

		const ImGuiWindowFlags windowFlags =
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoSavedSettings;

		ImGui::SetNextWindowSizeConstraints(ImVec2(600.0f, 320.0f), ImVec2(FLT_MAX, FLT_MAX));

		// The ring lock is taken once for the whole panel and released on every
		// path out of Begin(), including the collapsed case where Begin()
		// returns false and no content is drawn.
		Snapshot snapshot = BeginRead();

		if (ImGui::Begin("Conquer Packet Monitor", &g_visible, windowFlags))
		{
			float windowWidth = ImGui::GetWindowWidth();
			DrawTitleBar(windowWidth);

			// Reserve room for the status bar and the detail pane, then let the
			// table take whatever remains.
			float reserved = ImGui::GetTextLineHeightWithSpacing()          // status bar
				+ kDetailHeight
				+ ImGui::GetFrameHeightWithSpacing()                       // hex child border/margins
				+ ImGui::GetStyle().ItemSpacing.y * 4.0f
				+ ImGui::GetStyle().WindowPadding.y * 2.0f;

			float listHeight = ImGui::GetContentRegionAvail().y - reserved;
			if (listHeight < 80.0f) listHeight = 80.0f;

			DrawPacketTable(snapshot, listHeight);
			DrawDetailPane(snapshot);
			DrawStatusBar(snapshot);
		}

		ImGui::End();

		EndRead();
	}

	// -----------------------------------------------------------------------
	// Hotkeys
	// -----------------------------------------------------------------------
	// Polled with GetAsyncKeyState instead of handled in the window procedure.
	// Conquer drives its keyboard through DirectInput, so WM_KEYDOWN is not
	// reliably delivered to the window we subclassed - a message-based F8
	// handler simply never fires. Reading the physical key state works no
	// matter how the game consumes input.
	bool IsGameForeground()
	{
		HWND foreground = GetForegroundWindow();
		if (!foreground) return false;

		DWORD processId = 0;
		GetWindowThreadProcessId(foreground, &processId);
		return processId == GetCurrentProcessId();
	}

	// True only on the frame the key transitions from up to down, so holding
	// the key does not repeat the action.
	bool KeyPressedEdge(int virtualKey, bool& wasDown)
	{
		bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
		bool pressed = down && !wasDown;
		wasDown = down;
		return pressed;
	}

	void PollHotkeys()
	{
		static bool f8Down = false;
		static bool f9Down = false;
		static bool f10Down = false;
		static bool escapeDown = false;
		static bool middleDown = false;

		// Always sample, so the edge state stays correct even while another
		// window has focus and we deliberately ignore the result.
		bool f8 = KeyPressedEdge(VK_F8, f8Down);
		bool f9 = KeyPressedEdge(VK_F9, f9Down);
		bool f10 = KeyPressedEdge(VK_F10, f10Down);
		bool escape = KeyPressedEdge(VK_ESCAPE, escapeDown);
		bool middle = KeyPressedEdge(VK_MBUTTON, middleDown);

		// Only act while the game owns the foreground, so the overlay does not
		// toggle while the user is typing in some other application.
		if (!IsGameForeground()) return;

		if (f8)  g_visible = !g_visible;
		if (f9)  SetPaused(!IsPaused());
		if (f10) { Clear(); g_hasSelection = false; g_autoScroll = true; }
		if (escape && g_visible) g_visible = false;
		if (middle && g_visible) g_visible = false;

		// Auto-scroll (detach on scroll-up, re-attach at the bottom) is handled
		// inside DrawPacketTable, i.e. after NewFrame() - the ImGui:: helpers
		// are not valid before a frame has begun.
	}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SetRenderWindow(HWND windowHandle)
{
	g_renderWindow = windowHandle;
}

bool IsVisible()
{
	return g_visible;
}

void SetVisible(bool visible)
{
	g_visible = visible;
}

void OnLostDevice()
{
	ImGuiBridge::InvalidateDeviceObjects();
}

void OnResetDevice()
{
	ImGuiBridge::CreateDeviceObjects();
}

void OnEndScene(LPDIRECT3DDEVICE9 device)
{
	if (!device) return;

	// First frame: honour the "overlay" setting from packet_log.ini.
	if (!g_visibleInitialised)
	{
		g_visibleInitialised = true;
		g_visible = PacketCapture::OverlayEnabledAtStartup();
	}

	if (g_startTick == 0) g_startTick = GetTickCount();

	// Bring the bridge up once the real device and window are known.
	if (!ImGuiBridge::IsInitialised())
	{
		if (!ImGuiBridge::Init(device, g_renderWindow))
			return;
	}

	// Poll the hotkeys BEFORE the visibility check so F8 can still bring the
	// window back once it has been hidden.
	PollHotkeys();

	// ImGui needs NewFrame/Render as a pair every frame, visible or not.
	ImGuiBridge::NewFrame(device);

	if (g_visible)
		DrawPanel();

	ImGuiBridge::RenderDrawData();
}

bool OnWindowMessage(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
	return ImGuiBridge::ProcessMessage(windowHandle, message, wParam, lParam);
}

} // namespace PacketOverlay
