// ============================================================================
// PacketCapture - plaintext packet capture for Conquer.exe
// ----------------------------------------------------------------------------
// Hooks the client's own send/receive functions instead of ws2_32, so the
// buffers observed are plaintext (pre-encryption on send, post-decryption on
// receive). See packet_capture.h for the addresses and their derivation, and
// docs/packet-hooks.md for the analysis that produced them.
//
//   CMyClientSocket::DoSendMsg  @ 0x012799A1   outgoing, plaintext
//   GetMsgType helper           @ 0x00D0C67B   incoming, plaintext
//
// Both are verified against a byte signature before MinHook touches them, so
// a mismatched client build degrades to "hooks not installed" instead of
// crashing the game.
//
// Every captured packet is written to a ring buffer (for the overlay) and,
// unless disabled, appended to packets.log next to the game exe.
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "packet_capture.h"
#include "packet_names.h"
#include "packet_send.h"
#include "log.h"
#include "MinHook.h"

namespace PacketCapture {
namespace {

	// -----------------------------------------------------------------------
	// Static-analysis constants (Conquer.exe build 7952).
	// RVAs are (Ghidra address - image base 0x00400000); using RVAs rather
	// than absolute addresses keeps the hooks correct if the executable is
	// ever linked with a dynamic base.
	// -----------------------------------------------------------------------
	const uintptr_t kRvaDoSendMsg = 0x00E799A1u;   // absolute 0x012799A1
	const uintptr_t kRvaGetMsgType = 0x0090C67Bu;  // absolute 0x00D0C67B
	const uintptr_t kRvaActionSend = 0x00994FF5u;  // absolute 0x00D94FF5
	const uintptr_t kRvaRoleProcess = 0x00AD2E14u; // absolute 0x00ED2E14

	// Prologues, used to confirm the client build matches before hooking.
	//   DoSendMsg : PUSH EBP; MOV EBP,ESP; SUB ESP,0Ch; PUSH EBX; PUSH ESI; PUSH EDI; MOV EDI,[EBP+8]
	const uint8_t kSigDoSendMsg[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08 };
	//   GetMsgType: PUSH EBP; MOV EBP,ESP; MOV EAX,[EBP+8]; MOV AX,[EAX+2]; POP EBP; RET
	const uint8_t kSigGetMsgType[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x66, 0x8B, 0x40, 0x02, 0x5D, 0xC3 };
	//   ActionSend: PUSH EBP; MOV EBP,ESP; PUSH EBX; PUSH ESI; MOV ESI,[EBP+8]; PUSH EDI; MOV EDI,ECX; TEST ESI,ESI
	//   Stopped before the following JZ so no relative offset is baked in.
	const uint8_t kSigActionSend[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0x75, 0x08, 0x57, 0x8B, 0xF9, 0x85, 0xF6 };
	//   RoleProcess: PUSH 0x4C8; MOV EAX,<EH handler>; CALL __EH_prolog3  (the
	//   MSVC EH-prolog pattern, so the immediate is the frame size and stays
	//   stable for this build).
	const uint8_t kSigRoleProcess[] = { 0x68, 0xC8, 0x04, 0x00, 0x00, 0xB8, 0xF9, 0x93, 0x49, 0x01 };

	// Never read more than this from a claimed packet length.
	const uint32_t kHardMaxLength = 2048;

	// DoSendMsgFn is declared in packet_capture.h (PacketSend uses it too);
	// only the receive-side typedef belongs here.
	typedef uint16_t(__cdecl* GetMsgTypeFn)(const uint8_t* packet, int len);

	// The client's own CMsgAction (0x0833) sender. It stamps the wire id and
	// stores each argument into the message object, so hooking it shows
	// exactly what the game sends for a move - the mode and the coordinate
	// pair - which is the ground truth an injected packet has to match.
	//
	// All ten stack arguments are forwarded and logged rather than named
	// individually. The decompiler's argument numbering for a __thiscall
	// with this many stack arguments is off by one against the object
	// stores it performs, so naming them here would assert a mapping that
	// is not trustworthy. The interesting ones are recognisable by VALUE
	// (mode 137, and the two small coordinates).
	typedef int(__fastcall* ActionSendFn)(void* self, void* edx,
		int a2, int a3, int a4, int a5, int a6,
		int a7, int a8, int a9, int a10, int a11);

