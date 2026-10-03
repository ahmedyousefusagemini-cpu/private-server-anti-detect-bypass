// ============================================================================
// PacketSend - build and send packets from the overlay
// ----------------------------------------------------------------------------
// The mechanism is capture-and-replay, described at length in packet_send.h.
// The short version:
//
//   * ObserveSend() is called from the DoSendMsg hook with the live CMsg*.
//     The first time each packet id is seen, the object is copied into a
//     slot - [vtable(4)][wire length(2)][id(2)][payload...] - so we own the
//     genuine vtable pointer for that message type.
//
//   * SendSlot() lays out the same shape in a scratch buffer (with the
//     caller's edited body), then calls the real DoSendMsg through the
//     trampoline. DoSendMsg's virtual CMsg::GetSize() call lands on the
//     borrowed vtable, returns the length we wrote, and the size check
//     passes - so the message goes out down the normal path, cipher and all.
//
// We never invent a vtable and never call anything through a pointer we did
// not first observe on a live object.
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <cstring>
#include "packet_send.h"
#include "packet_capture.h"
#include "log.h"

namespace PacketSend {
namespace {

	CRITICAL_SECTION g_lock;
	bool g_lockReady = false;

	Slot g_slots[kMaxSlots];

	// Scratch buffer for the outgoing message. Kept as a module-level block
	// rather than a stack array so it is naturally 8-byte aligned (the vtable
	// pointer is read as a dword, and the client's memcpy expects a normal
	// heap-like buffer). SendSlot is only ever called from the render thread,
	// and the socket serialises its own sends, so one buffer is enough.
	//
	// Worst case is the 4-byte prefix plus a full body; the slack absorbs any
	// off-by-one in the length arithmetic without ever writing out of bounds.
	uint8_t g_sendBuffer[kMaxMessageBytes + 16];

	volatile long g_totalSent = 0;
	volatile long g_totalRefused = 0;

	void EnsureLock()
	{
		if (g_lockReady) return;
		InitializeCriticalSection(&g_lock);
		g_lockReady = true;
	}

	Slot* FindSlot(uint16_t messageId)
	{
		for (int i = 0; i < kMaxSlots; ++i)
			if (g_slots[i].used && g_slots[i].messageId == messageId)
				return &g_slots[i];
		return nullptr;
	}

