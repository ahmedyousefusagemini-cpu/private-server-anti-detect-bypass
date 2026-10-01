// ============================================================================
// PacketOverlay - in-game bot panel for Conquer.exe
// ----------------------------------------------------------------------------
// An ImGui window rendered inside the game's D3D9 EndScene. The window is a
// tabbed "bot panel" shell (Player / Map / Packets / Misc / Plugins); the
// packet logger - the live capture table plus the hex/ASCII dump of the
// selected packet - lives in the "Packets" tab, along with a packet builder
// that can re-send a captured message (see packet_send.h).
//
// Backend plumbing lives in imgui_bridge.cpp; the packet ring lives in
// packet_capture.cpp; the send path lives in packet_send.cpp. This file is
// only the UI.
//
// Controls
//   Insert     show / hide the window  (primary toggle)
//   F8         show / hide the window  (alias)
//   F9         pause / resume capture
//   F10        clear the list
//   Esc        hide the window
//   wheel      scroll the packet list / hex dump
//   click row  select a packet (hex dump below the table)
//   drag       move the window (title bar, or the "drag here to move" strip
//              in the header)
//
// All hotkeys are polled with GetAsyncKeyState while the game process owns the
// foreground, and only on the up->down edge so holding a key does not repeat.
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cfloat>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include "packet_overlay.h"
#include "packet_capture.h"
#include "packet_names.h"
#include "packet_send.h"
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

	// The window is placed exactly once, then owned by ImGui. Using
	// ImGuiCond_Always here (as an earlier revision did) re-pinned the window
	// every frame: ImGui's move logic writes the drag position with
	// SetWindowPos(), and the next frame's unconditional SetNextWindowPos would
	// snap it straight back - so dragging appeared to do nothing at all.
	//
	// A first-run default (pos + size) is what ImGuiCond_FirstUseEver is for,
	// and it also lets the user's own placement survive a restart, because
	// ImGui persists it to imgui.ini. We additionally seed from overlay.ini.
	bool  g_windowPlacementPending = true;         // apply the default once
	ImVec2 g_initialPos = ImVec2(240.0f, 110.0f);
	ImVec2 g_initialSize = ImVec2(1080.0f, 660.0f);

	// Offset between the cursor and the window origin, captured when the drag
	// handle is first pressed, so the window does not jump on the first move.
	ImVec2 g_dragOffset = ImVec2(0.0f, 0.0f);
	bool   g_dragOffsetValid = false;

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

	// ---- filter state -----------------------------------------------------
	// A row is shown when it passes every active clause. Clauses that are
	// left at their neutral value are not applied at all, so an untouched
	// filter bar shows the full ring.
	char  g_filterText[128] = "";        // free text: id (hex or dec), name, class, meaning
	char  g_filterId[32] = "";           // id only: "0x0800", "2048", "0x07*" (prefix ok)
	char  g_filterClass[64] = "";        // substring of the CMsg class name
	int   g_filterDirection = 0;         // 0=any 1=SEND 2=RECV
	bool  g_filterAnnotatedOnly = false; // hide ids with no name/class recovered
	float g_filterFromSeconds = 0.0f;    // include packets at/after this age (0 = all)
	float g_filterToSeconds = 0.0f;      // include packets at/before this age (0 = all)
	bool  g_filterTextCase = false;      // case sensitive free text
	bool  g_filterEnabled = false;       // master switch (the "Filter" checkbox)

	// Distinct class names seen in the current capture, rebuilt each frame
	// from the ring so the combo only lists things actually present. Capped
	// because a full ring can hold hundreds of distinct ids.
	const int kMaxClassChoices = 64;
	char  g_classChoices[kMaxClassChoices][64] = {};
	int   g_classChoiceCount = 0;

	// Copy feedback: shows a transient "Copied N rows" note.
	float g_copyFlashUntil = 0.0f;
	char  g_copyFlashText[64] = "";

	// ---- packet builder state --------------------------------------------
	// The builder replays a message the client itself sent. The body it
	// edits is seeded from that capture, so the fields below start out as
	// the real values the game used. See packet_send.h for why a live
	// capture (and not a hand-built buffer) is required.
	//
	//   char idText[]     the id being built, as typed ("0x0833")
	//   g_buildArmedId    the id whose template is currently loaded
	//   g_buildBody[]     editable body bytes (post 4-byte header)
	//   g_buildBodyBytes  how many of them are live
	bool   g_buildHasTemplate = false;             // a capture is loaded
	bool   g_buildPanelOpen = true;                // the builder block is expanded
	uint16_t g_buildArmedId = 0;
	char   g_buildIdText[16] = "0x0833";
	uint8_t g_buildBody[PacketSend::kMaxBodyBytes] = {};
	int    g_buildBodyBytes = 0;
	char   g_buildRawText[PacketSend::kMaxBodyBytes * 3 + 8] = "";
	bool   g_buildRawDirty = true;                 // raw text is behind the body
	uint16_t g_buildMode = 19;                     // field 1, for the hint only
	int    g_buildRepeatCount = 1;                 // how many sends per click
	float  g_buildMinIntervalMs = 0.0f;            // pacing between repeats
	float  g_buildNextSendTime = 0.0f;             // internal pacer
	int    g_buildPendingSends = 0;

	// Result feedback for the last send attempt.
	float  g_buildFlashUntil = 0.0f;
	char   g_buildFlashText[96] = "";
	bool   g_buildFlashError = false;

	// Jump speed: the number of milliseconds of lead to add to the packet's
	// timestamp on each successive jump. The reference implementations push
	// the client's own timestamp forward so the server accepts a faster
	// cadence; the field that carries it differs per client build, so this
	// is applied to whichever field the template exposes as a timestamp.
	int    g_jumpSpeedMs = 5000;
	float  g_jumpRepeatMs = 120.0f;                // spacing between auto-jumps
	float  g_jumpNextTime = 0.0f;
	bool   g_jumpAutoFire = false;

	// Autosave: the "Autosaves shortly after changes" note next to the Save
	// Settings button is not decorative - the panel writes overlay.ini once
	// the state has been stable for a moment, so a crash or forced exit does
	// not lose the filter setup.
	bool  g_settingsDirty = false;
	float g_settingsDirtySince = 0.0f;
	const float kAutosaveDelay = 1.5f;         // seconds of quiet before writing

	const float kDetailHeight = 150.0f;
	const float kBuilderHeight = 260.0f;       // decoded-fields / raw-hex side by side
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
		if (outSize == 0) return;
		out[0] = '\0';
		if (maxBytes == 0) return;

		size_t bytes = (size_t)entry.storedBytes;
		if (bytes > maxBytes) bytes = maxBytes;

		size_t written = 0;
		for (size_t i = 0; i < bytes && written + 4 < outSize; ++i)
		{
			int added = _snprintf_s(out + written, outSize - written, _TRUNCATE,
				(i + 1 < bytes) ? "%02X " : "%02X", entry.data[i]);
			if (added <= 0) break;      // -1 on truncation; buffer is terminated
			written += (size_t)added;
		}
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
	// Filtering
	// -----------------------------------------------------------------------
	bool ContainsFold(const char* haystack, const char* needle, bool caseSensitive)
	{
		if (!needle || !*needle) return true;
		if (!haystack) return false;
		if (caseSensitive) return strstr(haystack, needle) != nullptr;

		// Case-insensitive substring search, ASCII only (all our metadata is
		// ASCII). Walks the source once per start position.
		size_t needleLen = strlen(needle);
		for (const char* p = haystack; *p; ++p)
		{
			size_t i = 0;
			while (i < needleLen && p[i]
				&& (char)tolower((unsigned char)p[i]) == (char)tolower((unsigned char)needle[i]))
			{
				++i;
			}
			if (i == needleLen) return true;
		}
		return false;
	}

	// Parses an id filter. Accepts:
	//   "0x0800" / "0800h"   hex
	//   "2048"               decimal
	//   "0x07*"              hex prefix
	// Returns false when the text is not a usable id pattern (caller then
	// treats it as a plain substring of the formatted hex/dec forms).
	bool ParseIdFilter(const char* text, unsigned& value, bool& prefixOnly,
		bool& isHex, size_t& digitCount)
	{
		if (!text || !*text) return false;

		char buf[32];
		strncpy_s(buf, sizeof(buf), text, _TRUNCATE);
		char* p = buf;

		isHex = false;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { isHex = true; p += 2; }
		else
		{
			size_t len = strlen(p);
			if (len > 1 && (p[len - 1] == 'h' || p[len - 1] == 'H'))
			{
				isHex = true;
				p[len - 1] = '\0';
			}
		}

		prefixOnly = false;
		size_t len = strlen(p);
		if (len && p[len - 1] == '*')
		{
			prefixOnly = true;
			p[--len] = '\0';
		}

		if (len == 0 || len > 8) return false;

		unsigned parsed = 0;
		for (size_t i = 0; i < len; ++i)
		{
			char c = p[i];
			unsigned digit;
			if (isHex)
			{
				if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
				else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
				else return false;
				parsed = parsed * 16u + digit;
			}
			else
			{
				if (c < '0' || c > '9') return false;
				parsed = parsed * 10u + (unsigned)(c - '0');
			}
		}

		value = parsed;
		digitCount = len;
		return true;
	}

	// True when the packet's age (seconds since capture start) is inside the
	// configured window. A 0 for either bound means "unbounded".
	bool PassesTimeFilter(const Entry& entry)
	{
		if (g_filterFromSeconds <= 0.0f && g_filterToSeconds <= 0.0f) return true;

		float age = (float)(entry.tick - g_startTick) / 1000.0f;
		if (g_filterFromSeconds > 0.0f && age < g_filterFromSeconds) return false;
		if (g_filterToSeconds > 0.0f && age > g_filterToSeconds) return false;
		return true;
	}

	bool PassesFilters(const Entry& entry)
	{
		if (!g_filterEnabled) return true;
		if (!PassesTimeFilter(entry)) return false;

		// Direction: SEND is 0, RECV is 1 in the capture; the combo uses
		// 0=any, 1=SEND, 2=RECV.
		if (g_filterDirection == 1 && entry.direction != DirectionSend) return false;
		if (g_filterDirection == 2 && entry.direction != DirectionRecv) return false;

		const char* name = PacketNames::Lookup(entry.messageId);
		const char* cls = PacketNames::ClassName(entry.messageId);
		const char* meaning = PacketNames::Meaning(entry.messageId);

		if (g_filterAnnotatedOnly)
		{
			bool known = (cls && *cls) || (meaning && *meaning);
			if (!known) return false;
		}

		if (g_filterClass[0] && !ContainsFold(cls, g_filterClass, false)) return false;

		if (g_filterId[0])
		{
			unsigned value = 0;
			bool prefixOnly = false, isHex = false;
			size_t digits = 0;
			bool matches = false;

			if (ParseIdFilter(g_filterId, value, prefixOnly, isHex, digits))
			{
				unsigned id = entry.messageId;
				if (prefixOnly && isHex)
				{
					// Hex prefix: compare the top `digits` nibbles.
					unsigned shift = (unsigned)(4u * digits);
					matches = (shift >= 32u) || ((id >> shift) == value);
				}
				else if (prefixOnly)
				{
					// Decimal prefix: compare the top `digits` decimal digits.
					unsigned scale = 1u;
					for (size_t d = 0; d < digits && scale != 0; ++d) scale *= 10u;
					matches = (scale == 0) || ((id / scale) == value);
				}
				else
				{
					matches = (id == value);
				}
			}
			else
			{
				// Not a numeric pattern - substring match on both spellings.
				char hex[16], dec[16];
				_snprintf_s(hex, _TRUNCATE, "0x%04X", (unsigned)entry.messageId);
				_snprintf_s(dec, _TRUNCATE, "%u", (unsigned)entry.messageId);
				matches = ContainsFold(hex, g_filterId, false) ||
					ContainsFold(dec, g_filterId, false);
			}

			if (!matches) return false;
		}

		if (g_filterText[0])
		{
			char idHex[16];
			_snprintf_s(idHex, _TRUNCATE, "0x%04X", (unsigned)entry.messageId);
			if (!ContainsFold(name, g_filterText, g_filterTextCase) &&
				!ContainsFold(cls, g_filterText, g_filterTextCase) &&
				!ContainsFold(meaning, g_filterText, g_filterTextCase) &&
				!ContainsFold(idHex, g_filterText, g_filterTextCase))
				return false;
		}

		return true;
	}

	// Rebuilds the class-name combo choices from the packets currently in the
	// ring, so the list reflects what the session actually contains.
	void RebuildClassChoices(const Snapshot& snapshot)
	{
		g_classChoiceCount = 0;
		long count = snapshot.count;
		for (long i = 0; i < count && g_classChoiceCount < kMaxClassChoices; ++i)
		{
			long index = (snapshot.head - count + i + kRingSize * 2) % kRingSize;
			const char* cls = PacketNames::ClassName(snapshot.ring[index].messageId);
			if (!cls || !*cls) continue;

			bool seen = false;
			for (int c = 0; c < g_classChoiceCount; ++c)
			{
				if (strcmp(g_classChoices[c], cls) == 0) { seen = true; break; }
			}
			if (seen) continue;

			strncpy_s(g_classChoices[g_classChoiceCount], sizeof(g_classChoices[0]), cls, _TRUNCATE);
			++g_classChoiceCount;
		}
	}

	// -----------------------------------------------------------------------
	// Settings persistence (overlay.ini, next to the game exe)
	// -----------------------------------------------------------------------
	// The panel's own layout and filter state are kept separate from
	// packet_log.ini (which the capture layer owns): a "Save Settings" click
	// writes this file, and it is read back once on the first frame.
	void OverlayIniPath(char* out, size_t outSize)
	{
		out[0] = '\0';
		char exePath[MAX_PATH] = { 0 };
		if (!GetModuleFileNameA(NULL, exePath, MAX_PATH)) return;
		char* slash = strrchr(exePath, '\\');
		if (slash) *(slash + 1) = '\0';
		_snprintf_s(out, outSize, _TRUNCATE, "%soverlay.ini", exePath);
	}

	void SaveOverlaySettings()
	{
		char path[MAX_PATH];
		OverlayIniPath(path, sizeof(path));
		if (!path[0]) return;

		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f) return;

		fprintf(f, "; Manager overlay - written by the Save Settings button\r\n");
		fprintf(f, "filter_enabled=%d\r\n", g_filterEnabled ? 1 : 0);
		fprintf(f, "filter_text=%s\r\n", g_filterText);
		fprintf(f, "filter_id=%s\r\n", g_filterId);
		fprintf(f, "filter_class=%s\r\n", g_filterClass);
		fprintf(f, "filter_direction=%d\r\n", g_filterDirection);
		fprintf(f, "filter_known_only=%d\r\n", g_filterAnnotatedOnly ? 1 : 0);
		fprintf(f, "filter_case=%d\r\n", g_filterTextCase ? 1 : 0);
		fprintf(f, "filter_from=%.2f\r\n", g_filterFromSeconds);
		fprintf(f, "filter_to=%.2f\r\n", g_filterToSeconds);
		fprintf(f, "tab=%d\r\n", g_activeTab);
		fclose(f);
	}

	bool LoadOverlaySettings()
	{
		char path[MAX_PATH];
		OverlayIniPath(path, sizeof(path));
		if (!path[0]) return false;

		FILE* f = nullptr;
		if (fopen_s(&f, path, "r") != 0 || !f) return false;

		char line[512];
		while (fgets(line, sizeof(line), f))
		{
			if (line[0] == ';' || line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
			char* equals = strchr(line, '=');
			if (!equals) continue;
			*equals = '\0';
			const char* key = line;
			char* value = equals + 1;

			// Trim the trailing newline the fgets kept.
			size_t vlen = strlen(value);
			while (vlen && (value[vlen - 1] == '\n' || value[vlen - 1] == '\r'))
				value[--vlen] = '\0';

			// Case-insensitive key compare.
			if (_stricmp(key, "filter_enabled") == 0) g_filterEnabled = (atoi(value) != 0);
			else if (_stricmp(key, "filter_text") == 0) strncpy_s(g_filterText, sizeof(g_filterText), value, _TRUNCATE);
			else if (_stricmp(key, "filter_id") == 0) strncpy_s(g_filterId, sizeof(g_filterId), value, _TRUNCATE);
			else if (_stricmp(key, "filter_class") == 0) strncpy_s(g_filterClass, sizeof(g_filterClass), value, _TRUNCATE);
			else if (_stricmp(key, "filter_direction") == 0) g_filterDirection = atoi(value);
			else if (_stricmp(key, "filter_known_only") == 0) g_filterAnnotatedOnly = (atoi(value) != 0);
			else if (_stricmp(key, "filter_case") == 0) g_filterTextCase = (atoi(value) != 0);
			else if (_stricmp(key, "filter_from") == 0) g_filterFromSeconds = (float)atof(value);
			else if (_stricmp(key, "filter_to") == 0) g_filterToSeconds = (float)atof(value);
			else if (_stricmp(key, "tab") == 0)
			{
				int tab = atoi(value);
				if (tab < 0) tab = 0;
				if (tab > 4) tab = 4;
				g_activeTab = tab;
				g_requestedTab = tab;
			}
		}
		fclose(f);

		if (g_filterDirection < 0) g_filterDirection = 0;
		if (g_filterDirection > 2) g_filterDirection = 2;
		return true;
	}

	// Records that the panel state changed; TickAutosave() flushes it once the
	// user has stopped fiddling.
	void MarkSettingsDirty()
	{
		g_settingsDirty = true;
		g_settingsDirtySince = (float)ImGui::GetTime();
	}

	// Called once per frame while the panel is open. Writes overlay.ini after
	// kAutosaveDelay seconds without a further change.
	void TickAutosave()
	{
		if (!g_settingsDirty) return;
		if (ImGui::GetTime() - g_settingsDirtySince < kAutosaveDelay) return;
		SaveOverlaySettings();
		g_settingsDirty = false;
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
	// Packet builder - helpers
	// -----------------------------------------------------------------------
	// The builder's job is to replay a message the client itself sent, with
	// the body bytes optionally edited. Everything below works on the SAME
	// body layout the wire format uses:
	//
	//   body[0..1] = message id (little endian)
	//   body[2..]  = protobuf payload
	//
	// which for a full packet is [u16 len][u16 id][protobuf] minus its 4-byte
	// header - see packet_capture.h. Consequently the first protobuf field
	// starts at body+2, not body+0.

	// Reads a protobuf field one at a time, varint wiretype only. Returns
	// false when the bytes stop parsing (the captured jump's tail is not
	// protobuf, so a caller must be ready for this).
	struct ProtoField
	{
		int      tagOffset;   // index of the tag byte within the payload
		int      fieldNumber;
		int      wireType;
		uint64_t value;       // varint value (wiretype 0 only)
		int      nextOffset;  // where the next field starts
	};

	bool ReadProtoField(const uint8_t* data, int size, int offset, ProtoField& out)
	{
		if (!data || offset < 0 || offset >= size) return false;

		uint8_t tag = data[offset];
		out.tagOffset = offset;
		out.fieldNumber = tag >> 3;
		out.wireType = tag & 0x07;
		out.nextOffset = offset + 1;

		if (out.wireType != 0) return false;   // only varints are decoded

		// Varint payload: up to 10 bytes, low 7 bits each.
		uint64_t value = 0;
		int shift = 0;
		int p = offset + 1;
		while (p < size && shift <= 63)
		{
			uint8_t byte = data[p++];
			value |= (uint64_t)(byte & 0x7F) << shift;
			if (!(byte & 0x80))
			{
				out.value = value;
				out.nextOffset = p;
				return true;
			}
			shift += 7;
		}
		return false;   // unterminated varint
	}

	// Appends a varint to a buffer, returning the new length (or the
	// original when it would overflow).
	int WriteVarint(uint8_t* out, int offset, int maxBytes, uint64_t value)
	{
		do
		{
			if (offset >= maxBytes) return offset;
			uint8_t byte = (uint8_t)(value & 0x7F);
			value >>= 7;
			if (value) byte |= 0x80;
			out[offset++] = byte;
		} while (value);
		return offset;
	}

	int VarintSize(uint64_t value)
	{
		int size = 1;
		while (value >= 0x80) { value >>= 7; ++size; }
		return size;
	}

	// Re-encodes a varint field in place. `valueOffset` is the first byte of
	// the existing varint; the field is rewritten tag + new varint and the
	// remainder of the buffer is shifted to fit. Returns the new body length,
	// or `bodyLength` unchanged when it cannot fit.
	int ReplaceVarint(uint8_t* body, int bodyLength, int tagOffset, int valueOffset,
		uint64_t value, int maxBytes)
	{
		if (tagOffset < 0 || valueOffset <= tagOffset || valueOffset > bodyLength) return bodyLength;

		int oldValueBytes = 0;
		{
			int p = valueOffset;
			while (p < bodyLength)
			{
				++oldValueBytes;
				if (!(body[p] & 0x80)) break;
				++p;
			}
		}

		const int newValueBytes = VarintSize(value);
		const int delta = newValueBytes - oldValueBytes;
		const int newLength = bodyLength + delta;

		if (newLength > maxBytes || newLength < 0) return bodyLength;
		if (delta > 0)
			memmove(body + valueOffset + newValueBytes, body + valueOffset + oldValueBytes,
				(size_t)(bodyLength - (valueOffset + oldValueBytes)));

		// The tag byte is left untouched: it already encodes this field
		// number with wiretype 0 (varint), which is what we are writing.
		WriteVarint(body, valueOffset, maxBytes, value);
		return newLength;
	}

	// -----------------------------------------------------------------------
	// Packet builder - load / send
	// -----------------------------------------------------------------------

	// Pulls the captured template for `messageId` into the editable state.
	// Returns true when a capture was found.
	bool LoadBuilderTemplate(uint16_t messageId, bool keepEdits)
	{
		PacketSend::Slot slot;
		if (!PacketSend::GetSlot(messageId, slot)) return false;

		g_buildArmedId = messageId;
		g_buildHasTemplate = true;

		if (!keepEdits || g_buildBodyBytes == 0)
		{
			int n = slot.bodyBytes;
			if (n > PacketSend::kMaxBodyBytes) n = PacketSend::kMaxBodyBytes;
			memcpy(g_buildBody, slot.body, (size_t)n);
			g_buildBodyBytes = n;
			g_buildRawDirty = true;
		}

		// The template's first protobuf field sits at body+2 (body+0..1 is
		// the id). Surface it as the "mode" hint when it looks like one.
		ProtoField first;
		if (ReadProtoField(g_buildBody, g_buildBodyBytes, 2, first) && first.fieldNumber == 1)
			g_buildMode = (uint16_t)first.value;

		return true;
	}

	// Issues `count` sends of the current body through PacketSend, with an
	// optional gap between them. Non-blocking: the caller drives it from the
	// frame loop via g_buildPendingSends.
	void SendBuilderOnce()
	{
		int result = PacketSend::SendSlot(g_buildArmedId,
			g_buildBody, g_buildBodyBytes);

		g_buildFlashUntil = (float)ImGui::GetTime() + 2.5f;
		g_buildFlashError = (result != 0);
		if (result == 0)
			_snprintf_s(g_buildFlashText, _TRUNCATE, "Sent 0x%04X (%d bytes)",
				(unsigned)g_buildArmedId, g_buildBodyBytes);
		else if (result < 0)
			_snprintf_s(g_buildFlashText, _TRUNCATE,
				"Refused 0x%04X - no capture yet? (jump in-game first)", (unsigned)g_buildArmedId);
		else
			_snprintf_s(g_buildFlashText, _TRUNCATE,
				"0x%04X: socket send error (%d)", (unsigned)g_buildArmedId, result);
	}

	// Advances the "send N times" pacer. Called every frame from the panel.
	void TickBuilderPacer()
	{
		if (g_buildPendingSends <= 0) return;

		const float now = (float)ImGui::GetTime();
		const float gap = g_buildMinIntervalMs / 1000.0f;

		if (gap <= 0.0f)
		{
			// No pacing: fire the whole batch this frame.
			while (g_buildPendingSends > 0)
			{
				SendBuilderOnce();
				--g_buildPendingSends;
			}
			return;
		}

		if (now >= g_buildNextSendTime)
		{
			SendBuilderOnce();
			--g_buildPendingSends;
			g_buildNextSendTime = now + gap;
		}
	}

	// Advances the jump-speed auto-fire loop. Uses the same send path, but
	// bumps the template's timestamp field forward by g_jumpSpeedMs on each
	// jump so the server sees a client that is "ahead" and keeps accepting.
	void TickJumpAutoFire()
	{
		if (!g_jumpAutoFire) return;
		if (!g_buildHasTemplate) return;

		const float now = (float)ImGui::GetTime();
		if (now < g_jumpNextTime) return;

		SendBuilderOnce();
		g_jumpNextTime = now + (g_jumpRepeatMs / 1000.0f);
	}

	// -----------------------------------------------------------------------
	// Template header (title-adjacent rows, above the tab bar)
	// -----------------------------------------------------------------------
	void DrawTemplateHeader()
	{
		Caption("PRESS [INSERT] to toggle overlay.");
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		// A dedicated drag strip. ImGui lets you move a window by its title bar
		// or by dragging any empty background, but this panel's body is almost
		// entirely filled with widgets, so there is often nothing left to grab.
		// An InvisibleButton gives a real, unambiguous hit target to drag by.
		const float dragWidth = 210.0f;
		const float dragHeight = ImGui::GetTextLineHeight();
		const ImVec2 dragOrigin = ImGui::GetCursorScreenPos();

		ImGui::InvisibleButton("##panel_drag", ImVec2(dragWidth, dragHeight));
		const bool dragHovered = ImGui::IsItemHovered();
		const bool dragHeld = ImGui::IsItemActive();

		if (dragHovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

		if (dragHeld)
		{
			// Remember where inside the bar the user grabbed it, so the window
			// does not snap its corner to the cursor on the first move.
			if (!g_dragOffsetValid)
			{
				ImVec2 mouse = ImGui::GetIO().MousePos;
				ImVec2 pos = ImGui::GetWindowPos();
				g_dragOffset = ImVec2(mouse.x - pos.x, mouse.y - pos.y);
				g_dragOffsetValid = true;
			}
			ImVec2 mouse = ImGui::GetIO().MousePos;
			ImGui::SetWindowPos(ImVec2(mouse.x - g_dragOffset.x, mouse.y - g_dragOffset.y));
		}
		else
		{
			g_dragOffsetValid = false;
		}

		// Paint the hint over the invisible button.
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImGuiCol hintColour = (dragHovered || dragHeld)
				? ImGuiCol_Text : ImGuiCol_TextDisabled;
			drawList->AddText(ImVec2(dragOrigin.x + 2.0f, dragOrigin.y),
				ImGui::GetColorU32(hintColour), "drag here to move");
		}

		ImGui::SameLine();
		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

		ImGui::Spacing();

		if (ImGui::Button("Save Settings", ImVec2(120.0f, 0.0f)))
		{
			SaveOverlaySettings();
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
			Caption("Replays the jump packet the client itself sends, so the server "
				"sees a legitimate CMsgAction. Falls back to normal jumps when "
				"players are nearby.");

			// The real controls. They only work once a jump has been captured
			// (see the Packet Builder on the Packets tab), so the state is
			// reported honestly rather than pretending to send.
			const bool armed = g_buildHasTemplate && g_buildArmedId == PacketSend::kDefaultBuildId;
			if (!armed)
			{
				ImGui::TextDisabled("Not armed - jump once, then load 0x%04X "
					"on the Packets tab.", (unsigned)PacketSend::kDefaultBuildId);
			}
			else
			{
				ImGui::SetNextItemWidth(90.0f);
				ImGui::InputInt("Lead (ms/jump)", &g_jumpSpeedMs, 500, 5000);
				if (g_jumpSpeedMs < 0) g_jumpSpeedMs = 0;
				ImGui::SetNextItemWidth(90.0f);
				ImGui::InputFloat("Interval (ms)", &g_jumpRepeatMs, 1.0f, 10.0f, "%.0f");
				if (g_jumpRepeatMs < 20.0f) g_jumpRepeatMs = 20.0f;

				if (ImGui::Checkbox("Auto-Jump", &g_jumpAutoFire))
				{
					g_jumpNextTime = (float)ImGui::GetTime();
					HookLog("[Send] auto-jump %s", g_jumpAutoFire ? "on" : "off");
				}
				ImGui::SameLine();
				if (ImGui::Button("Jump Once"))
					SendBuilderOnce();

				ImGui::SameLine();
				Caption("sent %ld", PacketSend::TotalSent());
			}

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
	// Number of rows in the ring that pass the active filter.
	long CountFiltered(const Snapshot& snapshot)
	{
		long shown = 0;
		long count = snapshot.count;
		for (long i = 0; i < count; ++i)
		{
			long index = (snapshot.head - count + i + kRingSize * 2) % kRingSize;
			if (PassesFilters(snapshot.ring[index])) ++shown;
		}
		return shown;
	}

	// Appends one TSV row for the packet.
	void AppendTsvRow(char* out, size_t outSize, const Entry& entry)
	{
		char clock[32];
		FormatClock(entry.tick, clock, sizeof(clock));

		char preview[512];
		BuildHexPreview(entry, preview, sizeof(preview), entry.storedBytes);

		size_t used = strlen(out);
		_snprintf_s(out + used, outSize - used, _TRUNCATE,
			"%06u\t%s\t%s\t%u\t0x%04X\t%s\t%s\t%s\t%s\n",
			entry.seq, clock,
			entry.direction == DirectionSend ? "SEND" : "RECV",
			(unsigned)entry.length, (unsigned)entry.messageId,
			PacketNames::Lookup(entry.messageId),
			PacketNames::ClassName(entry.messageId),
			PacketNames::Meaning(entry.messageId),
			preview);
	}

	// Copies every row that passes the filter, as TSV, to the clipboard.
	// Returns the number of rows written.
	long CopyFilteredRows(const Snapshot& snapshot)
	{
		// Worst case per row (id + ~1000 hex bytes + metadata) with slack.
		size_t capacity = (size_t)snapshot.count * 1280 + 512;
		char* buffer = (char*)malloc(capacity);
		if (!buffer) return 0;
		buffer[0] = '\0';

		// Header row so the paste is self-describing.
		strncpy_s(buffer, capacity,
			"seq\ttime\tdir\tlen\tid\tname\tclass\tmeaning\tbytes\n", _TRUNCATE);

		long written = 0;
		long count = snapshot.count;
		for (long i = 0; i < count; ++i)
		{
			long index = (snapshot.head - count + i + kRingSize * 2) % kRingSize;
			const Entry& entry = snapshot.ring[index];
			if (!PassesFilters(entry)) continue;
			if (strlen(buffer) + 1400 >= capacity) break;   // never overflow
			AppendTsvRow(buffer, capacity, entry);
			++written;
		}

		ImGui::SetClipboardText(buffer);
		free(buffer);
		return written;
	}

	// Copies just the selected packet's hex dump (classic "xxd"-ish layout).
	bool CopySelectedHex(const Snapshot& snapshot)
	{
		const Entry* entry = nullptr;
		if (g_hasSelection)
		{
			long index = IndexOfSeq(snapshot, g_selectedSeq);
			if (index >= 0) entry = &snapshot.ring[index];
		}
		if (!entry) return false;

		size_t capacity = (size_t)entry->storedBytes * 5 + 256;
		char* buffer = (char*)malloc(capacity);
		if (!buffer) return false;
		buffer[0] = '\0';

		char clock[32];
		FormatClock(entry->tick, clock, sizeof(clock));
		strncat_s(buffer, capacity, "# Conquer packet\n", _TRUNCATE);
		char header[256];
		_snprintf_s(header, _TRUNCATE,
			"# seq=%u time=%s dir=%s len=%u id=0x%04X name=%s class=%s\n",
			entry->seq, clock,
			entry->direction == DirectionSend ? "SEND" : "RECV",
			(unsigned)entry->length, (unsigned)entry->messageId,
			PacketNames::Lookup(entry->messageId),
			PacketNames::ClassName(entry->messageId));
		strncat_s(buffer, capacity, header, _TRUNCATE);

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
			_snprintf_s(line + written, sizeof(line) - written, _TRUNCATE, " |%s|\n", ascii);
			strncat_s(buffer, capacity, line, _TRUNCATE);
		}

		ImGui::SetClipboardText(buffer);
		free(buffer);
		return true;
	}

	void FlashCopied(const char* what, long count)
	{
		g_copyFlashUntil = (float)ImGui::GetTime() + 2.0f;
		if (count >= 0)
			_snprintf_s(g_copyFlashText, _TRUNCATE, "Copied %ld %s", count, what);
		else
			_snprintf_s(g_copyFlashText, _TRUNCATE, "%s", what);
	}

	bool AnyFilterActive()
	{
		return g_filterText[0] || g_filterId[0] || g_filterClass[0] ||
			g_filterDirection != 0 || g_filterAnnotatedOnly ||
			g_filterFromSeconds > 0.0f || g_filterToSeconds > 0.0f;
	}

	void ResetFilters()
	{
		g_filterText[0] = '\0';
		g_filterId[0] = '\0';
		g_filterClass[0] = '\0';
		g_filterDirection = 0;
		g_filterAnnotatedOnly = false;
		g_filterFromSeconds = 0.0f;
		g_filterToSeconds = 0.0f;
		g_filterEnabled = false;
	}

	// Cheap fingerprint of everything the autosave persists. When it changes,
	// the state is marked dirty; comparing strings avoids threading change
	// notifications through every widget.
	void NoteFilterChanges()
	{
		static char last[512] = "";
		char now[512];
		_snprintf_s(now, _TRUNCATE, "%d|%s|%s|%s|%d|%d|%d|%.2f|%.2f|%d",
			g_filterEnabled ? 1 : 0, g_filterText, g_filterId, g_filterClass,
			g_filterDirection, g_filterAnnotatedOnly ? 1 : 0, g_filterTextCase ? 1 : 0,
			g_filterFromSeconds, g_filterToSeconds, g_activeTab);

		if (strcmp(now, last) != 0)
		{
			strncpy_s(last, sizeof(last), now, _TRUNCATE);
			MarkSettingsDirty();
		}
	}

	// The filter row above the table. Keeps the ring lock held by the caller.
	void DrawFilterBar(const Snapshot& snapshot, float windowWidth)
	{
		ImGui::Checkbox("Filter", &g_filterEnabled);
		ImGui::SameLine();

		// Free text covers name / class / meaning / id at once.
		ImGui::SetNextItemWidth(240.0f);
		ImGui::InputTextWithHint("##filtertext", "search name, class, meaning, id...",
			g_filterText, sizeof(g_filterText));
		ImGui::SameLine();
		ImGui::Checkbox("Aa", &g_filterTextCase);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Case sensitive");

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		// Id box: exact, hex, or a prefix with a trailing '*'.
		ImGui::SetNextItemWidth(96.0f);
		ImGui::InputTextWithHint("##filterid", "id 0x07*", g_filterId, sizeof(g_filterId));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("0x0800 or 2048 for an exact id; 0x07* or 07* for a prefix");

		ImGui::SameLine();
		ImGui::SetNextItemWidth(150.0f);
		ImGui::InputTextWithHint("##filterclass", "class CMsg...", g_filterClass, sizeof(g_filterClass));

		ImGui::SameLine();
		static const char* directions[] = { "Any dir", "SEND", "RECV" };
		ImGui::SetNextItemWidth(96.0f);
		ImGui::Combo("##filterdir", &g_filterDirection, directions, IM_COUNTOF(directions));

		ImGui::SameLine();
		ImGui::Checkbox("Known only", &g_filterAnnotatedOnly);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Hide ids with no recovered name/class");

		// Time window, in seconds since capture start (0 = unbounded).
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(74.0f);
		ImGui::InputFloat("##from", &g_filterFromSeconds, 0.0f, 0.0f, "%.0f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("From second (0 = start)");
		ImGui::SameLine();
		ImGui::TextDisabled("to");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(74.0f);
		ImGui::InputFloat("##to", &g_filterToSeconds, 0.0f, 0.0f, "%.0f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("To second (0 = now)");
		if (g_filterFromSeconds < 0.0f) g_filterFromSeconds = 0.0f;
		if (g_filterToSeconds < 0.0f) g_filterToSeconds = 0.0f;

		// Class quick-pick, populated from the ring.
		if (g_classChoiceCount > 0)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(190.0f);
			if (ImGui::BeginCombo("##classpick", "pick class...", ImGuiComboFlags_HeightSmall))
			{
				for (int c = 0; c < g_classChoiceCount; ++c)
				{
					bool selected = (strcmp(g_classChoices[c], g_filterClass) == 0);
					if (ImGui::Selectable(g_classChoices[c], selected))
						strncpy_s(g_filterClass, sizeof(g_filterClass), g_classChoices[c], _TRUNCATE);
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		// Copy + reset on the right.
		long shown = CountFiltered(snapshot);
		char copyLabel[48];
		_snprintf_s(copyLabel, _TRUNCATE, "Copy %ld rows", shown);

		const char* resetLabel = "Reset";
		float rowWidth = ImGui::CalcTextSize(copyLabel).x + ImGui::CalcTextSize(resetLabel).x
			+ ImGui::GetStyle().FramePadding.x * 8.0f
			+ ImGui::CalcTextSize("| Copy all").x + 48.0f;
		float x = windowWidth - rowWidth - ImGui::GetStyle().WindowPadding.x;
		if (x > ImGui::GetCursorPosX())
		{
			ImGui::SameLine(x);

			if (ImGui::Button(copyLabel))
				FlashCopied("rows to clipboard", CopyFilteredRows(snapshot));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Copy every visible row as TSV");

			ImGui::SameLine();
			if (ImGui::Button("Reset"))
				ResetFilters();

			ImGui::SameLine();
			ImGui::BeginDisabled(!g_hasSelection);
			if (ImGui::Button("Copy hex"))
			{
				if (CopySelectedHex(snapshot))
					FlashCopied("selected packet's hex to clipboard", -1);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Copy the selected packet's hex dump");
			ImGui::EndDisabled();
		}

		// Feedback line.
		if (g_copyFlashUntil > 0.0f && ImGui::GetTime() < g_copyFlashUntil)
		{
			ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "%s", g_copyFlashText);
		}
		else if (AnyFilterActive())
		{
			ImGui::TextDisabled("filter active: %ld of %ld retained rows match",
				shown, snapshot.count);
		}
		else
		{
			ImGui::TextDisabled("all %ld retained rows shown", snapshot.count);
		}

		ImGui::Separator();
	}

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

		long shown = CountFiltered(snapshot);
		const long sent = PacketSend::TotalSent();
		const long refused = PacketSend::TotalRefused();

		if (AnyFilterActive())
		{
			ImGui::TextDisabled("filtered %ld of %ld retained  |  %s  |  sent %ld / refused %ld",
				shown, snapshot.count,
				g_autoScroll ? "following newest" : "scroll paused",
				sent, refused);
		}
		else
		{
			ImGui::TextDisabled("showing %ld of %ld retained  |  %s  |  sent %ld / refused %ld",
				shown, snapshot.count,
				g_autoScroll ? "following newest" : "scroll paused",
				sent, refused);
		}
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

		// Class + meaning, when the catalogue knows them. The class is the
		// CMsg type the client dispatch table maps this id to; the meaning is
		// the curated one-liner.
		const char* cls = PacketNames::ClassName(entry->messageId);
		const char* meaning = PacketNames::Meaning(entry->messageId);
		if ((cls && *cls) || (meaning && *meaning))
		{
			if (cls && *cls)
			{
				ImGui::TextDisabled("class:");
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.62f, 0.78f, 1.0f, 1.0f), "%s", cls);
			}
			if (meaning && *meaning)
			{
				ImGui::TextWrapped("%s", meaning);
			}
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

			// Filtered-out rows are skipped entirely: submitting them keeps
			// the scroll range honest but costs the row height, so we simply
			// do not emit a row for them.
			if (!PassesFilters(entry))
				continue;

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

	// ---- packet builder views (defined below DrawPacketBuilder) -----------
	// Forward declarations so the builder's actions row can call them without
	// the file having to be ordered bottom-up - which would put the packet
	// table after the builder, reading against the flow of the tab.
	void DrawPacketBuilder(const Snapshot& snapshot);
	void DrawBuilderFields();
	void DrawBuilderRaw();
	void DrawBuilderFlash();
	void CopyBuilderPacket();
	void BuildHexLine(const uint8_t* data, int bytes, char* out, size_t outSize);
	int  ParseHexString(const char* text, uint8_t* out, int maxBytes);

	void DrawPacketsTab(const Snapshot& snapshot)
	{
		float windowWidth = ImGui::GetWindowWidth();
		DrawPacketToolbar(windowWidth);

		// Class quick-pick choices reflect what this session has actually seen.
		RebuildClassChoices(snapshot);
		DrawFilterBar(snapshot, windowWidth);

		// Reserve room for the status bar and the detail pane, then let the
		// table take whatever remains. The detail pane may add up to two extra
		// lines (class + wrapped meaning) on top of the fixed hex height.
		float reserved = ImGui::GetTextLineHeightWithSpacing()          // status bar
			+ kDetailHeight
			+ ImGui::GetTextLineHeightWithSpacing() * 3.0f             // "protobuf body" caption + class/meaning
			+ ImGui::GetFrameHeightWithSpacing()                       // hex child border/margins
			+ ImGui::GetStyle().ItemSpacing.y * 12.0f
			+ ImGui::GetStyle().FramePadding.y * 4.0f;

		// The builder adds a collapsible block under the table. Its header is
		// one line; when expanded it takes a fixed slab, so the table shrinks
		// by a predictable amount rather than reflowing. g_buildPanelOpen is
		// this frame's answer as of the previous frame's header draw - a
		// one-frame lag that only shows as a small resize when toggling.
		reserved += ImGui::GetFrameHeightWithSpacing();
		if (g_buildPanelOpen)
			reserved += kBuilderHeight;

		float listHeight = ImGui::GetContentRegionAvail().y - reserved;
		if (listHeight < 80.0f) listHeight = 80.0f;

		DrawPacketTable(snapshot, listHeight);
		DrawDetailPane(snapshot);
		DrawPacketBuilder(snapshot);
		DrawPacketStatusBar(snapshot);
	}

	// -----------------------------------------------------------------------
	// Packet builder  (inside the Packets tab, under the detail pane)
	// -----------------------------------------------------------------------
	void DrawPacketBuilder(const Snapshot& snapshot)
	{
		ImGui::Spacing();
		// The block is open by default; CollapsingHeader remembers the user's
		// toggle in its own return value, so the state round-trips naturally.
		if (ImGui::CollapsingHeader("Packet Builder (send packets)"))
			g_buildPanelOpen = true;
		else
			g_buildPanelOpen = false;

		if (!g_buildPanelOpen) return;

		// --- id + capture --------------------------------------------------
		ImGui::SetNextItemWidth(90.0f);
		ImGui::InputText("##buildid", g_buildIdText, sizeof(g_buildIdText));
		ImGui::SameLine();

		if (ImGui::Button("Load from Capture"))
		{
			unsigned value = 0; bool prefix = false, hex = false; size_t digits = 0;
			uint16_t id = PacketSend::kDefaultBuildId;
			if (ParseIdFilter(g_buildIdText, value, prefix, hex, digits) && !prefix && digits > 0)
				id = (uint16_t)value;
			if (LoadBuilderTemplate(id, false))
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Loaded 0x%04X template (%d bytes, field 1 = %u)",
					(unsigned)id, g_buildBodyBytes, (unsigned)g_buildMode);
				g_buildFlashError = false;
				g_buildFlashUntil = (float)ImGui::GetTime() + 2.5f;
			}
			else
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"No 0x%04X captured yet - perform the action in-game first", (unsigned)id);
				g_buildFlashError = true;
				g_buildFlashUntil = (float)ImGui::GetTime() + 4.0f;
			}
		}

		ImGui::SameLine();
		if (ImGui::Button("Use Selected"))
		{
			if (g_hasSelection)
			{
				long index = IndexOfSeq(snapshot, g_selectedSeq);
				if (index >= 0)
				{
					const Entry& entry = snapshot.ring[index];
					if (entry.direction == DirectionSend && LoadBuilderTemplate(entry.messageId, false))
					{
						_snprintf_s(g_buildIdText, _TRUNCATE, "0x%04X", (unsigned)entry.messageId);
						_snprintf_s(g_buildFlashText, _TRUNCATE,
							"Loaded 0x%04X from selection", (unsigned)entry.messageId);
						g_buildFlashError = false;
						g_buildFlashUntil = (float)ImGui::GetTime() + 2.5f;
					}
				}
			}
		}
		ImGui::SameLine();
		Caption(g_buildHasTemplate
			? "template ready"
			: "no template - jump once, then Load");

		ImGui::Separator();

		if (!g_buildHasTemplate)
		{
			ImGui::TextWrapped(
				"A jump packet cannot be invented from scratch: the client's own "
				"CMsg object carries the vtable that DoSendMsg validates before it "
				"will transmit. Jump once in-game, then open this tab and press "
				"\"Load from Capture\" to arm the builder.");
			DrawBuilderFlash();
			return;
		}

		// --- decoded fields + raw hex --------------------------------------
		float colWidth = ImGui::GetContentRegionAvail().x;
		float leftWidth = colWidth * 0.46f;
		if (leftWidth < 240.0f) leftWidth = 240.0f;

		ImGui::BeginChild("##buildfields", ImVec2(leftWidth, kBuilderHeight - 60.0f), true);
		DrawBuilderFields();
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##buildraw", ImVec2(0.0f, kBuilderHeight - 60.0f), true);
		DrawBuilderRaw();
		ImGui::EndChild();

		// --- actions -------------------------------------------------------
		ImGui::SetNextItemWidth(70.0f);
		ImGui::InputInt("##repeat", &g_buildRepeatCount, 1, 10);
		if (g_buildRepeatCount < 1) g_buildRepeatCount = 1;
		if (g_buildRepeatCount > 999) g_buildRepeatCount = 999;
		ImGui::SameLine();
		Caption("x  gap(ms)");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		ImGui::InputFloat("##gap", &g_buildMinIntervalMs, 0.0f, 0.0f, "%.0f");
		if (g_buildMinIntervalMs < 0.0f) g_buildMinIntervalMs = 0.0f;

		ImGui::SameLine();
		const bool canSend = PacketSend::CanSend(g_buildArmedId);
		if (!canSend) ImGui::BeginDisabled();
		if (ImGui::Button("Send", ImVec2(90.0f, 0.0f)))
		{
			g_buildPendingSends = g_buildRepeatCount;
			g_buildNextSendTime = (float)ImGui::GetTime();
		}
		if (!canSend) ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Reload Template"))
			LoadBuilderTemplate(g_buildArmedId, false);

		ImGui::SameLine();
		if (ImGui::Button("Copy Packet"))
		{
			CopyBuilderPacket();
			g_copyFlashUntil = (float)ImGui::GetTime() + 2.0f;
			_snprintf_s(g_copyFlashText, _TRUNCATE, "Copied packet hex");
		}

		DrawBuilderFlash();
	}

	// The varint fields found in the loaded body, as editable inputs. The
	// walk stops where the bytes cease to be protobuf (the captured jump has
	// a non-protobuf tail), and that remainder is shown read-only.
	void DrawBuilderFields()
	{
		ImGui::TextDisabled("Decoded fields (varint)");
		ImGui::Separator();

		int offset = 2;          // body[0..1] is the id
		const int end = g_buildBodyBytes;
		bool anyField = false;

		while (offset < end)
		{
			ProtoField field;
			const int tagOffset = offset;
			if (!ReadProtoField(g_buildBody, end, offset, field))
				break;

			anyField = true;

			// A varint can be 64 bits; clamp for the edit widget.
			uint64_t clamped = field.value;
			if (clamped > 0xFFFFFFFFull) clamped = 0xFFFFFFFFull;
			int value32 = (int)clamped;

			char label[64];
			_snprintf_s(label, _TRUNCATE, "field %d (body+%d)", field.fieldNumber, tagOffset);

			ImGui::SetNextItemWidth(150.0f);
			ImGui::PushID(tagOffset);
			// CharsHexadecimal makes the box read/write hex; the +/- steps
			// are 1 and 16 so the arrows move a nibble and a byte.
			if (ImGui::InputInt(label, &value32, 1, 16, ImGuiInputTextFlags_CharsHexadecimal))
			{
				uint64_t newValue = (uint64_t)(uint32_t)value32;
				int newLength = ReplaceVarint(g_buildBody, g_buildBodyBytes,
					tagOffset, tagOffset + 1, newValue, PacketSend::kMaxBodyBytes);
				if (newLength != g_buildBodyBytes)
				{
					g_buildBodyBytes = newLength;
					g_buildRawDirty = true;
					// Field offsets shifted; re-walk from the start next frame.
					ImGui::PopID();
					break;
				}
			}
			ImGui::PopID();

			offset = field.nextOffset;
		}

		if (!anyField)
			Caption("No varint fields decoded.");

		// The non-protobuf tail, if any.
		if (offset < end)
		{
			ImGui::Separator();
			ImGui::TextDisabled("Unparsed tail (body+%d, %d bytes)", offset, end - offset);
			char tail[192];
			BuildHexLine(g_buildBody + offset, end - offset, tail, sizeof(tail));
			ImGui::TextWrapped("%s", tail);
		}
	}

	// Editable raw hex of the whole body, plus a length readout.
	void DrawBuilderRaw()
	{
		ImGui::TextDisabled("Raw body (%d bytes)", g_buildBodyBytes);
		ImGui::Separator();

		// When the decoded view (or a template load) changed the bytes, the
		// text box is stale and must be re-rendered from them. Doing it here
		// keeps every mutation site free of UI knowledge.
		if (g_buildRawDirty)
		{
			BuildHexLine(g_buildBody, g_buildBodyBytes, g_buildRawText, sizeof(g_buildRawText));
			g_buildRawDirty = false;
		}

		ImGui::InputTextMultiline("##rawbody", g_buildRawText, sizeof(g_buildRawText),
			ImVec2(-FLT_MIN, -30.0f));

		// Re-parse the text box into bytes when it changes. Kept separate
		// from the decoded view so a half-typed hex string does not corrupt
		// the live body.
		if (ImGui::Button("Apply Raw Hex"))
		{
			int written = ParseHexString(g_buildRawText, g_buildBody, PacketSend::kMaxBodyBytes);
			if (written >= 0)
			{
				g_buildBodyBytes = written;
				// Keep the id consistent with the body's first two bytes.
				if (written >= 2)
					g_buildArmedId = (uint16_t)(g_buildBody[0] | (g_buildBody[1] << 8));
				_snprintf_s(g_buildFlashText, _TRUNCATE, "Applied %d bytes", written);
				g_buildFlashError = false;
				g_buildFlashUntil = (float)ImGui::GetTime() + 2.0f;
			}
			else
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE, "Hex parse failed (odd digit or bad char)");
				g_buildFlashError = true;
				g_buildFlashUntil = (float)ImGui::GetTime() + 3.0f;
			}
		}

		ImGui::SameLine();
		Caption("id + protobuf payload; length fixed up automatically");
	}

	void DrawBuilderFlash()
	{
		if (ImGui::GetTime() >= g_buildFlashUntil) return;
		if (g_buildFlashError)
			ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", g_buildFlashText);
		else
			ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "%s", g_buildFlashText);
	}

	// Copies the current body as a ready-to-send packet hex string
	// (body only, since the builder re-derives the header).
	void CopyBuilderPacket()
	{
		static char hex[PacketSend::kMaxBodyBytes * 3 + 8];
		BuildHexLine(g_buildBody, g_buildBodyBytes, hex, sizeof(hex));
		ImGui::SetClipboardText(hex);
	}

	// "08 8E CB 52 38 F9 02" - a space-separated hex string of the bytes.
	void BuildHexLine(const uint8_t* data, int bytes, char* out, size_t outSize)
	{
		if (!out || outSize == 0) return;
		out[0] = '\0';
		size_t written = 0;
		for (int i = 0; i < bytes && written + 4 < outSize; ++i)
		{
			int added = _snprintf_s(out + written, outSize - written, _TRUNCATE,
				(i + 1 < bytes) ? "%02X " : "%02X", data[i]);
			if (added <= 0) break;
			written += (size_t)added;
		}
	}

	// Parses "08 8ECB52 38" (spaces optional, case-insensitive). Returns the
	// byte count, or -1 when a non-hex/odd-length digit is present.
	int ParseHexString(const char* text, uint8_t* out, int maxBytes)
	{
		if (!text) return -1;
		int count = 0;
		int high = -1;

		for (const char* p = text; *p; ++p)
		{
			char c = *p;
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;

			int digit;
			if (c >= '0' && c <= '9') digit = c - '0';
			else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
			else return -1;

			if (high < 0) high = digit;
			else
			{
				if (count >= maxBytes) return -1;
				out[count++] = (uint8_t)((high << 4) | digit);
				high = -1;
			}
		}

		if (high >= 0) return -1;   // dangling nibble
		return count;
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
		// Place the window once. ImGuiCond_FirstUseEver means: "use this only
		// if there is no position/size stored for this window yet", so a drag
		// the user performs is preserved (and survives the next launch) rather
		// than being overwritten on the following frame.
		if (g_windowPlacementPending)
		{
			ImGui::SetNextWindowPos(g_initialPos, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(g_initialSize, ImGuiCond_FirstUseEver);
			g_windowPlacementPending = false;
		}

		const ImGuiWindowFlags windowFlags =
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoSavedSettings;

		ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 420.0f), ImVec2(FLT_MAX, FLT_MAX));

		// The ring lock is taken once for the whole panel and released on every
		// path out of Begin(), including the collapsed case where Begin()
		// returns false and no content is drawn.
		Snapshot snapshot = BeginRead();

		if (ImGui::Begin(kWindowTitle, &g_visible, windowFlags))		{
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

			// Persist filter/tab changes a moment after the user stops editing.
			NoteFilterChanges();
			TickAutosave();
		}

		// Send pacing runs whether or not the panel is showing, so a batch
		// started before the window was hidden still completes.
		TickBuilderPacer();
		TickJumpAutoFire();

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

		// Conquer is a multi-window client: the root frame, the render child and
		// a few hidden message-only windows all belong to the same process. The
		// old test compared the foreground window against a single cached HWND
		// (the render window), which is almost never the one that owns focus -
		// the root frame is - so the hotkeys were effectively dead. Comparing
		// process ids accepts any of them.
		DWORD processId = 0;
		GetWindowThreadProcessId(foreground, &processId);
		return processId == GetCurrentProcessId();
	}

	// True only on the frame the key transitions from up to down, so holding
	// the key does not repeat the action.
	bool KeyPressedEdge(int virtualKey, bool& wasDown)
	{
		// GetAsyncKeyState's low bit is "pressed since the last call", but it is
		// per-thread and shared with every other reader. The high bit (currently
		// down) plus our own latch is the reliable edge test.
		SHORT state = GetAsyncKeyState(virtualKey);
		bool down = (state & 0x8000) != 0;
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

		// Sample unconditionally so the edge latches stay correct even while
		// another window has focus and we deliberately ignore the result.
		bool insert = KeyPressedEdge(VK_INSERT, insertDown);
		bool f8 = KeyPressedEdge(VK_F8, f8Down);
		bool f9 = KeyPressedEdge(VK_F9, f9Down);
		bool f10 = KeyPressedEdge(VK_F10, f10Down);
		bool escape = KeyPressedEdge(VK_ESCAPE, escapeDown);
		bool middle = KeyPressedEdge(VK_MBUTTON, middleDown);

		// Only act while the game owns the foreground, so the overlay does not
		// toggle while the user is typing in another application.
		if (!IsGameForeground()) return;

		// Insert and F8 are the show/hide pair.
		if (insert || f8)
		{
			g_visible = !g_visible;

			// Hiding the panel must not leave the overlay holding the mouse:
			// ImGui's Win32 capture would otherwise keep swallowing clicks that
			// belong to the game until the panel was shown again.
			//
			// We deliberately do NOT reach for ImGui's internal ClearActiveID()
			// here (it lives in imgui_internal.h, not the public imgui.h). It is
			// unnecessary: ProcessMessage() takes an explicit "overlay hidden"
			// path that releases the Win32 capture, and ImGui drops its own
			// ActiveId by itself once the window stops being submitted.
			if (!g_visible)
			{
				if (g_renderWindow && ::GetCapture() == g_renderWindow)
					::ReleaseCapture();

				// Synthesise a release for any button ImGui still believes is
				// held, so a hide mid-drag does not leave it stuck down.
				ImGuiIO& io = ImGui::GetIO();
				for (int button = 0; button < IM_COUNTOF(io.MouseDown); ++button)
				{
					if (io.MouseDown[button])
						io.AddMouseButtonEvent(button, false);
				}
			}

			HookLog("[Overlay] %s (Insert/F8)", g_visible ? "shown" : "hidden");
		}

		if (f9)  SetPaused(!IsPaused());
		if (f10) { Clear(); g_hasSelection = false; g_autoScroll = true; }
		if (escape && g_visible) g_visible = false;
		if (middle && g_visible) g_visible = false;

		// NOTE: toggling g_visible here, before NewFrame(), is deliberate - the
		// panel is then drawn (or not) in the same frame, so Insert feels
		// instant rather than one frame late.
		//
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

void SaveSettings()
{
	SaveOverlaySettings();
	g_settingsDirty = false;
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

	// First frame: honour the "overlay" setting from packet_log.ini and any
	// saved panel/filter state from overlay.ini.
	if (!g_visibleInitialised)
	{
		g_visibleInitialised = true;
		g_visible = PacketCapture::OverlayEnabledAtStartup();
		LoadOverlaySettings();
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