	// The role Process function. __thiscall, so its `this` is the role object
	// - the pointer needed to read the character's position from memory
	// instead of waiting for it to move.
	typedef void(__fastcall* RoleProcessFn)(void* self, void* edx);

	DoSendMsgFn g_realDoSendMsg = nullptr;
	GetMsgTypeFn g_realGetMsgType = nullptr;
	ActionSendFn g_realActionSend = nullptr;
	RoleProcessFn g_realRoleProcess = nullptr;

	// Written on every Process call, read by the overlay. One pointer store,
	// no memory reads inside the hook: this runs per role per frame, so it has
	// to stay trivial.
	void* volatile g_roleSelf = nullptr;
	uint32_t volatile g_roleSelfTick = 0;

	// Last argument vector logged, so a burst of identical actions collapses
	// to one line instead of flooding the log. A move still logs each step
	// because its coordinates change.
	int   g_lastActionArgs[10] = { 0 };
	bool  g_haveLastActionArgs = false;
	DWORD g_lastActionLogTick = 0;
	const DWORD kActionLogRepeatMs = 1000;

	// The last action, kept for the builder. Guarded by g_lock: the hook runs
	// on the game thread and the overlay reads it from the render thread, and
	// a torn timestamp would be worse than no timestamp.
	LastAction g_lastAction = {};

	// The last position the server reported for a character, parsed out of a
	// 0x0833 RECV. This is what tells us whether an injected move was
	// accepted: the server echoes accepted moves back with the same layout.
	ServerPos g_serverPos = {};

	// When non-zero, only this character's moves are recorded. See
	// WatchCharacter in the header.
	uint32_t g_watchId = 0;

	// The socket the most recent outgoing send travelled on. DoSendMsg is
	// __thiscall, so `self` is the CMyClientSocket*; PacketSend needs it to
	// re-issue a message. Games routinely recreate the socket on reconnect,
	// so it is refreshed on every send rather than cached once.
	void* volatile g_lastSendSocket = nullptr;

	CRITICAL_SECTION g_lock;
	bool g_lockReady = false;
	bool g_installed = false;

	// Separate lock for packets.log: file I/O must never block the renderer,
	// which takes g_lock once per frame to read the ring.
	CRITICAL_SECTION g_fileLock;
	bool g_fileLockReady = false;

	Entry g_ring[kRingSize];
	long g_head = 0;
	long g_count = 0;
	uint32_t g_total = 0;

	volatile long g_paused = 0;
	volatile long g_totalSend = 0;
	volatile long g_totalRecv = 0;
	volatile long g_totalDropped = 0;

	// Minimal varint walker, receive side. The overlay has a fuller one, but
	// it lives in a different translation unit and this only needs the four
	// fields that matter (id, target x/y, mode).
	bool ReadRecvField(const uint8_t* p, int size, int& offset,
		int& fieldNumber, uint64_t& value)
	{
		if (!p || offset < 0 || offset >= size) return false;

		uint64_t tag = 0;
		int shift = 0;
		int i = offset;
		while (i < size && shift <= 63)
		{
			const uint8_t byte = p[i++];
			tag |= (uint64_t)(byte & 0x7F) << shift;
			if (!(byte & 0x80)) break;
			shift += 7;
		}
		if (i > offset && (p[i - 1] & 0x80)) return false;   // ran off the end

		fieldNumber = (int)(tag >> 3);
		if (fieldNumber <= 0) return false;
		if ((tag & 0x07) != 0) return false;                 // varints only

		uint64_t v = 0;
		shift = 0;
		while (i < size && shift <= 63)
		{
			const uint8_t byte = p[i++];
			v |= (uint64_t)(byte & 0x7F) << shift;
			if (!(byte & 0x80))
			{
				value = v;
				offset = i;
				return true;
			}
			shift += 7;
		}
		return false;
	}

