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

	// A "reference" snapshot of the body, kept so the decoded view can show
	// which fields moved since it was taken. This is how an unknown field
	// gets identified: capture a jump, mark it as the reference, jump
	// somewhere else, and the fields that changed are the position fields.
	bool     g_diffHasRef = false;
	uint16_t g_diffRefId = 0;
	uint8_t  g_diffBody[PacketSend::kMaxBodyBytes] = {};
	int      g_diffBodyBytes = 0;

	// Jump speed: the number of milliseconds of lead to add to the packet's
	// timestamp on each successive jump. The reference implementations push
	// the client's own timestamp forward so the server accepts a faster
	// cadence. The field number carrying that timestamp differs per client
	// build, so it is a separate, editable setting rather than a hardcoded
	// index - for the captured 0x0833 action packet it is field 9 (a
	// milliseconds-uptime value: 6666000 in the dump).
	int    g_jumpSpeedMs = 5000;
	int    g_jumpTimestampField = 9;               // protobuf field carrying the client clock
	float  g_jumpRepeatMs = 120.0f;                // spacing between auto-jumps
	float  g_jumpNextTime = 0.0f;
	bool   g_jumpAutoFire = false;

	// Jump-to-X,Y.
	//
	// The move target lives in 0x0833 CMsgAction, NOT in 0x0898. This was
	// proved by decompiling the client's own sender, FUN_00d94ff5, which
	// stamps *(u16*)(this+6) = 0x833 and stores its arguments at fixed
	// object offsets. Sorting those offsets ascending lines them up with
	// the protobuf fields, and two of the alignments were already known
	// independently (the clock and the sub-type), which pins the rest:
	//
	//   offset   param        field   meaning
	//   +0x42C   arg2  -> f1    character id (1353102 in every capture)
	//   +0x448   arg7  -> f7    target X
	//   +0x44C   arg8  -> f8    target Y
	//   +0x450   arg9  -> f9    timestamp  <-- already verified
	//   +0x45C   arg6  -> f12   action mode <-- already verified
	//   +0x460   arg5  -> f13
	//   +0x464   arg3  -> f14   origin X
	//   +0x46C   arg4  -> f15   origin Y
	//   +0x474   arg10 -> f17
	//   +0x480   arg11 -> f20
	//
	// The capture confirms it directly: across 13 consecutive move packets,
	// f14/f15 of one packet equal f7/f8 of the previous packet in 12 cases.
	// That is exactly a movement action carrying "from (f14,f15) to
	// (f7,f8)". An earlier pass missed this because the character barely
	// moved during that capture, so the values looked like a narrow band
	// rather than coordinates.
	//
	// So a jump is: take a real 0x0833 capture, write the target into f7/f8,
	// write the current position into f14/f15, and send. Every other field
	// stays as captured.
	//
	// Per the packet-editing trap, the rewritten packet must stay internally
	// consistent: the id in body[0..1] must still match g_buildArmedId, and
	// the length is recomputed by the send path. Each field is a varint whose
	// byte-length can change, so all of them go through ReplaceVarint() and
	// are never patched in place.
	uint16_t g_walkMsgId = 0x0833;                 // CMsgAction - carries the move target
	int    g_walkPosFieldX = 7;                    // f7 = target X
	int    g_walkPosFieldY = 8;                    // f8 = target Y
	int    g_walkOriginFieldX = 14;                // f14 = origin X
	int    g_walkOriginFieldY = 15;                // f15 = origin Y
	int    g_walkClockField = 9;                   // f9  = client clock (ms)
	bool   g_walkStampClock = true;                // refresh f9 to now on each move
	int    g_walkX = 0;                            // target, in map cells
	int    g_walkY = 0;
	char   g_walkTargetText[64] = "";              // typed "x,y" for the quick box

	// When on, the X/Y boxes track the character's live position, so the
	// operator never has to load coordinates from a capture. Typing in either
	// box turns it off, so a hand-entered target is not overwritten.
	bool   g_walkFollow = true;

	// Tiles the Map tab's "Jump X+15" button steps east.
	const int kJumpStepX = 15;

	// ---- object viewer -----------------------------------------------------
	//
	// A snapshot-and-diff view of the character object, in the overlay.
	//
	// This exists because the log round trip is the real bottleneck: dump ->
	// copy -> send -> read -> reply is minutes per iteration, and every guess
	// costs one. Snapshotting in the overlay collapses that to a single click:
	// snapshot, walk one step, and the fields that moved light up on screen.
	//
	// Same method as reading the log, minus the waiting.
	bool     g_objViewOpen = false;
	int      g_objViewBase = 0x3C0;      // first offset shown
	int      g_objViewCount = 48;        // dwords shown
	uint32_t g_objSnap[64] = {};         // snapshot values
	bool     g_objSnapValid = false;
	uint32_t g_objSnapObj = 0;

	// Whether to write the accepted position back into the client object.
	//
	// OFF by default, and deliberately so. The write is the only thing here
	// that modifies the client's own state, and an unproven write into a live
	// object is exactly what can drop the connection. Until the axis order is
	// calibrated and a write is observed to be safe, the safe default is to
	// leave the client alone - the server still moves us either way.
	bool g_resyncClient = false;

	// Move verification.
	//
	// A send that returns 0 only means the client accepted the bytes - it
	// says nothing about whether the SERVER did. The only end-to-end proof is
	// the server echoing the new position back, so after each move we watch
	// the 0x0833 receive stream for it and log the verdict either way.
	//
	// Without this, "the character did not move" is indistinguishable from
	// "the move was never accepted", which is exactly the state we were in.
	bool     g_movePending = false;
	int      g_moveFromX = 0, g_moveFromY = 0;
	int      g_moveToX = 0, g_moveToY = 0;
	uint32_t g_moveSentTick = 0;
	uint32_t kMoveVerifyMs = 3000;

	// Where the jump lands is decided here, so the builder tab can show it.
	bool     g_walkHasGoal = false;
	uint16_t g_walkGoalMsgId = 0;
	uint8_t  g_walkGoalBody[PacketSend::kMaxBodyBytes] = {};
	int      g_walkGoalBytes = 0;

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
		int      tagOffset;    // index of the first tag byte within the payload
		int      tagBytes;     // 1, 2, ... (a protobuf tag is itself a varint)
		int      fieldNumber;
		int      wireType;
		int      valueOffset;  // index of the first varint byte (after the tag)
		uint64_t value;        // varint value (wiretype 0 only)
		int      nextOffset;   // where the next field starts
	};

	// Reads one protobuf field, varint wiretype only. The TAG is itself a
	// varint, so field numbers above 15 use two or more tag bytes - e.g.
	// field 17 is "88 01" and field 20 is "A0 01". Treating the tag as a
	// single byte mis-locates the value for every such field, which both
	// mis-decodes the field AND silently corrupts the packet when the value
	// is edited (the write would land on the tag's second byte).
	bool ReadProtoField(const uint8_t* data, int size, int offset, ProtoField& out)
	{
		if (!data || offset < 0 || offset >= size) return false;

		out.tagOffset = offset;

		// Decode the tag varint.
		uint64_t tag = 0;
		int shift = 0;
		int p = offset;
		while (p < size && shift <= 63)
		{
			uint8_t byte = data[p++];
			tag |= (uint64_t)(byte & 0x7F) << shift;
			if (!(byte & 0x80)) break;
			shift += 7;
		}
		if (p >= size && shift > 63) return false;      // unterminated tag
		if (p > offset && (data[p - 1] & 0x80)) return false;  // ran off the end

		out.tagBytes = p - offset;
		out.fieldNumber = (int)(tag >> 3);
		out.wireType = (int)(tag & 0x07);
		out.valueOffset = p;
		out.nextOffset = p;

		if (out.fieldNumber <= 0) return false;         // 0 is not a valid field
		if (out.wireType != 0) return false;            // only varints decoded

		// Decode the value varint.
		uint64_t value = 0;
		shift = 0;
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
		return false;   // unterminated value
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
	// the existing value varint (AFTER the tag, which may be multi-byte). The
	// new varint is written there and the remainder of the buffer is shifted
	// to fit. Returns the new body length, or `bodyLength` unchanged when the
	// field cannot fit.
	int ReplaceVarint(uint8_t* body, int bodyLength, int valueOffset,
		uint64_t value, int maxBytes)
	{
		if (!body || valueOffset <= 0 || valueOffset >= bodyLength) return bodyLength;

		// Measure the existing value varint: bytes until one without bit 7.
		int oldValueBytes = 0;
		int p = valueOffset;
		while (p < bodyLength)
		{
			++oldValueBytes;
			if (!(body[p] & 0x80)) break;
			++p;
		}
		if (oldValueBytes == 0) return bodyLength;

		const int newValueBytes = VarintSize(value);
		const int delta = newValueBytes - oldValueBytes;
		const int newLength = bodyLength + delta;

		if (newLength > maxBytes || newLength < 0) return bodyLength;

		if (delta != 0)
			memmove(body + valueOffset + newValueBytes,
				body + valueOffset + oldValueBytes,
				(size_t)(bodyLength - (valueOffset + oldValueBytes)));

		WriteVarint(body, valueOffset, maxBytes, value);
		return newLength;
	}

	// -----------------------------------------------------------------------
	// Field editing (jump to x,y)
	// -----------------------------------------------------------------------

	// Defined below, beside the rest of the builder. Declared here because
	// BuildJumpGoal() reloads the template before stamping the target.
	bool LoadBuilderTemplate(uint16_t messageId, bool keepEdits);

	// Sets one varint field of the loaded body to `value`, resizing the body
	// when the new varint is a different width. Returns the new body length,
	// or -1 when the field is absent or the result would not fit.
	//
	// Absent is a hard failure on purpose: 0x0833 omits fields it does not
	// set, and silently inventing one would change the message shape the
	// server sees.
	int SetFieldValue(uint8_t* body, int bodyBytes, int fieldNumber, uint64_t value)
	{
		if (!body || bodyBytes <= 2 || fieldNumber <= 0) return -1;

		int offset = 2;                       // body[0..1] is the message id
		while (offset < bodyBytes)
		{
			ProtoField field;
			if (!ReadProtoField(body, bodyBytes, offset, field)) return -1;

			if (field.fieldNumber == fieldNumber)
			{
				if (field.value == value) return bodyBytes;   // nothing to do

				const int newLength = ReplaceVarint(body, bodyBytes,
					field.valueOffset, value, PacketSend::kMaxBodyBytes);
				if (newLength == bodyBytes) return -1;        // could not fit
				return newLength;
			}
			offset = field.nextOffset;
		}
		return -1;                            // field not present
	}

	// Reads one varint field, or false when it is not present.
	bool GetFieldValue(const uint8_t* body, int bodyBytes, int fieldNumber, uint64_t& out)
	{
		if (!body || bodyBytes <= 2 || fieldNumber <= 0) return false;

		int offset = 2;
		while (offset < bodyBytes)
		{
			ProtoField field;
			if (!ReadProtoField(body, bodyBytes, offset, field)) return false;
			if (field.fieldNumber == fieldNumber) { out = field.value; return true; }
			offset = field.nextOffset;
		}
		return false;
	}

	// Writes the body's field numbers into `out` as "1,7,8,9,...". Used to
	// explain a failed edit: knowing which fields a template actually has is
	// what tells the operator they picked the wrong shape.
	void ListFieldNumbers(const uint8_t* body, int bodyBytes, char* out, size_t outSize)
	{
		if (!out || outSize == 0) return;
		out[0] = '\0';
		if (!body || bodyBytes <= 2) return;

		int offset = 2;
		size_t used = 0;
		while (offset < bodyBytes)
		{
			ProtoField field;
			if (!ReadProtoField(body, bodyBytes, offset, field)) break;

			char one[16];
			_snprintf_s(one, _TRUNCATE, "%s%d", used ? "," : "", field.fieldNumber);
			const size_t len = strlen(one);
			if (used + len + 1 >= outSize) { strncat_s(out, outSize, ",...", _TRUNCATE); break; }
			strncat_s(out, outSize, one, _TRUNCATE);
			used += len;

			offset = field.nextOffset;
		}
	}

	// Reads the move target (f7/f8) out of a body, so the UI can seed its
	// boxes from a real capture instead of making the operator guess.
	bool ReadMoveTarget(const uint8_t* body, int bodyBytes,
		int fieldX, int fieldY, int& x, int& y)
	{
		uint64_t vx = 0, vy = 0;
		if (!GetFieldValue(body, bodyBytes, fieldX, vx)) return false;
		if (!GetFieldValue(body, bodyBytes, fieldY, vy)) return false;
		x = (int)vx;
		y = (int)vy;
		return true;
	}

	// Parses a typed coordinate pair. Accepts the separators people actually
	// type - "430,380", "430 380", "(430, 380)", "430;380", "x=430 y=380" -
	// by simply collecting the first two runs of digits and ignoring
	// everything else.
	//
	// Returns false unless BOTH numbers are present. That matters: a typo
	// must not silently become a move to (0,0) or to (430, 0), so a partial
	// parse is treated as no parse at all.
	bool ParseCellText(const char* text, int& x, int& y)
	{
		if (!text) return false;

		int nums[2] = { 0, 0 };
		int found = 0;
		const char* p = text;

		while (*p && found < 2)
		{
			if (*p >= '0' && *p <= '9')
			{
				int v = 0;
				while (*p >= '0' && *p <= '9')
				{
					v = v * 10 + (*p - '0');
					if (v > 100000) v = 100000;   // clamp rather than overflow
					++p;
				}
				nums[found++] = v;
			}
			else
			{
				++p;
			}
		}

		if (found < 2) return false;

		x = nums[0];
		y = nums[1];
		return true;
	}

	// The direction a step faces, as f13.
	//
	// Derived from every real move in the packet log - 18 of 18 matched - so
	// this is the client's own rule, not a guess. It is the DOMINANT AXIS of
	// (target - origin), not the octant: a step of (+3,+10) is South, not
	// South-East.
	//
	//     0 = S (+y)    2 = W (-x)    4 = N (-y)    6 = E (+x)
	//
	// The client never sent 1/3/5/7 for a move, which is consistent with
	// walking being axis-aligned.
	//
	// This matters because the packet carries the facing as a field. Copying
	// the previous move's direction - which is what the synthesis did at
	// first - means a step in a new direction declares the wrong facing, and
	// the server can refuse the move.
	int DirectionFromDelta(int dx, int dy)
	{
		if (dx == 0 && dy == 0) return 0;

		const int adx = (dx < 0) ? -dx : dx;
		const int ady = (dy < 0) ? -dy : dy;

		if (ady >= adx) return (dy > 0) ? 0 : 4;   // south / north
		return (dx > 0) ? 6 : 2;                   // east / west
	}

	// The hero object, read from the client's own singleton global.
	//
	// The accessor at 0x0043e561 shows it plainly:
	//     CMP  [0x01a64560], 0
	//     JNZ  +
	//     CALL <create>
	//     MOV  EAX, [0x01a64560]
	//     RET
	// so 0x01A64560 holds the hero pointer, created on first use. Reading it
	// directly means the position is available even if the ROLE hook has not
	// fired - no detour required.
	const uintptr_t kRvaHeroGlobal = 0x01664560u;   // absolute 0x01A64560

	uint32_t GetHeroObject()
	{
		__try
		{
			const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
			if (!base) return 0;
			return *(const uint32_t*)(base + kRvaHeroGlobal);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	// The object to read the position from: the hook's if it has fired,
	// otherwise the global. Both are the same object; the global is simply
	// available sooner.
	uint32_t CurrentRoleObject()
	{
		PacketCapture::RoleProbe rp;
		if (PacketCapture::GetRoleProbe(rp) && rp.self) return rp.self;
		return GetHeroObject();
	}

	// Reads dwords out of the role object.
	//
	// Guarded because the pointer comes from a hook: if the object is freed or
	// the offset is wrong, this must fail quietly rather than take the client
	// down inside the render loop.
	bool ReadRoleWords(uint32_t self, int base, uint32_t* out, int count)
	{
		if (!self || !out || count <= 0) return false;

		__try
		{
			const uint8_t* p = (const uint8_t*)(uintptr_t)self;
			for (int i = 0; i < count; ++i)
				out[i] = *(const uint32_t*)(p + base + i * 4);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Decodes one coordinate out of the role object.
	//
	// The client does not store the position plainly. The role Process
	// function computes it as:
	//
	//     ptr   = *(p + offA) ^ *(p + offK)      // an XOR-encoded pointer
	//     value = *ptr ^ *(p + offK)             // value XORed with the key
	//
	// i.e. two dwords hold (encoded pointer, key), and the coordinate lives at
	// the address they decode to, still XORed with the key. That is why the
	// raw window shows nothing that looks like a coordinate - it is not
	// stored as one.
	//
	// A zero first half means "no value", which the client represents as
	// 0xFFFFFFFF rather than reading through a null pointer.
	//
	// Guarded: the decoded address is arbitrary, so a wrong offset must fail
	// quietly rather than fault inside the render loop.
	bool DecodeCoord(uint32_t self, int offA, int offK, uint32_t& out)
	{
		out = 0xFFFFFFFFu;
		if (!self) return false;

		__try
		{
			const uint8_t* p = (const uint8_t*)(uintptr_t)self;
			const uint32_t a = *(const uint32_t*)(p + offA);
			const uint32_t k = *(const uint32_t*)(p + offK);

			if (a == 0) return true;                  // no value
			out = *(const uint32_t*)(uintptr_t)(a ^ k) ^ k;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Writes a coordinate back through the same encoding DecodeCoord reads.
	//
	// The encoded pointer and its key are left exactly as they were; only the
	// dword they point at changes, re-XORed with the same key. The client's
	// own reader therefore keeps working - this is the inverse of the read,
	// not a second format to keep in sync.
	//
	//     *(*(p + offA) ^ *(p + offK)) = value ^ *(p + offK)
	//
	// Guarded like the read: the address is decoded from object contents, so a
	// wrong offset must fail quietly rather than fault.
	bool EncodeCoord(uint32_t self, int offA, int offK, uint32_t value)
	{
		if (!self) return false;

		__try
		{
			const uint8_t* p = (const uint8_t*)(uintptr_t)self;
			const uint32_t a = *(const uint32_t*)(p + offA);
			const uint32_t k = *(const uint32_t*)(p + offK);

			if (a == 0) return false;                 // no storage to write
			*(uint32_t*)(uintptr_t)(a ^ k) = value ^ k;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Which decoded pair is X and which is Y.
	//
	// The Process function passes (+0x468,+0x46C) and (+0x474,+0x478) to the
	// sender, but the decompiler's argument order is not trustworthy and
	// guessing would silently mirror every move. So it is CALIBRATED: the
	// first time a decoded pair and a real move are both available, whichever
	// ordering matches the true coordinates wins.
	//
	//   0 = unknown, 1 = 468-pair is X, 2 = 474-pair is X
	//
	// Defaults to 1 because the disassembly PROVED it, not because it was
	// assumed. In FUN_00ed2e14 the (+0x468,+0x46C) pair is decoded into EDX
	// and EDX is then pushed as param_7, which is the target X; the
	// (+0x474,+0x478) pair lands in ESI and is pushed as param_8, the target
	// Y. The calibration below is kept as a safety net in case a different
	// client build orders them the other way.
	int g_axisOrder = 1;

	void CalibrateAxes(uint32_t self, int trueX, int trueY)
	{
		if (g_axisOrder != 0) return;

		uint32_t c1 = 0, c2 = 0;
		if (!DecodeCoord(self, 0x468, 0x46C, c1)) return;
		if (!DecodeCoord(self, 0x474, 0x478, c2)) return;

		if ((int)c1 == trueX && (int)c2 == trueY) g_axisOrder = 1;
		else if ((int)c2 == trueX && (int)c1 == trueY) g_axisOrder = 2;

		if (g_axisOrder)
			HookLog("[Role] axis order calibrated: %s pair is X (decoded %u/%u, true %d/%d)",
				g_axisOrder == 1 ? "+468" : "+474", c1, c2, trueX, trueY);
	}

	// Pushes a position into the client's own object so it agrees with what
	// the server has just accepted.
	//
	// The server moves us and broadcasts the result, but a client ignores
	// position updates for its own character - it trusts its own state and
	// keeps re-sending it. Without this the two drift apart, and the next move
	// is sent from a stale origin.
	bool ResyncClientPosition(int x, int y)
	{
		// The global works even when the ROLE hook has not fired.
		const uint32_t self = CurrentRoleObject();
		if (!self) return false;

		// Without a known axis order, writing the wrong way round would mirror
		// the character. Refuse rather than risk it - the caller reports this
		// honestly.
		if (g_axisOrder == 0) return false;

		// READ BEFORE WRITING, and require the read to make sense.
		//
		// This is the guard that matters. A successful read of a plausible
		// coordinate proves the decoded pointer is a real address holding a
		// real value - which is precisely the precondition for writing it
		// back. Without it, a bad `a ^ k` lands somewhere real-but-wrong and
		// corrupts state instead of faulting, which is what drops a
		// connection. __try/__except cannot catch that; this can.
		uint32_t curX = 0, curY = 0;
		if (!DecodeCoord(self, 0x468, 0x46C, curX)) return false;
		if (!DecodeCoord(self, 0x474, 0x478, curY)) return false;
		if (curX > 4095 || curY > 4095) return false;   // not a map coordinate

		const bool okX = (g_axisOrder == 1)
			? EncodeCoord(self, 0x468, 0x46C, (uint32_t)x)
			: EncodeCoord(self, 0x474, 0x478, (uint32_t)x);
		const bool okY = (g_axisOrder == 1)
			? EncodeCoord(self, 0x474, 0x478, (uint32_t)y)
			: EncodeCoord(self, 0x468, 0x46C, (uint32_t)y);

		return okX && okY;
	}

	// Writes a complete 0x0833 move body from scratch.
	//
	// This replaces editing a captured template, which was the wrong design.
	// The client sends several shapes under the same id and the one that
	// carries a position is rare - 18 of 2401 captures - so "load the 0x0833
	// template" would usually hand back a different action entirely (an
	// 87-byte Lua message with no position fields was the common case). Every
	// field a move needs is now known, so there is nothing to look up.
	//
	// Fields are written in ascending order, which is the order the client
	// itself uses:
	//
	//   33 08          id   0x0833 (the body starts at the id)
	//   08 <id>        f1   character id
	//   38 <x>         f7   target X
	//   40 <y>         f8   target Y
	//   48 <clock>     f9   client clock (ms)
	//   60 89 01       f12  mode 137 = move
	//   68 <dir>       f13  direction
	//   70 <originX>   f14  origin X
	//   78 <originY>   f15  origin Y
	//   88 01 <f17>    f17  (2-byte tag)
	//   A0 01 <f20>    f20  all-ones sentinel (2-byte tag)
	//
	// f13/f17/f20 come from the client's own last move rather than being
	// hardcoded, so a client that changes them does not silently start
	// producing rejected packets.
	//
	// Returns the body length, or 0 if it would not fit.
	int BuildMoveBody(uint8_t* out, int maxBytes,
		const PacketCapture::LastAction& live,
		int x, int y, int originX, int originY, uint32_t clock)
	{
		if (!out || maxBytes < 64) return 0;

		int n = 0;
		out[n++] = 0x33;                 // id 0x0833, little endian
		out[n++] = 0x08;

		out[n++] = 0x08;                 // f1 character id
		n = WriteVarint(out, n, maxBytes, live.id);

		out[n++] = 0x38;                 // f7 target X
		n = WriteVarint(out, n, maxBytes, (uint64_t)x);

		out[n++] = 0x40;                 // f8 target Y
		n = WriteVarint(out, n, maxBytes, (uint64_t)y);

		out[n++] = 0x48;                 // f9 client clock
		n = WriteVarint(out, n, maxBytes, clock);

		out[n++] = 0x60;                 // f12 mode
		n = WriteVarint(out, n, maxBytes, 137);

		// f13 is COMPUTED from this step, not copied from the previous move.
		// A step in a new direction must declare the facing that goes with
		// it; carrying the last move's facing is what got a move rejected.
		out[n++] = 0x68;                 // f13 direction
		n = WriteVarint(out, n, maxBytes,
			(uint64_t)DirectionFromDelta(x - originX, y - originY));

		out[n++] = 0x70;                 // f14 origin X
		n = WriteVarint(out, n, maxBytes, (uint64_t)originX);

		out[n++] = 0x78;                 // f15 origin Y
		n = WriteVarint(out, n, maxBytes, (uint64_t)originY);

		out[n++] = 0x88; out[n++] = 0x01;   // f17, two-byte tag
		n = WriteVarint(out, n, maxBytes, (uint64_t)(uint32_t)live.f17);

		out[n++] = 0xA0; out[n++] = 0x01;   // f20, two-byte tag
		n = WriteVarint(out, n, maxBytes, (uint64_t)(int64_t)live.f20raw);

		return (n <= maxBytes) ? n : 0;
	}

	// Builds the jump body: synthesises a 0x0833 move, and stores the result.
	//
	// A 0x0833 move carries BOTH ends: f14/f15 is where the move started and
	// f7/f8 is where it ends. Writing only the target would leave the origin
	// describing wherever the capture was taken, so the pair would claim a
	// step the server can see never happened. The capture's own f7/f8 is the
	// last known position, so it becomes the origin.
	bool BuildJumpGoal(int x, int y)
	{
		// Everything comes from the client's own last move. Without one there
		// is no id, direction or f17 to build with, and guessing them would
		// produce a packet the server rejects for reasons we cannot see.
		PacketCapture::LastAction live;
		if (!PacketCapture::GetLastAction(live)) return false;

		// The clock is aged forward to now. A replayed capture carries the
		// timestamp from whenever it was recorded - potentially minutes old -
		// and a stale timestamp reads as out-of-order traffic.
		//
		// Only the hook's clock is used. Guessing the epoch (GetTickCount, a
		// login timer, ...) would be a guess; this value is observed.
		const uint32_t clock = live.clock
			+ (uint32_t)(GetTickCount() - live.capturedTick)
			+ (uint32_t)(g_walkStampClock ? g_jumpSpeedMs : 0);

		// The origin is where the character actually is, which is where its
		// last move ended.
		const int originX = live.targetX;
		const int originY = live.targetY;

		const int n = BuildMoveBody(g_buildBody, PacketSend::kMaxBodyBytes,
			live, x, y, originX, originY, clock);
		if (n <= 2) return false;
		g_buildBodyBytes = n;

		// Latch the id so the send path's consistency guard is satisfied.
		g_buildArmedId = g_walkMsgId;

		const int total = g_buildBodyBytes;
		if (total <= 0 || total > PacketSend::kMaxBodyBytes) return false;
		memcpy(g_walkGoalBody, g_buildBody, (size_t)total);
		g_walkGoalBytes = total;
		g_walkGoalMsgId = g_walkMsgId;
		g_walkHasGoal = true;
		g_buildRawDirty = true;
		return true;
	}

	// -----------------------------------------------------------------------
	// Packet builder - load / send
	// -----------------------------------------------------------------------

	// Pulls the captured template for `messageId` into the editable state.
	// Returns true when a capture was found.
	//
	// `keepEdits` only makes sense when it is the SAME id already loaded -
	// keeping the edits while switching id would leave the body carrying one
	// id and the slot holding another class's vtable, which SendSlot refuses
	// with -2. So a differing id always overwrites the body.
	bool LoadBuilderTemplate(uint16_t messageId, bool keepEdits)
	{
		PacketSend::Slot slot;
		if (!PacketSend::GetSlot(messageId, slot)) return false;

		const bool sameId = g_buildHasTemplate && g_buildArmedId == messageId;
		const bool keep = keepEdits && sameId && g_buildBodyBytes > 0;

		g_buildArmedId = messageId;
		g_buildHasTemplate = true;

		if (!keep)
		{
			int n = slot.bodyBytes;
			if (n > PacketSend::kMaxBodyBytes) n = PacketSend::kMaxBodyBytes;
			if (n < 2) n = 2;                       // a body always holds an id
			memcpy(g_buildBody, slot.body, (size_t)n);
			g_buildBodyBytes = n;
			g_buildRawDirty = true;
		}

		// The first protobuf field sits at body+2 (body[0..1] is the id).
		// Surface it as the "field 1" hint when there is one.
		ProtoField first;
		if (ReadProtoField(g_buildBody, g_buildBodyBytes, 2, first) && first.fieldNumber == 1)
			g_buildMode = (uint16_t)first.value;

		return true;
	}

	// Loads the builder body from a captured packet's RAW BYTES, instead of
	// from the slot.
	//
	// This exists because the slot keeps only the LATEST message for an id.
	// A client that sends several different 0x0833 actions overwrites the one
	// you want with the next one: walk a step, stop, and the "stop" packet
	// replaces the "walk" packet. 0x0833 comes in four shapes in practice
	// (16 / 20 / 25 / 42 bytes) and only the 42-byte one carries the position
	// fields, so reloading from the slot frequently lands on a shape with no
	// f7/f8 - which is exactly the "Fields 7/8 missing" report.
	//
	// `packet` is the full wire packet: [u16 size][u16 id][payload]. The
	// builder's body starts at the id, so it is packet+2 for size-2 bytes.
	//
	// The vtable still comes from the slot at send time. A vtable is
	// per-class, not per-instance, so any 0x0833 body is valid with the
	// 0x0833 vtable - but the class must have been seen at least once, which
	// is what the slot lookup below checks.
	bool LoadBuilderFromBytes(const uint8_t* packet, int packetBytes)
	{
		if (!packet || packetBytes < 6) return false;

		const uint16_t declared = (uint16_t)(packet[0] | (packet[1] << 8));
		if (declared < 4 || declared > PacketSend::kMaxMessageBytes) return false;
		if ((int)declared > packetBytes) return false;   // the ring truncated it

		const uint16_t messageId = (uint16_t)(packet[2] | (packet[3] << 8));

		// The send path refuses a body whose id disagrees with the armed id,
		// and it needs a vtable for that class. Both come from the slot, so
		// require one to exist rather than failing later at send time.
		PacketSend::Slot slot;
		if (!PacketSend::GetSlot(messageId, slot)) return false;

		const int bodyBytes = (int)declared - 2;         // id + payload
		if (bodyBytes < 2 || bodyBytes > PacketSend::kMaxBodyBytes) return false;

		memcpy(g_buildBody, packet + 2, (size_t)bodyBytes);
		g_buildBodyBytes = bodyBytes;
		g_buildArmedId = messageId;
		g_buildHasTemplate = true;
		g_buildRawDirty = true;

		ProtoField first;
		if (ReadProtoField(g_buildBody, g_buildBodyBytes, 2, first) && first.fieldNumber == 1)
			g_buildMode = (uint16_t)first.value;

		return true;
	}

	// Issues one send of the current body through PacketSend. The caller
	// drives repetition (see g_buildPendingSends).
	void SendBuilderOnce()
	{
		int result = PacketSend::SendSlot(g_buildArmedId,
			g_buildBody, g_buildBodyBytes);

		g_buildFlashUntil = (float)ImGui::GetTime() + 3.0f;
		g_buildFlashError = (result != 0);
		if (result == 0)
			_snprintf_s(g_buildFlashText, _TRUNCATE, "Sent 0x%04X (%d bytes)",
				(unsigned)g_buildArmedId, g_buildBodyBytes);
		else if (result == -2)
			_snprintf_s(g_buildFlashText, _TRUNCATE,
				"Body id 0x%04X != armed 0x%04X - reload the template",
				(unsigned)(g_buildBodyBytes >= 2 ? (g_buildBody[0] | (g_buildBody[1] << 8)) : 0),
				(unsigned)g_buildArmedId);
		else if (result < 0)
			_snprintf_s(g_buildFlashText, _TRUNCATE,
				"Refused 0x%04X - no capture yet? (jump in-game first)", (unsigned)g_buildArmedId);
		else
			_snprintf_s(g_buildFlashText, _TRUNCATE,
				"0x%04X: socket send error (%d)", (unsigned)g_buildArmedId, result);
	}

	// Advances the "send N times" pacer. Called every frame from the panel.
	//
	// Even with no gap the batch is spread over frames: each send is a
	// blocking socket write, so firing a whole 999-send batch in one frame
	// would stall the render thread (and the game with it). One send per
	// frame is still 60+ per second, which is as fast as the un-paced case
	// was ever going to be useful.
	void TickBuilderPacer()
	{
		if (g_buildPendingSends <= 0) return;

		const float now = (float)ImGui::GetTime();
		const float gap = g_buildMinIntervalMs / 1000.0f;

		if (gap <= 0.0f)
		{
			SendBuilderOnce();
			--g_buildPendingSends;
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
	// before each jump it rolls the template's timestamp field forward by
	// g_jumpSpeedMs, so the server keeps seeing a client that is legitimately
	// "ahead" rather than a burst of identical timestamps.
	//
	// The lead is applied to g_jumpTimestampField. When that field is not
	// present (a different packet shape) the auto-fire still runs - it just
	// replays the template verbatim, which is what the manual Send does.
	void AdvanceTimestampField(int fieldNumber, uint64_t delta);   // defined below

	void TickJumpAutoFire()
	{
		if (!g_jumpAutoFire) return;
		if (!g_buildHasTemplate) return;

		const float now = (float)ImGui::GetTime();
		if (now < g_jumpNextTime) return;

		if (g_jumpSpeedMs > 0 && g_jumpTimestampField > 0)
			AdvanceTimestampField(g_jumpTimestampField, (uint64_t)g_jumpSpeedMs);

		SendBuilderOnce();
		g_jumpNextTime = now + (g_jumpRepeatMs / 1000.0f);
	}

	// Finds `fieldNumber` in the live body and adds `delta` to its varint
	// value, re-encoding in place. Silent when the field is absent or the
	// edit does not fit - the caller is the auto-fire loop, which must not
	// stall or spam on a shape it did not expect.
	void AdvanceTimestampField(int fieldNumber, uint64_t delta)
	{
		if (fieldNumber <= 0 || g_buildBodyBytes <= 2) return;

		int offset = 2;                         // body[0..1] is the id
		const int end = g_buildBodyBytes;

		while (offset < end)
		{
			ProtoField field;
			if (!ReadProtoField(g_buildBody, end, offset, field)) return;
			if (field.fieldNumber == fieldNumber)
			{
				// Guard the add: some fields are all-ones sentinels
				// (field 20 in the captured packet), and rolling one
				// forward would silently turn it into a small number.
				if (field.value > 0xFFFFFFFFFFFFFFFFull - delta) return;

				int newLength = ReplaceVarint(g_buildBody, g_buildBodyBytes,
					field.valueOffset, field.value + delta, PacketSend::kMaxBodyBytes);
				if (newLength != g_buildBodyBytes)
				{
					g_buildBodyBytes = newLength;
					g_buildRawDirty = true;
				}
				return;
			}
			offset = field.nextOffset;
		}
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
	// Defined with the packet builder further down. Declared here because the
	// Map tab's "Jump X+15" button runs a move, and this tab is drawn well
	// before the builder in the file.
	void DoMoveToTarget();

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

			// The Hero Pos line used to be a hardcoded placeholder. Show the
			// real position when the ACTION hook has seen a move, and say so
			// honestly when it has not rather than printing a made-up pair.
			{
				PacketCapture::LastAction live;
				if (PacketCapture::GetLastAction(live))
					ImGui::Text("Hero Pos: (%d, %d)  dir %d", live.targetX, live.targetY, live.dir);
				else
					ImGui::TextDisabled("Hero Pos: unknown - move once");
			}

			// Quick step: move 5 tiles east of wherever the character is.
			//
			// The starting point comes from the ACTION hook, so this is
			// relative to the character's real position rather than to a
			// stale capture - pressing it repeatedly walks east 5 at a time.
			if (ImGui::Button("Jump X+15"))
			{
				PacketCapture::LastAction live;
				if (PacketCapture::GetLastAction(live))
				{
					g_walkX = live.targetX + kJumpStepX;
					g_walkY = live.targetY;
				}
				else
				{
					// No live fix yet - step from whatever is in the box so
					// the button still does something predictable.
					g_walkX += kJumpStepX;
				}
				DoMoveToTarget();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("-> (%d, %d)", g_walkX, g_walkY);

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
				ImGui::SameLine();
				// The lead is added to this protobuf field of the template -
				// the one carrying the client clock. 0 disables the lead and
				// replays the capture byte-for-byte.
				ImGui::SetNextItemWidth(60.0f);
				ImGui::InputInt("lead field ##jump", &g_jumpTimestampField, 1, 5);
				if (g_jumpTimestampField < 0) g_jumpTimestampField = 0;
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
				// Caption() is a single-argument helper, so the count is
				// formatted first rather than passed as a format string.
				char sentNote[48];
				_snprintf_s(sentNote, _TRUNCATE, "sent %ld", PacketSend::TotalSent());
				Caption(sentNote);
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

			// ---- position readout, under the preview ----------------------
			//
			// Read from the character object rather than from traffic, so it
			// is correct from the moment you log in - standing still included.
			ImGui::Spacing();
			{
				const uint32_t obj = CurrentRoleObject();
				uint32_t px = 0xFFFFFFFFu, py = 0xFFFFFFFFu;
				const bool okX = DecodeCoord(obj, 0x468, 0x46C, px);
				const bool okY = DecodeCoord(obj, 0x474, 0x478, py);
				const bool sane = obj && okX && okY && px <= 4095 && py <= 4095;

				if (sane)
				{
					ImGui::Text("Position:  X %u   Y %u", px, py);
					ImGui::SameLine();
					ImGui::TextDisabled("(read from memory)");
				}
				else if (!obj)
				{
					ImGui::TextDisabled("Position: character object not found yet");
				}
				else
				{
					// Say which half failed. "unreadable" on its own would
					// hide whether it is the pointer or the decode.
					ImGui::TextDisabled("Position: unreadable - obj 0x%08X, X %s, Y %s",
						obj, okX ? "ok" : "failed", okY ? "ok" : "failed");
				}

				// Keep the move boxes in step, so the two never disagree.
				if (g_walkFollow && sane)
				{
					g_walkX = (int)px;
					g_walkY = (int)py;
				}

				// ---- object viewer ----------------------------------------
				//
				// Snapshot the object, walk one step, and whatever changed is
				// the field we are looking for. This is the same method as
				// diffing the log, except the answer appears on screen in one
				// click instead of after a rebuild-and-report round trip.
				ImGui::Spacing();
				if (ImGui::CollapsingHeader("Object viewer - find a field"))
				{
					if (!obj)
					{
						ImGui::TextDisabled("no character object");
					}
					else
					{
						ImGui::SetNextItemWidth(70.0f);
						ImGui::InputInt("base##obj", &g_objViewBase, 4, 16,
							ImGuiInputTextFlags_CharsHexadecimal);
						if (g_objViewBase < 0) g_objViewBase = 0;

						ImGui::SameLine();
						ImGui::SetNextItemWidth(60.0f);
						ImGui::InputInt("count##obj", &g_objViewCount, 4, 16);
						if (g_objViewCount < 4) g_objViewCount = 4;
						if (g_objViewCount > 64) g_objViewCount = 64;

						ImGui::SameLine();
						if (ImGui::Button("Snapshot"))
						{
							if (ReadRoleWords(obj, g_objViewBase, g_objSnap, g_objViewCount))
							{
								g_objSnapValid = true;
								g_objSnapObj = obj;
							}
						}
						ImGui::SameLine();
						if (ImGui::Button("Clear")) g_objSnapValid = false;

						ImGui::SameLine();
						ImGui::TextDisabled(g_objSnapValid
							? "now move one step - changed fields glow"
							: "snapshot, then move one step");

						uint32_t cur[64];
						if (ReadRoleWords(obj, g_objViewBase, cur, g_objViewCount))
						{
							ImGui::BeginChild("##objwin", ImVec2(0.0f, 150.0f), true);
							for (int i = 0; i < g_objViewCount; ++i)
							{
								const bool changed = g_objSnapValid
									&& g_objSnapObj == obj
									&& cur[i] != g_objSnap[i];

								char line[64];
								_snprintf_s(line, _TRUNCATE, "+%03X  %08X",
									g_objViewBase + i * 4, cur[i]);

								if (changed)
									ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.20f, 1.0f),
										"%s   was %08X", line, g_objSnap[i]);
								else
									ImGui::TextDisabled("%s", line);
							}
							ImGui::EndChild();
						}
					}
				}

				// Log here too, not only from the Packets tab.
				//
				// Only the ACTIVE tab's body is drawn, so the probe on the
				// Move row never runs while the Map tab is open - which is
				// why the log stayed empty even after it was made
				// unconditional. Whatever the user is looking at has to be
				// the thing that reports.
				static uint32_t lastPosLogTick = 0;
				const uint32_t t = GetTickCount();
				if (t - lastPosLogTick >= 2000)
				{
					lastPosLogTick = t;

					uint32_t a1 = 0, k1 = 0, a2 = 0, k2 = 0;
					ReadRoleWords(obj, 0x468, &a1, 1);
					ReadRoleWords(obj, 0x46C, &k1, 1);
					ReadRoleWords(obj, 0x474, &a2, 1);
					ReadRoleWords(obj, 0x478, &k2, 1);

					PacketCapture::RoleProbe probe;
					const bool hooked = PacketCapture::GetRoleProbe(probe);

					HookLog("[Pos] obj=0x%08X (hook=%s self=0x%08X global=0x%08X) | "
						"A1=%08X K1=%08X A2=%08X K2=%08X | %s -> X=%u Y=%u",
						obj, hooked ? "yes" : "no", hooked ? probe.self : 0,
						GetHeroObject(), a1, k1, a2, k2,
						sane ? "ok" : "NOT SANE", px, py);

					// Wide dump of the object.
					//
					// The decoded pair is only populated once the client has
					// moved, so +0x468 is a copy written for the move message,
					// not the primary position. The primary one has to be
					// somewhere else in this object, and the only way to find
					// it is to look - so dump a window and diff it across a
					// move. The fields that change ARE the position.
					if (obj)
					{
						uint32_t win[32];
						if (ReadRoleWords(obj, 0x3C0, win, 32))
						{
							for (int r = 0; r < 4; ++r)
							{
								HookLog("[Pos] +%03X: %08X %08X %08X %08X  %08X %08X %08X %08X",
									0x3C0 + r * 32,
									win[r * 8 + 0], win[r * 8 + 1], win[r * 8 + 2], win[r * 8 + 3],
									win[r * 8 + 4], win[r * 8 + 5], win[r * 8 + 6], win[r * 8 + 7]);
							}
						}
					}
				}
			}
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
	void DrawJumpToCell();
	void DoMoveToTarget();
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
			const bool parsed = ParseIdFilter(g_buildIdText, value, prefix, hex, digits);
			if (!parsed || prefix || digits == 0)
			{
				// Do not quietly fall back to the default id - the user
				// would be arming something they did not ask for.
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Type an id first, e.g. 0x%04X", (unsigned)PacketSend::kDefaultBuildId);
				g_buildFlashError = true;
				g_buildFlashUntil = (float)ImGui::GetTime() + 3.0f;
			}
			else if (LoadBuilderTemplate((uint16_t)value, false))
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Loaded 0x%04X template (%d bytes, field 1 = %u)",
					(unsigned)value, g_buildBodyBytes, (unsigned)g_buildMode);
				g_buildFlashError = false;
				g_buildFlashUntil = (float)ImGui::GetTime() + 2.5f;
			}
			else
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"No 0x%04X captured yet - perform the action in-game first", (unsigned)value);
				g_buildFlashError = true;
				g_buildFlashUntil = (float)ImGui::GetTime() + 4.0f;
			}
		}

		ImGui::SameLine();
		if (ImGui::Button("Use Selected"))
		{
			// Every failure path reports why - a button that appears to do
			// nothing is worse than one that explains itself.
			if (!g_hasSelection)
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE, "Select a packet in the table first");
				g_buildFlashError = true;
				g_buildFlashUntil = (float)ImGui::GetTime() + 3.0f;
			}
			else
			{
				long index = IndexOfSeq(snapshot, g_selectedSeq);
				if (index < 0)
				{
					_snprintf_s(g_buildFlashText, _TRUNCATE, "That packet has scrolled out of the ring");
					g_buildFlashError = true;
					g_buildFlashUntil = (float)ImGui::GetTime() + 3.0f;
				}
				else
				{
					const Entry& entry = snapshot.ring[index];
					if (entry.direction != DirectionSend)
					{
						_snprintf_s(g_buildFlashText, _TRUNCATE,
							"0x%04X is a RECV packet - only SEND packets can be replayed",
							(unsigned)entry.messageId);
						g_buildFlashError = true;
						g_buildFlashUntil = (float)ImGui::GetTime() + 3.0f;
					}
					// Load THIS row's bytes, not the slot's latest. The whole
					// point of selecting a row is to choose which of several
					// same-id shapes you want; going back to the slot would
					// silently substitute the newest one.
					else if (!LoadBuilderFromBytes(entry.data, entry.storedBytes))
					{
						_snprintf_s(g_buildFlashText, _TRUNCATE,
							"Could not load 0x%04X from that row (truncated or unknown class)",
							(unsigned)entry.messageId);
						g_buildFlashError = true;
						g_buildFlashUntil = (float)ImGui::GetTime() + 4.0f;
					}
					else
					{
						_snprintf_s(g_buildIdText, _TRUNCATE, "0x%04X", (unsigned)entry.messageId);
						_snprintf_s(g_buildFlashText, _TRUNCATE,
							"Loaded 0x%04X from row %u (%d bytes)",
							(unsigned)entry.messageId, entry.seq, g_buildBodyBytes);
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

		// --- field identification ------------------------------------------
		// The coordinate fields cannot be named from a single capture. This
		// row is the tool for finding them: snapshot the current packet as a
		// reference, move in-game, reload, and every field that changed is
		// flagged in the decoded view above.
		ImGui::SameLine();
		if (ImGui::Button("Set Reference"))
		{
			int n = g_buildBodyBytes;
			if (n > PacketSend::kMaxBodyBytes) n = PacketSend::kMaxBodyBytes;
			memcpy(g_diffBody, g_buildBody, (size_t)n);
			g_diffBodyBytes = n;
			g_diffRefId = g_buildArmedId;
			g_diffHasRef = true;
			_snprintf_s(g_buildFlashText, _TRUNCATE,
				"Reference set (0x%04X, %d bytes) - move, then Reload Template",
				(unsigned)g_buildArmedId, n);
			g_buildFlashError = false;
			g_buildFlashUntil = (float)ImGui::GetTime() + 4.0f;
		}

		ImGui::SameLine();
		if (ImGui::Button("Clear Diff"))
		{
			g_diffHasRef = false;
			g_diffBodyBytes = 0;
			_snprintf_s(g_buildFlashText, _TRUNCATE, "Reference cleared");
			g_buildFlashError = false;
			g_buildFlashUntil = (float)ImGui::GetTime() + 2.0f;
		}

		ImGui::SameLine();
		if (g_diffHasRef && g_diffRefId == g_buildArmedId)
			Caption("changed fields shown in the list");
		else if (g_diffHasRef)
			Caption("reference is for a different id");
		else
			Caption("no reference set");

		// --- jump to x, y --------------------------------------------------
		DrawJumpToCell();

		DrawBuilderFlash();
	}

	// Watches for the server's answer to a move and logs the verdict.
	//
	// A send returning 0 means the CLIENT accepted the bytes. It says nothing
	// about the server. The server answers by echoing the position back as a
	// 0x0833 receive, so that echo is the only end-to-end proof. Called once
	// per frame; cheap when nothing is pending.
	void TickMoveVerify()
	{
		if (!g_movePending) return;

		PacketCapture::ServerPos sp;
		if (PacketCapture::GetServerPos(sp) && sp.tick >= g_moveSentTick)
		{
			const bool reached = (sp.x == g_moveToX && sp.y == g_moveToY);

			// Report WHOSE move the server echoed. RecordServerPos records any
			// 0x0833 receive, so without this check a neighbouring player's
			// move looks exactly like our own being accepted - which would
			// make a rejected move look like a success.
			PacketCapture::LastAction who;
			const bool haveWho = PacketCapture::GetLastAction(who);
			const bool ours = haveWho && (sp.id == who.id);

			HookLog("[Move] SERVER SAYS (%d,%d) for character %u (ours: %s); "
				"asked (%d,%d) from (%d,%d) -> %s",
				sp.x, sp.y, sp.id, ours ? "yes" : "NO",
				g_moveToX, g_moveToY, g_moveFromX, g_moveFromY,
				!ours ? "ANOTHER PLAYER - not our move"
					: (reached ? "ACCEPTED" : "different position"));

			// A move by someone else proves nothing about ours. Do not report
			// it as a result - keep waiting for our own echo.
			if (!ours) return;

			g_buildFlashUntil = (float)ImGui::GetTime() + 5.0f;
			g_buildFlashError = !reached;
			if (reached)
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Move worked - server put us at (%d, %d)", sp.x, sp.y);
			else
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Server reports (%d, %d), not (%d, %d)",
					sp.x, sp.y, g_moveToX, g_moveToY);

			g_movePending = false;
			return;
		}

		if ((uint32_t)(GetTickCount() - g_moveSentTick) > kMoveVerifyMs)
		{
			HookLog("[Move] NO SERVER ECHO within %ums - the move was rejected. "
				"Asked for (%d,%d) from (%d,%d).",
				kMoveVerifyMs, g_moveToX, g_moveToY, g_moveFromX, g_moveFromY);

			g_buildFlashUntil = (float)ImGui::GetTime() + 6.0f;
			g_buildFlashError = true;
			_snprintf_s(g_buildFlashText, _TRUNCATE,
				"No server echo within %ums - move rejected", kMoveVerifyMs);
			g_movePending = false;
		}
	}

	// Runs one move to (g_walkX, g_walkY) and reports the outcome.
	//
	// Shared by the Move button and the coordinate text box so both paths
	// behave identically - in particular both go through the same
	// BuildJumpGoal, which is what stamps the origin as well as the target.
	void DoMoveToTarget()
	{
		// Log the attempt first, before anything can fail. Every later line is
		// conditional, so without this a refused move is invisible in the log
		// and "did not move" cannot be told from "never tried".
		{
			PacketCapture::LastAction probe;
			const bool haveLive = PacketCapture::GetLastAction(probe);
			HookLog("[Move] pressed - target (%d,%d), armed 0x%04X, template %d bytes, "
				"live action %s%s",
				g_walkX, g_walkY, (unsigned)g_walkMsgId, g_buildBodyBytes,
				haveLive ? "yes" : "no",
				haveLive ? "" : " (no move seen yet - the clock and origin fall back "
					"to the capture)");
		}

		// Remember where we started. Without this the verifier could only say
		// "some position changed", not "we reached the target we asked for".
		PacketCapture::LastAction live;
		if (PacketCapture::GetLastAction(live))
		{
			g_moveFromX = live.targetX;
			g_moveFromY = live.targetY;

			// Only this character's moves count as an answer to our move.
			// Everyone else on the map is moving too, and their broadcasts
			// would otherwise be read as our own success.
			PacketCapture::WatchCharacter(live.id);

			// A real move is the one moment the true coordinates are known,
			// which is what settles which decoded pair is X. Cheap and only
			// does anything the first time.
			PacketCapture::RoleProbe rp;
			if (PacketCapture::GetRoleProbe(rp))
				CalibrateAxes(rp.self, live.targetX, live.targetY);
		}
		else
		{
			// No live fix - fall back to the box, which is at least what the
			// operator is looking at.
			g_moveFromX = g_walkX;
			g_moveFromY = g_walkY;
		}
		g_moveToX = g_walkX;
		g_moveToY = g_walkY;

		if (BuildJumpGoal(g_walkX, g_walkY))
		{
			const int result = PacketSend::SendSlot(g_walkGoalMsgId,
				g_walkGoalBody, g_walkGoalBytes);

			g_buildFlashUntil = (float)ImGui::GetTime() + 3.0f;
			g_buildFlashError = (result != 0);
			if (result == 0)
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Sent (%d,%d) -> (%d,%d), checking the server...",
					g_moveFromX, g_moveFromY, g_moveToX, g_moveToY);

				// Arm the verifier. "Sent" is not "worked" - the send path
				// only proves the client accepted the bytes.
				g_movePending = true;
				g_moveSentTick = GetTickCount();

				// Report the direction too: it is computed per step now, and
				// a wrong facing is a plausible cause of a rejection, so it
				// needs to be visible rather than buried in the hex.
				HookLog("[Move] SENT  (%d,%d) -> (%d,%d)  dir %d  %d bytes  id=0x%04X",
					g_moveFromX, g_moveFromY, g_moveToX, g_moveToY,
					DirectionFromDelta(g_moveToX - g_moveFromX, g_moveToY - g_moveFromY),
					g_walkGoalBytes, (unsigned)g_walkGoalMsgId);

				// The server will move us, but the client ignores position
				// updates for its own character, so the two drift apart. The
				// write that fixes that is gated: it modifies live client
				// state, and until it has been proven safe it stays off.
				if (!g_resyncClient)
				{
					HookLog("[Move] client resync disabled (server moved us; "
						"the client will follow on its own)");
				}
				else if (ResyncClientPosition(g_moveToX, g_moveToY))
				{
					HookLog("[Move] client resynced to (%d,%d)", g_moveToX, g_moveToY);
				}
				else
				{
					HookLog("[Move] client NOT resynced (axis order not calibrated "
						"yet - move once by hand so it can be worked out)");
				}
			}
			else
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Move send failed (%d) - see the log", result);
				HookLog("[Move] SEND FAILED (result %d)  target (%d,%d)",
					result, g_moveToX, g_moveToY);
			}
		}
		else
		{
			// Name the fields the template DOES have - that is what tells the
			// operator they loaded the wrong shape, and which shape to go
			// looking for instead.
			// The move is now synthesised from the client's own last action,
			// so the only reasons to fail are "no real move seen yet" (no id,
			// direction or f17 to build with) and "it would not fit".
			PacketCapture::LastAction probe;
			const bool haveLive = PacketCapture::GetLastAction(probe);

			g_buildFlashUntil = (float)ImGui::GetTime() + 5.0f;
			g_buildFlashError = true;

			if (!haveLive)
			{
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"No live move yet - walk once, then press Move");

				// Log it too: a refusal that only touches the panel leaves no
				// trace in the file, so "did not move" and "never tried" look
				// identical when reading the log afterwards.
				HookLog("[Move] REFUSED - no live move observed yet. Walk once so "
					"the id, direction and f17 come from a real move.");
			}
			else
			{
				char have[96];
				ListFieldNumbers(g_buildBody, g_buildBodyBytes, have, sizeof(have));
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Could not build the move (%d bytes)", g_buildBodyBytes);
				HookLog("[Move] REFUSED - could not build the move. Has: %s", have);
			}
		}
	}

	// Move-to-X,Y. Sends a 0x0833 CMsgAction whose target is the cell the
	// operator types, so the character moves there instead of wherever the
	// client chose. See docs/walk-position-fields.md for how f7/f8 and
	// f14/f15 were identified.
	//
	// Everything except those four fields is copied from a real 0x0833
	// capture: the client's own SendSlot path still does the id/vtable/size
	// validation, so the message has to remain a well-formed CMsgAction or it
	// is dropped with "Check Size Failed".
	void DrawJumpToCell()
	{
		ImGui::Separator();
		ImGui::TextDisabled("Move to cell (0x%04X)", (unsigned)g_walkMsgId);
		ImGui::Separator();

		PacketSend::Slot walkSlot;
		if (!PacketSend::GetSlot(g_walkMsgId, walkSlot))
		{
			Caption("No 0x0833 capture yet - move once, then reopen this tab.");
			return;
		}

		// Seed the boxes from the loaded capture the first time, so the
		// operator starts from a real position rather than 0,0. A static is
		// fine here: this panel is only drawn on the single render thread.
		// The boxes track the character's LIVE position.
		//
		// This used to seed them once from the loaded capture, which meant
		// they showed wherever the character stood when that packet was
		// recorded - stale the moment it took a step, and completely wrong
		// after a walk. The ACTION hook sees every move the client makes, so
		// its last target IS the current position.
		//
		// Typing in either box turns the follow off, so a hand-entered
		// target is never silently overwritten.
		PacketCapture::LastAction live;
		const bool haveLive = PacketCapture::GetLastAction(live);

		if (haveLive && g_walkFollow)
		{
			g_walkX = live.targetX;
			g_walkY = live.targetY;
		}

		// --- quick path: type the coordinates ------------------------------
		// One box for "430,380" - also accepts "430 380", "(430,380)",
		// "430;380" or "x=430 y=380". Enter or Go submits. It runs exactly
		// the same move as the button beside it; it just saves reaching for
		// two number fields.
		ImGui::SetNextItemWidth(150.0f);
		const bool submitted = ImGui::InputTextWithHint("##targettext", "x,y",
			g_walkTargetText, sizeof(g_walkTargetText),
			ImGuiInputTextFlags_EnterReturnsTrue);

		const bool canJump = PacketSend::CanSend(g_walkMsgId);

		ImGui::SameLine();
		if (!canJump) ImGui::BeginDisabled();
		const bool goPressed = ImGui::Button("Go");
		if (!canJump) ImGui::EndDisabled();

		if (submitted || goPressed)
		{
			int tx = 0, ty = 0;
			if (ParseCellText(g_walkTargetText, tx, ty))
			{
				g_walkX = tx;
				g_walkY = ty;
				g_walkFollow = false;   // a typed target is not overwritten
				DoMoveToTarget();
			}
			else
			{
				// Half a coordinate pair is not a coordinate pair - say so
				// rather than sending a move to whatever did parse.
				g_buildFlashUntil = (float)ImGui::GetTime() + 4.0f;
				g_buildFlashError = true;
				_snprintf_s(g_buildFlashText, _TRUNCATE,
					"Type two numbers, e.g. 430,380");
			}
		}

		ImGui::SameLine();
		if (!canJump) ImGui::BeginDisabled();
		if (ImGui::Button("Move")) DoMoveToTarget();
		if (!canJump) ImGui::EndDisabled();

		// --- the same target as editable numbers ---------------------------
		// The map runs to roughly 0..1000, so these are not clamped to a byte
		// the way the old packed-cell version was.
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		if (ImGui::InputInt("X##jumpcell", &g_walkX, 1, 10)) g_walkFollow = false;
		if (g_walkX < 0) g_walkX = 0;
		if (g_walkX > 4095) g_walkX = 4095;

		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		if (ImGui::InputInt("Y##jumpcell", &g_walkY, 1, 10)) g_walkFollow = false;
		if (g_walkY < 0) g_walkY = 0;
		if (g_walkY > 4095) g_walkY = 4095;

		// Follow toggle. On by default so the boxes are always the character's
		// real position; off to keep a target you typed.
		ImGui::SameLine();
		ImGui::Checkbox("follow##jumpcell", &g_walkFollow);

		// Off by default. This is the only control here that writes into the
		// live client object, and an unproven write is what can drop the
		// connection - so it is opt-in and clearly labelled.
		ImGui::SameLine();
		ImGui::Checkbox("resync##jumpcell", &g_resyncClient);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Write the accepted position into the client object.\n"
				"Off by default: this modifies live client state and can\n"
				"disconnect if the axis order is wrong.");

		ImGui::SameLine();
		ImGui::SetNextItemWidth(55.0f);
		ImGui::InputInt("Xf##jumpcell", &g_walkPosFieldX, 1, 5);
		if (g_walkPosFieldX < 1) g_walkPosFieldX = 1;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(55.0f);
		ImGui::InputInt("Yf##jumpcell", &g_walkPosFieldY, 1, 5);
		if (g_walkPosFieldY < 1) g_walkPosFieldY = 1;

		ImGui::SameLine();
		if (ImGui::Button("Load Cell"))
		{
			int x = 0, y = 0;
			if (ReadMoveTarget(g_buildBody, g_buildBodyBytes,
				g_walkPosFieldX, g_walkPosFieldY, x, y))
			{
				g_walkX = x;
				g_walkY = y;
			}
		}

		// "stamp origin" used to live here. It is gone because the origin is
		// now ALWAYS the character's live position - there is no captured
		// origin left to choose between, so the toggle had nothing to do.
		ImGui::SameLine();
		ImGui::Checkbox("lead clock##jumpcell", &g_walkStampClock);
		if (g_walkStampClock)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(55.0f);
			ImGui::InputInt("t##jumpcell", &g_walkClockField, 1, 5);
			if (g_walkClockField < 0) g_walkClockField = 0;
		}

		// Show where the packet will claim the character is starting from.
		// `live`/`haveLive` come from the follow block above - this used to
		// re-declare them, which does not compile in the same scope.
		if (haveLive)
			ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.95f, 1.0f),
				"live: from (%d, %d) dir %d  ->  target (%d, %d)",
				live.targetX, live.targetY, live.dir, g_walkX, g_walkY);
		else
		{
			int curX = 0, curY = 0;
			const bool haveCur = ReadMoveTarget(walkSlot.body, walkSlot.bodyBytes,
				g_walkPosFieldX, g_walkPosFieldY, curX, curY);

			if (haveCur)
				ImGui::TextDisabled("capture only: from (%d, %d)  ->  target (%d, %d)",
					curX, curY, g_walkX, g_walkY);
			else
				ImGui::TextDisabled("target (%d, %d)", g_walkX, g_walkY);
		}

		if (!haveLive)
			Caption("No live move seen yet - walk once so the clock and "
				"origin come from the client instead of the capture.");

		// Role probe. The ACTION hook can only report a position after a move;
		// the character object holds it all the time. This shows the object
		// pointer and a window of its dwords, so the position fields can be
		// identified from real values rather than guessed - which is what
		// removes the "walk once" dependency.
		const uint32_t roleObj = CurrentRoleObject();
		if (roleObj)
		{
			PacketCapture::RoleProbe rp;
			const bool fromHook = PacketCapture::GetRoleProbe(rp);
			ImGui::TextDisabled("role @ 0x%08X (%s)", roleObj,
				(fromHook && rp.self == roleObj) ? "hook" : "global");

			// The two coordinate pairs, as the client's own reader decodes
			// them. (+0x468,+0x46C) is X and (+0x474,+0x478) is Y - proved
			// from the disassembly, where the 468 pair is pushed as the
			// sender's target-X argument and the 474 pair as target-Y.
			uint32_t c1 = 0xFFFFFFFFu, c2 = 0xFFFFFFFFu;
			const bool ok1 = DecodeCoord(roleObj, 0x468, 0x46C, c1);
			const bool ok2 = DecodeCoord(roleObj, 0x474, 0x478, c2);

			// Log UNCONDITIONALLY, once a second.
			//
			// This used to be inside the success branch, which made a FAILED
			// decode completely silent - so there was no way to tell "the hook
			// never fired" from "the object pointer is wrong" from "the decode
			// returned nonsense". A diagnostic that only speaks when things
			// work is worse than none. Every input is printed raw.
			{
				static uint32_t lastRoleLogTick = 0;
				const uint32_t nowTick = GetTickCount();
				if (nowTick - lastRoleLogTick >= 1000)
				{
					lastRoleLogTick = nowTick;

					uint32_t a1 = 0, k1 = 0, a2 = 0, k2 = 0;
					ReadRoleWords(roleObj, 0x468, &a1, 1);
					ReadRoleWords(roleObj, 0x46C, &k1, 1);
					ReadRoleWords(roleObj, 0x474, &a2, 1);
					ReadRoleWords(roleObj, 0x478, &k2, 1);

					HookLog("[Role] obj=0x%08X (hook=%s global=0x%08X) | "
						"A1=%08X K1=%08X A2=%08X K2=%08X | decode %s -> X=%u Y=%u | live=%s",
						roleObj, fromHook ? "yes" : "no", GetHeroObject(),
						a1, k1, a2, k2,
						(ok1 && ok2) ? "ok" : "FAILED", c1, c2,
						haveLive ? "yes" : "no");
				}
			}

			if (ok1 && ok2)
			{
				ImGui::TextDisabled("  decoded: %u / %u   axes: %s", c1, c2,
					g_axisOrder == 0 ? "not calibrated"
					: (g_axisOrder == 1 ? "+468 is X" : "+474 is X"));

				// Feed the boxes from memory when there is no live move, which
				// is what removes the "walk once" dependency entirely.
				if (g_walkFollow && !haveLive && c1 <= 4095 && c2 <= 4095)
				{
					g_walkX = (int)c1;
					g_walkY = (int)c2;
				}
			}
			else
				Caption("position fields unreadable");

			// Raw window, kept alongside the decode so a wrong formula is
			// visible rather than hidden behind a plausible-looking number.
			uint32_t words[4];
			if (ReadRoleWords(roleObj, 0x468, words, 4))
				ImGui::TextDisabled("  raw +468: %08X %08X %08X %08X",
					words[0], words[1], words[2], words[3]);
		}
		else
		{
			Caption("role object not seen yet - neither the ROLE hook nor the "
				"hero global produced a pointer.");

			// Log the failure too. Without this the "no object" case is
			// invisible in the file, which is how it stayed unexplained.
			static uint32_t lastNoObjTick = 0;
			const uint32_t t = GetTickCount();
			if (t - lastNoObjTick >= 2000)
			{
				lastNoObjTick = t;
				PacketCapture::RoleProbe probe;
				const bool hooked = PacketCapture::GetRoleProbe(probe);
				HookLog("[Role] NO OBJECT - hook fired=%s (self=0x%08X), "
					"global 0x%08X reads 0x%08X",
					hooked ? "yes" : "no", hooked ? probe.self : 0,
					(unsigned)(0x01A64560u), GetHeroObject());
			}
		}

		if (g_walkHasGoal)
		{
			char goal[96];
			_snprintf_s(goal, _TRUNCATE, "last built: 0x%04X, %d bytes",
				(unsigned)g_walkGoalMsgId, g_walkGoalBytes);
			Caption(goal);
		}
	}

	// What we currently know about each protobuf field. These labels come
	// from diffing real captures (see docs/packet-send.md and
	// docs/walk-position-fields.md) and they are PER MESSAGE ID: 0x0833 and
	// 0x0898 number their fields differently, so one shared table would
	// mislabel both. Field numbers are only meaningful within one id.
	//
	// A label is a hint for the operator, never a promise: an unknown field
	// stays "field N" rather than being guessed at. `isClockLead` marks the
	// one field the auto-jump is allowed to roll forward.
	struct FieldNote
	{
		int         fieldNumber;
		const char* label;
		bool        isClockLead;   // safe to advance by the jump-speed lead
	};

	// 0x0833 CMsgAction - entity action / movement. Field map taken from the
	// client's own sender FUN_00d94ff5, which stamps id 0x833 and stores its
	// arguments at fixed object offsets; sorting those offsets lines them up
	// with the protobuf fields, and f9/f12 were already known independently.
	// See docs/action-protobuf-fields.md.
	//
	//   1  the character id (1353102 in every capture)
	//   7  TARGET X          - one half of the destination
	//   8  TARGET Y
	//   9  client clock (ms) - 32 distinct values, strictly increasing
	//   12 the action mode   - 137 (0x89) is the move/walk mode
	//   13 direction
	//   14 ORIGIN X          - where the move started
	//   15 ORIGIN Y
	//   17 fixed 10364 (2-byte tag).  20 all-ones uint64 sentinel.
	//   3  an entity/object id, not a coordinate
	//
	// f14/f15 of one packet equal f7/f8 of the previous packet in 12 of 13
	// consecutive captures, which is what identifies them as from/to.
	const FieldNote kActionFieldNotes[] =
	{
		{  1, "character id",                false },
		{  3, "entity / object id",          false },
		{  7, "TARGET X",                    false },
		{  8, "TARGET Y",                    false },
		{  9, "client clock (ms)",           true  },
		{ 12, "action mode (137=move)",      false },
		{ 13, "direction",                   false },
		{ 14, "ORIGIN X",                    false },
		{ 15, "ORIGIN Y",                    false },
		{ 17, "constant 10364 (2-byte tag)", false },
		{ 20, "no target (all-ones)",        false },
	};

	// 0x0898 CMsgWalk - client walk request (target cell).
	//   field 4 = (y << 16) | (x << 8) | frac  -- a fixed-point cell; two
	//   capture sets (3 + 8 packets) pinned x and y to real movement.
	//   fields 2, 3 and 5 are constant across every sample (a character
	//   reference and two fixed values), copied verbatim from the capture.
	//   field 1 is the direction / animation id and changes per step.
	const FieldNote kWalkFieldNotes[] =
	{
		{ 1, "direction / animation id",         false },
		{ 2, "character reference (const)",      false },
		{ 3, "fixed",                            false },
		{ 4, "cell: y<<16 | x<<8 | frac",        false },
		{ 5, "fixed",                            false },
	};

	// Returns the note for a field number of `messageId`, or null when it is
	// not known. The table is selected by id so the two layouts never mix.
	const FieldNote* FindFieldNote(uint16_t messageId, int fieldNumber)
	{
		const FieldNote* table = nullptr;
		int count = 0;

		if (messageId == 0x0833)
		{
			table = kActionFieldNotes;
			count = IM_COUNTOF(kActionFieldNotes);
		}
		else if (messageId == 0x0898)
		{
			table = kWalkFieldNotes;
			count = IM_COUNTOF(kWalkFieldNotes);
		}

		for (int i = 0; i < count; ++i)
			if (table[i].fieldNumber == fieldNumber)
				return &table[i];
		return nullptr;
	}

	// The varint fields in the loaded body, as editable inputs. The walk
	// runs field to field and stops at the first byte that is not a varint
	// tag (a non-varint wiretype, or a malformed run). For the captured
	// 0x0833 packet it consumes the whole body; whatever is left over is
	// shown read-only rather than silently dropped.
	//
	// Each row is labelled from kFieldNotes where known, so an operator can
	// see "client clock (ms)" instead of a bare field number. Unknown fields
	// are shown as-is - the labels are derived from real captures, not
	// guessed, and a wrong label is worse than no label.
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
			if (!ReadProtoField(g_buildBody, end, offset, field))
				break;

			anyField = true;

			// A varint can be 64 bits; clamp for the edit widget.
			uint64_t clamped = field.value;
			if (clamped > 0xFFFFFFFFull) clamped = 0xFFFFFFFFull;
			int value32 = (int)clamped;

			const FieldNote* note = FindFieldNote(g_buildArmedId, field.fieldNumber);

			// If a reference is set for this same id, look up the same field
			// in it so the row can show how much it moved. That delta is the
			// signal that identifies an unknown field: on a position change,
			// the coordinate fields are the ones that shift.
			bool  haveDelta = false;
			uint64_t delta = 0;
			if (g_diffHasRef && g_diffRefId == g_buildArmedId)
			{
				int ro = 2;
				while (ro < g_diffBodyBytes)
				{
					ProtoField rf;
					if (!ReadProtoField(g_diffBody, g_diffBodyBytes, ro, rf)) break;
					if (rf.fieldNumber == field.fieldNumber)
					{
						delta = field.value - rf.value;
						haveDelta = true;
						break;
					}
					ro = rf.nextOffset;
				}
			}

			char label[96];
			if (note)
				_snprintf_s(label, _TRUNCATE, "%d  %s", field.fieldNumber, note->label);
			else
				_snprintf_s(label, _TRUNCATE, "%d  (unknown)", field.fieldNumber);

			ImGui::SetNextItemWidth(190.0f);
			ImGui::PushID(field.tagOffset);
			// CharsHexadecimal makes the box read/write hex; the +/- steps
			// are 1 and 16 so the arrows move a nibble and a byte.
			if (ImGui::InputInt(label, &value32, 1, 16, ImGuiInputTextFlags_CharsHexadecimal))
			{
				uint64_t newValue = (uint64_t)(uint32_t)value32;
				if (newValue != field.value)   // only rewrite on an actual change
				{
					int newLength = ReplaceVarint(g_buildBody, g_buildBodyBytes,
						field.valueOffset, newValue, PacketSend::kMaxBodyBytes);
					if (newLength != g_buildBodyBytes)
					{
						g_buildBodyBytes = newLength;
						g_buildRawDirty = true;
						// Every later field's offset just shifted, so stop
						// drawing now and re-walk from the start next frame.
						ImGui::PopID();
						break;
					}
				}
			}
			ImGui::PopID();

			// Right-hand column: the wire offset, or the change since the
			// reference when one is set. A non-zero delta is highlighted
			// because it is the thing the operator is hunting for.
			if (g_buildArmedId == g_walkMsgId
				&& (field.fieldNumber == g_walkPosFieldX
					|| field.fieldNumber == g_walkPosFieldY))
			{
				// One half of the move target. Marking it makes the pair
				// obvious in the list, which matters because f7/f8 look like
				// ordinary small numbers until you know what they are.
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.95f, 1.0f),
					"target %s", field.fieldNumber == g_walkPosFieldX ? "X" : "Y");
			}
			else if (g_buildArmedId == g_walkMsgId
				&& (field.fieldNumber == g_walkOriginFieldX
					|| field.fieldNumber == g_walkOriginFieldY))
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.60f, 1.0f),
					"origin %s", field.fieldNumber == g_walkOriginFieldX ? "X" : "Y");
			}
			else if (field.value > 0xFFFFFFFFull)
			{
				ImGui::SameLine();
				Caption("wide (>32-bit)");
			}
			else if (haveDelta && delta != 0)
			{
				char note2[48];
				_snprintf_s(note2, _TRUNCATE, "changed %+lld",
					(long long)((int64_t)delta));
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.20f, 1.0f), "%s", note2);
			}
			else
			{
				char off[32];
				_snprintf_s(off, _TRUNCATE, "+%d", field.tagOffset);
				ImGui::SameLine();
				Caption(off);
			}

			offset = field.nextOffset;
		}

		if (!anyField)
			Caption("No varint fields decoded.");

		// Anything the field walk could not consume (normally nothing).
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

				// Deliberately do NOT re-target g_buildArmedId from the new
				// first two bytes. The armed id selects the vtable we replay
				// under; silently following an edited id would send the
				// message under a different class's vtable. Instead report
				// the mismatch and let the user re-arm explicitly.
				uint16_t bodyId = (written >= 2)
					? (uint16_t)(g_buildBody[0] | (g_buildBody[1] << 8)) : 0;

				if (bodyId != g_buildArmedId)
				{
					_snprintf_s(g_buildFlashText, _TRUNCATE,
						"Applied %d bytes - body id 0x%04X differs from armed 0x%04X",
						written, (unsigned)bodyId, (unsigned)g_buildArmedId);
					g_buildFlashError = true;
					g_buildFlashUntil = (float)ImGui::GetTime() + 4.0f;
				}
				else
				{
					_snprintf_s(g_buildFlashText, _TRUNCATE, "Applied %d bytes", written);
					g_buildFlashError = false;
					g_buildFlashUntil = (float)ImGui::GetTime() + 2.0f;
				}
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
		TickMoveVerify();

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
