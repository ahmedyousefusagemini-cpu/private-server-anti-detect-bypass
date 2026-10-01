// ============================================================================
// PacketOverlay - in-game bot panel for Conquer.exe
// ----------------------------------------------------------------------------
// An ImGui window rendered inside the game's D3D9 EndScene. The window is a
// tabbed "bot panel" shell (Player / Map / Packets / Misc / Plugins); the
// packet logger - the live capture table plus the hex/ASCII dump of the
// selected packet - lives in the "Packets" tab.
//
// Backend plumbing lives in imgui_bridge.cpp; the packet ring lives in
// packet_capture.cpp. This file is only the UI.
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
	bool  g_autoScroll = true;                     // follow the newest packet
	bool  g_positionSet = false;                   // window placed on the first frame
	ImVec2 g_initialPos = ImVec2(240.0f, 110.0f);

	HWND  g_renderWindow = nullptr;                // set by SetRenderWindow()
	uint32_t g_startTick = 0;

	uint32_t g_selectedSeq = 0;
	bool     g_hasSelection = false;

	// Which tab's contents are currently showing (0=Player 1=Map 2=Packets
	// 3=Misc 4=Plugins). Written by BeginPanelTab(); reserved for logic that
	// wants to know what the user is looking at (e.g. skipping the packet
	// table read when the Packets tab is not visible).
	int g_activeTab = 2;

	// One-shot "force this tab open" request. ImGuiTabItemFlags_SetSelected is
	// sticky: passing it every frame on a fixed tab would fight the user's
	// clicks (the tab bar queues focus back to it each frame). We therefore
	// only pass it once, for the tab we want to jump to, then clear it.
	int g_requestedTab = 2;

	// Template header widgets.
	bool g_saveSettingsPressed = false;            // latched, read by the app layer
	float g_saveFlashUntil = 0.0f;                 // seconds; shows "Saved" briefly

	// Map tab shell state (presentational only - no backend yet).
	bool  g_mapEntities = true;
	bool  g_mapFollow = false;
	int   g_mapDestination = 0;
	bool  g_speedhack = true;
	bool  g_avoidMobs = true;

	// Misc tab shell state.
	bool  g_miscShowFps = true;
	bool  g_miscNoLimit = false;

	// Table column widths. MESSAGE and BYTES are user-resizable; the last
	// column stretches to fill the window.
	float g_colSeq = 62.0f;
	float g_colTime = 76.0f;
	float g_colDir = 52.0f;
	float g_colLen = 56.0f;
	float g_colId = 68.0f;
	float g_colName = 172.0f;

	const float kDetailHeight = 150.0f;
	const float kPanelWidth = 1080.0f;
	const float kPanelHeight = 660.0f;
	const int   kBytesPerRow = 16;

	const char* const kWindowTitle = "Manager";

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

	// Small dimmed caption used for the "Autosaves shortly after changes" note
	// and for the placeholder text in the not-yet-implemented tabs.
	void Caption(const char* text)
	{
		ImGui::TextDisabled("%s", text);
	}

	// A titled section that the template repeats all over the panel
	// ("Overview", "Entities", "Travel", ...). Returns true while open.
	bool BeginSection(const char* label)
	{
		ImGui::Spacing();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
		return ImGui::CollapsingHeader(label, flags);
	}

	void SectionSeparator()
	{
		ImGui::Spacing();
		ImGui::Separator();
	}

	// -----------------------------------------------------------------------
	// Template header (title-adjacent rows, above the tab bar)
	// -----------------------------------------------------------------------
	void DrawTemplateHeader()
	{
		Caption("PRESS [INSERT] to toggle overlay.");
		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

		ImGui::Spacing();

		if (ImGui::Button("Save Settings", ImVec2(120.0f, 0.0f)))
		{
			g_saveSettingsPressed = true;
			g_saveFlashUntil = (float)ImGui::GetTime() + 2.0f;
		}
		ImGui::SameLine();
		if (ImGui::GetTime() < g_saveFlashUntil)
			ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "Settings saved.");
		else
			Caption("Autosaves shortly after changes");

		ImGui::Spacing();
	}

	// -----------------------------------------------------------------------
	// Tab: Player  (shell)
	// -----------------------------------------------------------------------
	void DrawPlayerTab()
	{
		if (BeginSection("Status"))
		{
			Caption("No player bound. Log in, then reopen this panel.");
			ImGui::Spacing();
			ImGui::TextDisabled("HP   -- / --");
			ImGui::TextDisabled("MP   -- / --");
			ImGui::TextDisabled("Level  --");
		}

		SectionSeparator();

		if (BeginSection("Inventory"))
		{
			Caption("Item grid is not wired up yet.");
		}

		SectionSeparator();

		if (BeginSection("Skills"))
		{
			Caption("Skill list will mirror the client's learned spells.");
		}
	}

	// -----------------------------------------------------------------------
	// Tab: Map  (the template's default look)
	// -----------------------------------------------------------------------
	void DrawMapTab()
	{
		if (BeginSection("Overview"))
		{
			ImGui::Text("Map: Twin City  (ID: 1882)");
			ImGui::Spacing();
			ImGui::Text("Hero: (432, 376)");
			ImGui::SameLine();
			ImGui::TextDisabled("[Copy]");
			ImGui::SameLine();
			ImGui::TextDisabled("[12 gateways]");
			ImGui::Spacing();
			ImGui::Checkbox("Entities", &g_mapEntities);
			ImGui::SameLine();
			ImGui::Checkbox("Follow", &g_mapFollow);
			ImGui::SameLine();
			ImGui::Button("Reset View");
			ImGui::SameLine();
			ImGui::Button("Dump Map");
		}

		SectionSeparator();

		if (BeginSection("Travel"))
		{
			Caption("Idle");
			ImGui::Spacing();
			ImGui::Text("Current Map: Twin City (1882)");
			ImGui::Text("Hero Pos: (432, 376)");
			ImGui::Spacing();
			ImGui::Checkbox("Speedhack If No Players Nearby", &g_speedhack);
			Caption("Send raw jump packets for faster travel. Falls back to normal jumps when players are nearby.");
			ImGui::Checkbox("Avoid Mobs While Traveling", &g_avoidMobs);
		}

		SectionSeparator();

		if (BeginSection("Twin City"))
		{
			static const char* destinations[] = { "Twin City", "Desert City", "Ape Mountain", "Phoenix Castle" };
			ImGui::SetNextItemWidth(220.0f);
			ImGui::Combo("Destination", &g_mapDestination, destinations, IM_COUNTOF(destinations));
			ImGui::Button("Travel");
			ImGui::SameLine();
			Caption("Travel: (430, 380)");
			ImGui::Spacing();
			Caption("Already on Twin City.");
		}

		SectionSeparator();

		if (BeginSection("Minimap"))
		{
			// Placeholder canvas so the template's minimap frame is visible.
			ImVec2 available = ImGui::GetContentRegionAvail();
			float height = available.y;
			if (height < 140.0f) height = 140.0f;
			if (height > 260.0f) height = 260.0f;

			ImVec2 canvasSize(available.x, height);
			ImVec2 origin = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled(
				origin,
				ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y),
				IM_COL32(22, 26, 24, 255));
			ImGui::GetWindowDrawList()->AddRect(
				origin,
				ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y),
				IM_COL32(70, 78, 74, 255));

			// A few dots standing in for entities.
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImU32 dotColour = IM_COL32(150, 220, 160, 255);
			const float dots[][2] = {
				{ 0.30f, 0.42f }, { 0.33f, 0.55f }, { 0.52f, 0.30f },
				{ 0.61f, 0.62f }, { 0.72f, 0.48f }, { 0.44f, 0.72f },
			};
			for (int i = 0; i < IM_COUNTOF(dots); ++i)
			{
				ImVec2 p(origin.x + canvasSize.x * dots[i][0],
					origin.y + canvasSize.y * dots[i][1]);
				draw->AddCircleFilled(p, 2.0f, dotColour);
			}
			ImGui::Dummy(canvasSize);
		}
	}

	// -----------------------------------------------------------------------
	// Tab: Packets  (the actual logger)
	// -----------------------------------------------------------------------
	void DrawPacketToolbar(float windowWidth)
	{
		// Capture state + totals on the left, capture controls on the right.
		if (IsPaused())
			ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f), "PAUSED");
		else
			ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "CAPTURING");

		ImGui::SameLine();
		ImGui::TextDisabled("| SEND %ld  RECV %ld  dropped %ld",
			TotalSend(), TotalRecv(), TotalDropped());

		char controls[96];
		_snprintf_s(controls, _TRUNCATE, "[F9] %s  [F10] clear",
			IsPaused() ? "resume" : "pause");
		float controlsWidth = ImGui::CalcTextSize(controls).x;
		float x = windowWidth - controlsWidth - ImGui::GetStyle().WindowPadding.x;
		if (x > ImGui::GetCursorPosX())
		{
			ImGui::SameLine(x);
			ImGui::TextDisabled("%s", controls);
		}

		ImGui::Separator();
	}

	void DrawPacketStatusBar(const Snapshot& snapshot)
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

		ImGui::TextDisabled("protobuf body follows the 4-byte header (len, id):");

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

	void DrawPacketsTab(const Snapshot& snapshot)
	{
		float windowWidth = ImGui::GetWindowWidth();
		DrawPacketToolbar(windowWidth);

		// Reserve room for the status bar and the detail pane, then let the
		// table take whatever remains.
		float reserved = ImGui::GetTextLineHeightWithSpacing()          // status bar
			+ kDetailHeight
			+ ImGui::GetTextLineHeightWithSpacing()                    // "protobuf body" caption
			+ ImGui::GetFrameHeightWithSpacing()                       // hex child border/margins
			+ ImGui::GetStyle().ItemSpacing.y * 6.0f
			+ ImGui::GetStyle().FramePadding.y * 2.0f;

		float listHeight = ImGui::GetContentRegionAvail().y - reserved;
		if (listHeight < 80.0f) listHeight = 80.0f;

		DrawPacketTable(snapshot, listHeight);
		DrawDetailPane(snapshot);
		DrawPacketStatusBar(snapshot);
	}

	// -----------------------------------------------------------------------
	// Tab: Misc  (shell)
	// -----------------------------------------------------------------------
	void DrawMiscTab()
	{
		if (BeginSection("Interface"))
		{
			ImGui::Checkbox("Show FPS Counter", &g_miscShowFps);
			ImGui::Checkbox("Unlock Frame Rate", &g_miscNoLimit);
			Caption("Frame-rate changes apply after a restart.");
		}

		SectionSeparator();

		if (BeginSection("Automation"))
		{
			Caption("Triggers, schedules and anti-idle land here.");
		}

		SectionSeparator();

		if (BeginSection("Diagnostics"))
		{
			ImGui::Text("ImGui %s", IMGUI_VERSION);
			ImGui::TextDisabled("Log: %s", PacketCapture::LogFilePath());
		}
	}

	// -----------------------------------------------------------------------
	// Tab: Plugins  (shell)
	// -----------------------------------------------------------------------
	void DrawPluginsTab()
	{
		if (BeginSection("Loaded Plugins"))
		{
			Caption("No plugins loaded.");
			ImGui::Spacing();
			Caption("Drop a .dll next to the client and it will be listed here.");
		}

		SectionSeparator();

		if (BeginSection("Marketplace"))
		{
			Caption("Not connected.");
		}
	}

	// -----------------------------------------------------------------------
	// Panel
	// -----------------------------------------------------------------------
	// Opens one tab. The SetSelected flag is passed only while a jump to this
	// tab is pending, so the user's clicks are never fought over. When the tab
	// is the visible one we record it as the current selection.
	bool BeginPanelTab(const char* label, int index)
	{
		ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
		if (g_requestedTab == index)
			flags |= ImGuiTabItemFlags_SetSelected;

		bool open = ImGui::BeginTabItem(label, nullptr, flags);
		if (open)
		{
			g_activeTab = index;
			if (g_requestedTab == index)
				g_requestedTab = -1;   // request satisfied
		}
		return open;
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

		ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 420.0f), ImVec2(FLT_MAX, FLT_MAX));

		// The ring lock is taken once for the whole panel and released on every
		// path out of Begin(), including the collapsed case where Begin()
		// returns false and no content is drawn.
		Snapshot snapshot = BeginRead();

		if (ImGui::Begin(kWindowTitle, &g_visible, windowFlags))
		{
			DrawTemplateHeader();

			if (ImGui::BeginTabBar("##bottomtabs", ImGuiTabBarFlags_None))
			{
				if (BeginPanelTab("Player", 0))
				{
					DrawPlayerTab();
					ImGui::EndTabItem();
				}

				if (BeginPanelTab("Map", 1))
				{
					DrawMapTab();
					ImGui::EndTabItem();
				}

				if (BeginPanelTab("Packets", 2))
				{
					DrawPacketsTab(snapshot);
					ImGui::EndTabItem();
				}

				if (BeginPanelTab("Misc", 3))
				{
					DrawMiscTab();
					ImGui::EndTabItem();
				}

				if (BeginPanelTab("Plugins", 4))
				{
					DrawPluginsTab();
					ImGui::EndTabItem();
				}

				ImGui::EndTabBar();
			}
		}

		ImGui::End();

		EndRead();
	}

	// -----------------------------------------------------------------------
	// Hotkeys
	// -----------------------------------------------------------------------
	// Polled with GetAsyncKeyState instead of handled in the window procedure.
	// Conquer drives its keyboard through DirectInput, so WM_KEYDOWN is not
	// reliably delivered to the window we subclassed - a message-based handler
	// simply never fires. Reading the physical key state works no matter how
	// the game consumes input.
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
		static bool insertDown = false;
		static bool f8Down = false;
		static bool f9Down = false;
		static bool f10Down = false;
		static bool escapeDown = false;
		static bool middleDown = false;

		// Always sample, so the edge state stays correct even while another
		// window has focus and we deliberately ignore the result.
		bool insert = KeyPressedEdge(VK_INSERT, insertDown);
		bool f8 = KeyPressedEdge(VK_F8, f8Down);
		bool f9 = KeyPressedEdge(VK_F9, f9Down);
		bool f10 = KeyPressedEdge(VK_F10, f10Down);
		bool escape = KeyPressedEdge(VK_ESCAPE, escapeDown);
		bool middle = KeyPressedEdge(VK_MBUTTON, middleDown);

		// Only act while the game owns the foreground, so the overlay does not
		// toggle while the user is typing in some other application.
		if (!IsGameForeground()) return;

		if (insert || f8) g_visible = !g_visible;
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

bool ConsumeSaveSettingsRequest()
{
	bool pressed = g_saveSettingsPressed;
	g_saveSettingsPressed = false;
	return pressed;
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

	// Poll the hotkeys BEFORE the visibility check so INSERT can still bring
	// the window back once it has been hidden.
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