	// Pulls id / target x / target y / mode out of a 0x0833 receive and stores
	// it. The wire layout is [u16 len][u16 id][protobuf], so the walk starts
	// at byte 4.
	//
	// Defined after the globals above, and gated on g_lockReady the same way
	// the ring writers are: this can be reached from the receive thread
	// before Install() has initialised the lock.
	void RecordServerPos(const uint8_t* packet, int length)
	{
		if (!packet || length < 8) return;
		if (!g_lockReady) return;

		uint32_t id = 0;
		int x = 0, y = 0, mode = 0;
		bool haveId = false, haveX = false, haveY = false, haveMode = false;

		int offset = 4;
		while (offset < length)
		{
			int f = 0;
			uint64_t v = 0;
			if (!ReadRecvField(packet, length, offset, f, v)) break;

			switch (f)
			{
			case 1:  id = (uint32_t)v;      haveId = true;   break;
			case 7:  x = (int)v;            haveX = true;    break;
			case 8:  y = (int)v;            haveY = true;    break;
			case 12: mode = (int)v;         haveMode = true; break;
			default: break;
			}
		}

		// Only a move carries a position. Anything else would overwrite the
		// record with zeros and make a working move look like a failure.
		if (!haveId || !haveX || !haveY || !haveMode) return;
		if (mode != 137) return;

		// On a busy map everyone's moves arrive here, and the last one to
		// land is rarely ours. When a character is being watched, ignore the
		// rest - otherwise a neighbour's move is read as our own being
		// accepted, which is worse than no signal at all.
		const uint32_t watchId = g_watchId;
		if (watchId != 0 && id != watchId) return;

		EnterCriticalSection(&g_lock);
		g_serverPos.valid = true;
		g_serverPos.id = id;
		g_serverPos.x = x;
		g_serverPos.y = y;
		g_serverPos.mode = mode;
		g_serverPos.tick = GetTickCount();
		g_serverPos.seq = g_total;
		LeaveCriticalSection(&g_lock);
	}

	// ---- configuration ----------------------------------------------------
	bool g_fileLog = true;
	bool g_overlayStartup = true;
	uint32_t g_fileBytes = 256;
	char g_logPath[MAX_PATH] = { 0 };
	FILE* g_logFile = nullptr;

	// -----------------------------------------------------------------------
	// Paths / configuration
	// -----------------------------------------------------------------------
	bool KeyEquals(const char* line, const char* key)
	{
		while (*line && *key)
		{
			char a = *line++;
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (a != *key++) return false;
		}
		return *line == '\0' && *key == '\0';
	}

	void ExeDirectory(char* out, size_t outSize)
	{
		out[0] = '\0';
		char exePath[MAX_PATH] = { 0 };
		if (!GetModuleFileNameA(NULL, exePath, MAX_PATH)) return;
		char* slash = strrchr(exePath, '\\');
		if (slash) *(slash + 1) = '\0';
		_snprintf_s(out, outSize, _TRUNCATE, "%s", exePath);
	}