	Slot* ClaimSlot(uint16_t messageId)
	{
		Slot* existing = FindSlot(messageId);
		if (existing) return existing;

		// Prefer a free slot; if none, recycle the least recently seen.
		Slot* victim = nullptr;
		for (int i = 0; i < kMaxSlots; ++i)
		{
			if (!g_slots[i].used)
			{
				victim = &g_slots[i];
				break;
			}
			if (!victim || g_slots[i].lastSeenTick < victim->lastSeenTick)
				victim = &g_slots[i];
		}
		if (!victim) return nullptr;

		memset(victim, 0, sizeof(*victim));
		victim->messageId = messageId;
		victim->used = true;
		return victim;
	}

} // namespace

// ---------------------------------------------------------------------------
// Capture side
// ---------------------------------------------------------------------------
void ObserveSend(void* socket, const void* msg)
{
	if (!msg) return;
	EnsureLock();

	__try
	{
		const uint8_t* bytes = (const uint8_t*)msg;

		// CMsg layout, confirmed against DoSendMsg's decompilation and the
		// live capture:
		//   msg+0..3  vtable
		//   msg+4..5  uint16 wire length  (the whole packet: id + payload)
		//   msg+6..7  uint16 message id
		//   msg+8..   protobuf payload
		//
		// DoSendMsg checks *(u16*)(msg+4) against the virtual GetSize() and
		// then memcpy's length bytes from msg+4.
		const uint32_t vtable = *(const uint32_t*)bytes;
		const uint16_t length = *(const uint16_t*)(bytes + 4);

		// A CMsg must have at least the 4-byte header, a plausible length,
		// and a non-null vtable. Anything else is not a message we can use.
		if (vtable == 0) return;
		if (length < 4 || length > kMaxMessageBytes) return;

		// The body we store starts at the id, so its size is length minus
		// the 4-byte [vtable][len] prefix (the id itself is part of the
		// length DoSendMsg validates).
		// `length` at msg+4 is the size of the whole wire packet, which is
		// [u16 len][u16 id][payload]. The body we store starts at the id, so
		// it is the packet minus the 2-byte len field: length - 2.
		//
		// This used to be length - 4, which dropped the last 2 payload bytes
		// of EVERY captured template. Combined with SendSlot's matching
		// error the total size came out right, which is why it looked
		// healthy - but the final 2 bytes were zeros, corrupting the last
		// field of every packet we replayed.
		const uint16_t bodyBytes = (uint16_t)(length - 2);
		if (bodyBytes > kMaxBodyBytes) return;
		if (bodyBytes < 2) return;            // at least the id must fit

		const uint16_t messageId = (uint16_t)(bytes[6] | (bytes[7] << 8));

		EnterCriticalSection(&g_lock);

		Slot* slot = ClaimSlot(messageId);
		if (slot)
		{
			slot->vtable = vtable;
			slot->length = length;
			slot->bodyBytes = bodyBytes;
			memcpy(slot->body, bytes + 6, bodyBytes);
			slot->lastSeenTick = GetTickCount();
			++slot->captures;
		}

		LeaveCriticalSection(&g_lock);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		// A malformed message must never take the game down.
	}
}

// ---------------------------------------------------------------------------
// Query side
// ---------------------------------------------------------------------------
bool GetSlot(uint16_t messageId, Slot& out)
{
	EnsureLock();
	bool found = false;
	EnterCriticalSection(&g_lock);
	if (Slot* slot = FindSlot(messageId))
	{
		out = *slot;
		found = true;
	}
	LeaveCriticalSection(&g_lock);
	return found;
}

int ListArmed(uint16_t* ids, int maxIds)
{
	EnsureLock();
	if (!ids || maxIds <= 0) return 0;

	int count = 0;
	EnterCriticalSection(&g_lock);
	for (int i = 0; i < kMaxSlots && count < maxIds; ++i)
		if (g_slots[i].used)
			ids[count++] = g_slots[i].messageId;
	LeaveCriticalSection(&g_lock);
	return count;
}

bool CanSend(uint16_t messageId)
{
	EnsureLock();
	bool armed = false;
	EnterCriticalSection(&g_lock);
	armed = (FindSlot(messageId) != nullptr);
	LeaveCriticalSection(&g_lock);

	return armed && PacketCapture::LastSendSocket() != nullptr
		&& PacketCapture::RealDoSendMsg() != nullptr;
}

// ---------------------------------------------------------------------------
// Send side
// ---------------------------------------------------------------------------
// Logs one outgoing injected packet to hook_init.log.
//
// This is NOT redundant with the capture hook. SendSlot calls the MinHook
// trampoline (RealDoSendMsg), which is the ORIGINAL function body and so
// bypasses HookedDoSendMsg entirely. Injected packets therefore never appear
// in packets.log, and without this line there is no way to see what we
// actually put on the wire - which makes "the send reported ok but the
// character did not move" impossible to diagnose.
//
// The format matches the capture's, so the two can be compared directly.
void LogInjected(const uint8_t* packet, int length)
{
	if (!packet || length < 4) return;

	static const char* kDigits = "0123456789ABCDEF";
	char hex[3 * 128 + 4];
	size_t n = 0;

	const int shown = (length < 128) ? length : 128;
	for (int i = 0; i < shown; ++i)
	{
		if (n + 4 >= sizeof(hex)) break;
		hex[n++] = kDigits[(packet[i] >> 4) & 0xF];
		hex[n++] = kDigits[packet[i] & 0xF];
		hex[n++] = ' ';
	}
	if (n) --n;                     // drop the trailing space
	hex[n] = '\0';

	HookLog("[Send] INJECTED len=%d id=0x%04X  %s%s", length,
		(unsigned)(packet[2] | (packet[3] << 8)), hex,
		(length > shown) ? " ..." : "");
}

int SendSlot(uint16_t messageId, const uint8_t* bodyOverride, int bodyOverrideBytes)
{
	EnsureLock();

	void* socket = PacketCapture::LastSendSocket();
	PacketCapture::DoSendMsgFn doSend = PacketCapture::RealDoSendMsg();

	Slot local;
	bool haveSlot = false;

	EnterCriticalSection(&g_lock);
	if (Slot* slot = FindSlot(messageId))
	{
		local = *slot;
		haveSlot = true;
	}
	LeaveCriticalSection(&g_lock);

	if (!haveSlot || !socket || !doSend)
	{
		InterlockedIncrement(&g_totalRefused);
		return -1;
	}

	// Work out the body for this send. An override (the edited builder
	// contents) wins; otherwise replay exactly what was captured.
	const uint8_t* body = local.body;
	int bodyBytes = local.bodyBytes;

	if (bodyOverride)
	{
		body = bodyOverride;
		bodyBytes = bodyOverrideBytes;
		if (bodyBytes < 2) bodyBytes = 2;              // room for the id
		if (bodyBytes > kMaxBodyBytes) bodyBytes = kMaxBodyBytes;

		// The body carries its own id at [0..1], and the vtable we are about
		// to borrow came from `messageId`'s slot. If an edit changed the id
		// but not the template, the message would go out under whichever
		// class's vtable we happen to hold - a mismatch the client's own
		// dispatch would never produce. Refuse rather than send that.
		const uint16_t bodyId = (uint16_t)(body[0] | (body[1] << 8));
		if (bodyId != messageId)
		{
			InterlockedIncrement(&g_totalRefused);
			return -2;                                 // id / vtable mismatch
		}
	}
	if (bodyBytes < 2) bodyBytes = 2;

	// DoSendMsg validates *(u16*)(msg+4) against the virtual GetSize() and
	// memcpy's that many bytes from msg+4. So `length` IS the size of the
	// wire packet, which is laid out as [u16 len][u16 id][payload].
	//
	// The body we hold starts at the id, so the bytes actually written from
	// msg+4 are 2 (the len field) + bodyBytes (id + payload). That is the
	// number `length` has to be.
	//
	// It used to be bodyBytes + 4, which told DoSendMsg to read 2 bytes past
	// the body - the zeros from the memset below. Every injected packet
	// therefore ended in two zero bytes, which corrupts the last field. For
	// the 0x0833 move that is the tail of f20's 10-byte all-ones varint, so
	// the message reached the server malformed and was dropped.
	const uint16_t length = (uint16_t)(bodyBytes + 2);

	// Lay the message out in the scratch buffer. This is the whole trick:
	// we reuse the vtable we observed on a live CMsg, so the virtual
	// CMsg::GetSize() inside DoSendMsg resolves to real code and returns
	// `length` - which is exactly what the size check compares against.
	uint8_t* out = g_sendBuffer;
	memset(out, 0, sizeof(g_sendBuffer));
	*(uint32_t*)(out + 0) = local.vtable;
	*(uint16_t*)(out + 4) = length;
	if (bodyBytes > 0 && body)
		memcpy(out + 6, body, (size_t)bodyBytes);

	// Confirm our own work before handing it over: if the layout ever
	// drifts, fail closed rather than fault inside the game.
	if (*(const uint16_t*)(out + 4) != length)
	{
		InterlockedIncrement(&g_totalRefused);
		return -1;
	}

	InterlockedIncrement(&g_totalSent);

	// Record what is actually going out, before it goes out - if the send
	// faults we still want to see the bytes we tried.
	// out+4, not out: the buffer starts with the 4-byte [vtable][len] prefix,
	// and the wire packet begins at the len field. Logging from `out` shifted
	// every byte by 4, so the id read back as garbage (0x016F instead of
	// 0x0833) and the dump was 4 bytes short.
	LogInjected(out + 4, length);

	int result = 0;
	__try
	{
		result = doSend(socket, out);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		HookLog("[Send] DoSendMsg raised for id 0x%04X - message skipped", messageId);
		return -1;
	}

	return result;
}

long TotalSent() { return g_totalSent; }
long TotalRefused() { return g_totalRefused; }

} // namespace PacketSend
