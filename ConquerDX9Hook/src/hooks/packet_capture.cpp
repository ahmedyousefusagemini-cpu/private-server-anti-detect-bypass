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

	// Prologues, used to confirm the client build matches before hooking.
	//   DoSendMsg : PUSH EBP; MOV EBP,ESP; SUB ESP,0Ch; PUSH EBX; PUSH ESI; PUSH EDI; MOV EDI,[EBP+8]
	const uint8_t kSigDoSendMsg[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08 };
	//   GetMsgType: PUSH EBP; MOV EBP,ESP; MOV EAX,[EBP+8]; MOV AX,[EAX+2]; POP EBP; RET
	const uint8_t kSigGetMsgType[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x66, 0x8B, 0x40, 0x02, 0x5D, 0xC3 };

	// Never read more than this from a claimed packet length.
	const uint32_t kHardMaxLength = 2048;

	typedef int(__thiscall* DoSendMsgFn)(void* self, void* msg);
	typedef uint16_t(__cdecl* GetMsgTypeFn)(const uint8_t* packet, int len);

	DoSendMsgFn g_realDoSendMsg = nullptr;
	GetMsgTypeFn g_realGetMsgType = nullptr;

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
				const uint8_t* packet = (const uint8_t*)msg + 4;
				uint32_t length = *(const uint16_t*)packet;
				Push(DirectionSend, packet, length);
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
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				InterlockedIncrement(&g_totalDropped);
			}
		}
		return g_realGetMsgType(packet, len);
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

} // namespace PacketCapture