	void LoadConfig()
	{
		char dir[MAX_PATH];
		ExeDirectory(dir, sizeof(dir));
		if (!dir[0]) return;

		char iniPath[MAX_PATH];
		_snprintf_s(iniPath, _TRUNCATE, "%spacket_log.ini", dir);

		FILE* f = nullptr;
		if (fopen_s(&f, iniPath, "r") != 0 || !f)
		{
			// First run: drop a template next to the exe.
			if (fopen_s(&f, iniPath, "w") == 0 && f)
			{
				fputs("; PacketLog - plaintext packet capture for Conquer.exe\r\n"
					";\r\n"
					"; enabled  : 1 = hook the client's send/recv at all, 0 = disable everything\r\n"
					"; overlay  : 1 = show the in-game packet window on startup (F8 toggles)\r\n"
					"; file     : 1 = also append every packet to packets.log\r\n"
					"; filebytes: bytes dumped per packet in packets.log (16 .. 2048)\r\n"
					"\r\n"
					"enabled=1\r\n"
					"overlay=1\r\n"
					"file=1\r\n"
					"filebytes=256\r\n", f);
				fclose(f);
			}
			return;
		}

		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			if (line[0] == ';' || line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
			char* equals = strchr(line, '=');
			if (!equals) continue;
			*equals = '\0';

			int value = atoi(equals + 1);
			if (KeyEquals(line, "overlay"))
			{
				g_overlayStartup = (value != 0);
			}
			else if (KeyEquals(line, "file"))
			{
				g_fileLog = (value != 0);
			}
			else if (KeyEquals(line, "filebytes"))
			{
				if (value < 16) value = 16;
				if ((uint32_t)value > kHardMaxLength) value = (int)kHardMaxLength;
				g_fileBytes = (uint32_t)value;
			}
			// "enabled" is consumed by Install() before this point.
		}
		fclose(f);
	}

	bool ReadEnabledFlag()
	{
		char dir[MAX_PATH];
		ExeDirectory(dir, sizeof(dir));
		if (!dir[0]) return true;

		char iniPath[MAX_PATH];
		_snprintf_s(iniPath, _TRUNCATE, "%spacket_log.ini", dir);

		FILE* f = nullptr;
		if (fopen_s(&f, iniPath, "r") != 0 || !f) return true;   // no file yet -> on

		bool enabled = true;
		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			if (line[0] == ';' || line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
			char* equals = strchr(line, '=');
			if (!equals) continue;
			*equals = '\0';
			if (KeyEquals(line, "enabled")) enabled = (atoi(equals + 1) != 0);
		}
		fclose(f);
		return enabled;
	}

	void BuildLogPath()
	{
		char dir[MAX_PATH];
		ExeDirectory(dir, sizeof(dir));
		if (!dir[0]) return;
		_snprintf_s(g_logPath, _TRUNCATE, "%spackets.log", dir);
	}

	// -----------------------------------------------------------------------
	// packets.log writer
	// -----------------------------------------------------------------------
	void WriteLogEntry(uint8_t direction, const uint8_t* data, uint32_t length)
	{
		if (!g_fileLog || !g_logPath[0] || !g_fileLockReady) return;

		EnterCriticalSection(&g_fileLock);

		if (!g_logFile)
		{
			if (fopen_s(&g_logFile, g_logPath, "a") != 0 || !g_logFile)
			{
				LeaveCriticalSection(&g_fileLock);
				return;
			}
			SYSTEMTIME opened;
			GetLocalTime(&opened);
			fprintf(g_logFile,
				"\n===== session started %04u-%02u-%02u %02u:%02u:%02u =====\n",
				opened.wYear, opened.wMonth, opened.wDay,
				opened.wHour, opened.wMinute, opened.wSecond);
		}

		uint16_t messageId = (length >= 4) ? (uint16_t)(data[2] | (data[3] << 8)) : 0;
		SYSTEMTIME st;
		GetLocalTime(&st);

		fprintf(g_logFile, "[%02u:%02u:%02u.%03u] %-4s len=%-5u id=0x%04X  %s\n",
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			direction == DirectionSend ? "SEND" : "RECV",
			length, messageId, PacketNames::Lookup(messageId));

		uint32_t dump = length < g_fileBytes ? length : g_fileBytes;
		for (uint32_t i = 0; i < dump; i += 16)
		{
			fprintf(g_logFile, "  %04X  ", i);
			for (uint32_t j = 0; j < 16; ++j)
			{
				if (i + j < dump) fprintf(g_logFile, "%02X ", data[i + j]);
				else              fputs("   ", g_logFile);
				if (j == 7) fputc(' ', g_logFile);
			}
			fputs(" |", g_logFile);
			for (uint32_t j = 0; j < 16 && i + j < dump; ++j)
			{
				uint8_t c = data[i + j];
				fputc((c >= 0x20 && c < 0x7F) ? (int)c : '.', g_logFile);
			}
			fputs("|\n", g_logFile);
		}
		if (dump < length) fprintf(g_logFile, "  ... (%u more bytes)\n", length - dump);

		fflush(g_logFile);   // survive a crash mid-session

		LeaveCriticalSection(&g_fileLock);
	}

	// -----------------------------------------------------------------------
	// Ring writer. Called from the game thread (send) and the client's
	// network thread (recv), so everything is guarded by g_lock.
	// -----------------------------------------------------------------------
	void Push(uint8_t direction, const uint8_t* data, uint32_t length)
	{
		if (!g_lockReady) return;
		if (!data || length < 2) return;
		if (length > kHardMaxLength) length = kHardMaxLength;

		if (direction == DirectionSend) InterlockedIncrement(&g_totalSend);
		else                            InterlockedIncrement(&g_totalRecv);

		if (InterlockedCompareExchange(&g_paused, 0, 0) != 0)
		{
			InterlockedIncrement(&g_totalDropped);
			return;
		}

		uint32_t copyLength = length;
		if (copyLength > (uint32_t)kMaxStoredBytes) copyLength = kMaxStoredBytes;

		EnterCriticalSection(&g_lock);

		Entry& entry = g_ring[g_head];
		entry.seq = g_total;
		entry.tick = GetTickCount();
		entry.length = (uint16_t)length;
		entry.messageId = (length >= 4) ? (uint16_t)(data[2] | (data[3] << 8)) : 0;
		entry.direction = direction;
		entry.storedBytes = (uint16_t)copyLength;
		memcpy(entry.data, data, copyLength);

		g_head = (g_head + 1) % kRingSize;
		if (g_count < kRingSize) ++g_count;
		++g_total;

		LeaveCriticalSection(&g_lock);

		// Outside the ring lock on purpose: a slow disk must not stall the
		// renderer, which takes g_lock once per frame.
		WriteLogEntry(direction, data, length);
	}

	// -----------------------------------------------------------------------
	// Hooks
	// -----------------------------------------------------------------------

	// CMyClientSocket::DoSendMsg - outgoing plaintext, pre-encryption.
	//
	// The original is __thiscall: `this` in ECX, CMsg* on the stack, RET 4.
	// MSVC will not accept __thiscall on a free function (C3865), so the hook
	// is declared __fastcall instead - ECX = self, EDX = unused, and the real
	// argument still arrives on the stack. That is the same ABI, so the
	// trampoline can jump straight into it.
	int __fastcall HookedDoSendMsg(void* self, void* /*unusedEdx*/, void* msg)
	{
		if (msg)
		{
			__try
			{
				// Remember the socket for the send builder. Cheap, and it
				// must happen even when capture itself is dropped below.
				InterlockedExchangePointer(&g_lastSendSocket, self);

				const uint8_t* packet = (const uint8_t*)msg + 4;
				uint32_t length = *(const uint16_t*)packet;
				Push(DirectionSend, packet, length);

				// Let the builder learn this message's vtable. ObserveSend
				// copies the whole live CMsg (vtable + length + body) the
				// first time each id is seen, so a later re-send passes the
				// CMsg::GetSize() check inside DoSendMsg.
				PacketSend::ObserveSend(self, msg);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				InterlockedIncrement(&g_totalDropped);
			}
		}
		return g_realDoSendMsg(self, msg);
	}

	// Packet helper reached from DoReceive - incoming plaintext, post-decryption.
	uint16_t __cdecl HookedGetMsgType(const uint8_t* packet, int len)
	{
		if (packet && len >= 2)
		{
			__try
			{
				Push(DirectionRecv, packet, (uint32_t)len);

				// A 0x0833 receive is the server telling us where someone
				// is. Recording it is how a sent move gets verified: if the
				// server echoes our target back, it accepted the move.
				if (len >= 4 && packet[2] == 0x33 && packet[3] == 0x08)
					RecordServerPos(packet, (int)len);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				InterlockedIncrement(&g_totalDropped);
			}
		}
		return g_realGetMsgType(packet, len);
	}

	// The client's own action sender (0x0833). Logs the argument vector so a
	// real move can be read straight off, then passes the call through
	// untouched - this observes, it does not change behaviour.
	//
	// Reading the log: the mode is the argument whose value is 137 (0x89) on
	// a move, and the coordinate pair is the two adjacent small numbers that
	// change as the character walks. Those two facts are enough to place
	// every other argument relative to them.
	int __fastcall HookedActionSend(void* self, void* /*unusedEdx*/,
		int a2, int a3, int a4, int a5, int a6,
		int a7, int a8, int a9, int a10, int a11)
	{
		const int args[10] = { a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 };

		bool changed = !g_haveLastActionArgs;
		if (!changed)
		{
			for (int i = 0; i < 10; ++i)
				if (args[i] != g_lastActionArgs[i]) { changed = true; break; }
		}

		// Collapse repeats so a burst of identical actions is one line, but
		// never go silent for longer than kActionLogRepeatMs - otherwise an
		// unchanging stream would look like the hook had stopped working.
		const DWORD now = GetTickCount();
		if (changed || (now - g_lastActionLogTick) >= kActionLogRepeatMs)
		{
			for (int i = 0; i < 10; ++i) g_lastActionArgs[i] = args[i];
			g_haveLastActionArgs = true;
			g_lastActionLogTick = now;

			HookLog("[Action] a2=%d a3=%d a4=%d a5=%d a6=%d a7=%d a8=%d a9=%u a10=%d a11=%d",
				a2, a3, a4, a5, a6, a7, a8, (unsigned)a9, a10, a11);
		}

		// Keep the last action for the builder. Only a move (mode 137) is
		// worth keeping: it is the only shape that carries a position, so
		// anything else would give the builder a bogus "current position".
		// The clock is recorded alongside GetTickCount() so the reader can
		// age it forward - a captured clock is already stale by the time
		// anyone uses it.
		if (a6 == 137 && g_lockReady)
		{
			EnterCriticalSection(&g_lock);
			g_lastAction.valid = true;
			g_lastAction.id = (uint32_t)a2;
			g_lastAction.originX = a3;
			g_lastAction.originY = a4;
			g_lastAction.dir = a5;
			g_lastAction.mode = a6;
			g_lastAction.targetX = a7;
			g_lastAction.targetY = a8;
			g_lastAction.clock = (uint32_t)a9;
			g_lastAction.f17 = a10;
			g_lastAction.f20raw = a11;
			g_lastAction.capturedTick = GetTickCount();
			LeaveCriticalSection(&g_lock);
		}

		return g_realActionSend(self, nullptr,
			a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
	}

	// The role Process function. Deliberately does almost nothing: it records
	// the `this` pointer and forwards the call. Everything else (reading the
	// position out of that object) happens in the overlay, once per frame,
	// rather than per role per frame here.
	void __fastcall HookedRoleProcess(void* self, void* /*unusedEdx*/)
	{
		if (self)
		{
			g_roleSelf = self;
			g_roleSelfTick = GetTickCount();
		}
		g_realRoleProcess(self, nullptr);
	}


	// -----------------------------------------------------------------------
	// Install helpers
	// -----------------------------------------------------------------------
	bool SignatureMatches(uintptr_t address, const uint8_t* signature, size_t size)
	{
		__try
		{
			return memcmp((const void*)address, signature, size) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

// Copies the last move the client sent itself. See packet_capture.h for why
// the builder needs it: a replayed capture carries a stale clock and a stale
// origin, and a server that checks either will drop the move.
bool GetLastAction(LastAction& out)
{
	// Same guard the ring readers use: the overlay can ask for this before
	// Install() has initialised the lock. Returning false with a zeroed
	// struct keeps the caller on its "no live data yet" path rather than
	// reading an uninitialised critical section.
	out = LastAction();
	if (!g_lockReady) return false;

	EnterCriticalSection(&g_lock);
	out = g_lastAction;
	LeaveCriticalSection(&g_lock);
	return out.valid;
}

// Copies the last position the server reported for a character. See
// packet_capture.h: this is the end-to-end check on whether a move worked.
bool GetServerPos(ServerPos& out)
{
	out = ServerPos();
	if (!g_lockReady) return false;

	EnterCriticalSection(&g_lock);
	out = g_serverPos;
	LeaveCriticalSection(&g_lock);
	return out.valid;
}

// Copies the role object pointer captured by the Process hook. See
// packet_capture.h: this is what makes the position readable without moving.
bool GetRoleProbe(RoleProbe& out)
{
	out = RoleProbe();

	void* self = g_roleSelf;
	if (!self) return false;

	out.valid = true;
	out.self = (uint32_t)(uintptr_t)self;
	out.tick = g_roleSelfTick;
	return true;
}

// See packet_capture.h. 0 records everyone again.
void WatchCharacter(uint32_t id)
{
	if (!g_lockReady) return;
	EnterCriticalSection(&g_lock);
	g_watchId = id;
	LeaveCriticalSection(&g_lock);
}

void Install()
{
	if (g_installed) return;
	g_installed = true;

	if (!ReadEnabledFlag())
	{
		HookLog("[Capture] disabled by packet_log.ini");
		return;
	}

	LoadConfig();
	BuildLogPath();

	InitializeCriticalSection(&g_lock);
	g_lockReady = true;
	InitializeCriticalSection(&g_fileLock);
	g_fileLockReady = true;

	HMODULE exeModule = GetModuleHandleA(NULL);
	uintptr_t base = (uintptr_t)exeModule;
	if (!base)
	{
		HookLog("[Capture] could not resolve module base - hooks not installed");
		return;
	}

	uintptr_t doSendMsg = base + kRvaDoSendMsg;
	uintptr_t getMsgType = base + kRvaGetMsgType;

	// Stamp this DLL's own write time into the log.
	//
	// Every "is the new build actually running?" question has cost a round
	// trip this session - several tests were run against a DLL built seconds
	// before the fix. With the build time written at startup, one glance at
	// the log answers it and no external timestamp comparison is needed.
	{
		char selfPath[MAX_PATH] = { 0 };
		if (GetModuleFileNameA((HMODULE)exeModule, selfPath, MAX_PATH))
		{
			WIN32_FILE_ATTRIBUTE_DATA fad;
			if (GetFileAttributesExA(selfPath, GetFileExInfoStandard, &fad))
			{
				FILETIME local = { 0 };
				SYSTEMTIME st = { 0 };
				if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local) &&
					FileTimeToSystemTime(&local, &st))
				{
					HookLog("[Build] DLL written %04d-%02d-%02d %02d:%02d:%02d",
						st.wYear, st.wMonth, st.wDay,
						st.wHour, st.wMinute, st.wSecond);
				}
			}
		}
	}

	HookLog("[Capture] base %p  DoSendMsg %p  GetMsgType %p", (void*)base, (void*)doSendMsg, (void*)getMsgType);
	HookLog("[Capture] log %s (file=%d, filebytes=%u, overlay=%d)",
		g_logPath, g_fileLog ? 1 : 0, g_fileBytes, g_overlayStartup ? 1 : 0);

	if (SignatureMatches(doSendMsg, kSigDoSendMsg, sizeof(kSigDoSendMsg)))
	{
		MH_STATUS st = MH_CreateHook((LPVOID)doSendMsg, (LPVOID)HookedDoSendMsg, (LPVOID*)&g_realDoSendMsg);
		if (st == MH_OK) { MH_EnableHook((LPVOID)doSendMsg); HookLog("[Capture] SEND hook installed"); }
		else HookLog("[Capture] SEND hook failed %d", st);
	}
	else
	{
		HookLog("[Capture] SEND signature mismatch at %p - client build differs, hook skipped", (void*)doSendMsg);
	}

	if (SignatureMatches(getMsgType, kSigGetMsgType, sizeof(kSigGetMsgType)))
	{
		MH_STATUS st = MH_CreateHook((LPVOID)getMsgType, (LPVOID)HookedGetMsgType, (LPVOID*)&g_realGetMsgType);
		if (st == MH_OK) { MH_EnableHook((LPVOID)getMsgType); HookLog("[Capture] RECV hook installed"); }
		else HookLog("[Capture] RECV hook failed %d", st);
	}
	else
	{
		HookLog("[Capture] RECV signature mismatch at %p - client build differs, hook skipped", (void*)getMsgType);
	}

	// The action sender. Observational only - it logs and forwards, so a
	// signature mismatch costs nothing but a missing log line.
	uintptr_t actionSend = base + kRvaActionSend;
	if (SignatureMatches(actionSend, kSigActionSend, sizeof(kSigActionSend)))
	{
		MH_STATUS st = MH_CreateHook((LPVOID)actionSend, (LPVOID)HookedActionSend, (LPVOID*)&g_realActionSend);
		if (st == MH_OK) { MH_EnableHook((LPVOID)actionSend); HookLog("[Capture] ACTION hook installed"); }
		else HookLog("[Capture] ACTION hook failed %d", st);
	}
	else
	{
		HookLog("[Capture] ACTION signature mismatch at %p - client build differs, hook skipped", (void*)actionSend);
	}

	// The role Process function. Also observational - it records a pointer and
	// forwards, so a mismatch costs a log line, not behaviour.
	uintptr_t roleProcess = base + kRvaRoleProcess;
	if (SignatureMatches(roleProcess, kSigRoleProcess, sizeof(kSigRoleProcess)))
	{
		MH_STATUS st = MH_CreateHook((LPVOID)roleProcess, (LPVOID)HookedRoleProcess, (LPVOID*)&g_realRoleProcess);
		if (st == MH_OK) { MH_EnableHook((LPVOID)roleProcess); HookLog("[Capture] ROLE hook installed"); }
		else HookLog("[Capture] ROLE hook failed %d", st);
	}
	else
	{
		HookLog("[Capture] ROLE signature mismatch at %p - client build differs, hook skipped", (void*)roleProcess);
	}
}

bool IsPaused()
{
	return InterlockedCompareExchange(&g_paused, 0, 0) != 0;
}

void SetPaused(bool paused)
{
	InterlockedExchange(&g_paused, paused ? 1 : 0);
}

void Clear()
{
	if (!g_lockReady) return;
	EnterCriticalSection(&g_lock);
	g_head = 0;
	g_count = 0;
	LeaveCriticalSection(&g_lock);
}

long TotalSend() { return g_totalSend; }
long TotalRecv() { return g_totalRecv; }
long TotalDropped() { return g_totalDropped; }

Snapshot BeginRead()
{
	Snapshot snapshot = { g_ring, 0, 0, 0 };
	if (!g_lockReady) return snapshot;

	EnterCriticalSection(&g_lock);
	snapshot.head = g_head;
	snapshot.count = g_count;
	snapshot.total = g_total;
	return snapshot;
}

void EndRead()
{
	if (!g_lockReady) return;
	LeaveCriticalSection(&g_lock);
}

long IndexOfSeq(const Snapshot& snapshot, uint32_t seq)
{
	if (snapshot.count <= 0) return -1;

	uint32_t oldest = snapshot.total - (uint32_t)snapshot.count;
	if (seq < oldest || seq >= snapshot.total) return -1;

	long offsetFromOldest = (long)(seq - oldest);
	long oldestIndex = (snapshot.head - snapshot.count + kRingSize) % kRingSize;
	return (oldestIndex + offsetFromOldest) % kRingSize;
}

bool FileLoggingEnabled() { return g_fileLog; }
bool OverlayEnabledAtStartup() { return g_overlayStartup; }
const char* LogFilePath() { return g_logPath; }

DoSendMsgFn RealDoSendMsg() { return g_realDoSendMsg; }

void* LastSendSocket()
{
	return InterlockedCompareExchangePointer(&g_lastSendSocket, nullptr, nullptr);
}

} // namespace PacketCapture
