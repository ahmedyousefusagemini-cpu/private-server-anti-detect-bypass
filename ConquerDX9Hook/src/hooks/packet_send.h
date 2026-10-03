#pragma once

// ============================================================================
// PacketSend - build and send packets from the overlay
// ----------------------------------------------------------------------------
// WHY THIS IS NOT A RAW BYTE INJECTION
//
// CMyClientSocket::DoSendMsg (0x012799A1) validates the message before it
// sends anything. Decompiled:
//
//     uVar2 = (**(code **)(*msg + 8))();       // CMsg::GetSize()  - VIRTUAL
//     uVar6 = *(uint16 *)(msg + 4);            // length field
//     if (uVar6 != (uVar2 & 0xffff)) {
//         FUN_01223336("CMyClientSocket::DoSendMsg Check Size Failed %u", ...);
//         return 0;                            // <-- never reaches send()
//     }
//
// The body bytes are NEVER parsed - they are memcpy'd into the socket's
// staging buffer (this+0x1008) and ciphered. So the only thing DoSendMsg
// insists on is that msg[0..3] be a REAL vtable whose slot [+8] returns
// exactly the value in the length field at msg+4.
//
// A raw malloc'd buffer therefore cannot be fed in: the virtual call through
// a garbage "vtable" would fault. The way around it is to borrow a vtable
// from a message the client itself built - which is exactly what happens
// below: the send hook copies the live CMsg object (vtable included) the
// first time an id is seen, and every later send reuses that vtable pointer.
//
// WHAT THE HOOK STORES
//
//   HookedDoSendMsg sees the live CMsg*. On the first send of each id it
//   snapshots the object as [vtable(4)][wire length(2)][id(2)][payload...]
//   into a slot, and remembers the CMyClientSocket* it arrived on. "Send"
//   later rebuilds that exact layout with the current (possibly edited)
//   length/body and hands it back to the real DoSendMsg, so the vtable - and
//   the GetSize() check - are genuine.
//
// THREADING
//
// DoSendMsg is called from the game's main thread. The overlay renders from
// the D3D present thread. Slots are guarded by their own critical section,
// and SendSlot() is called from the render thread, so the two never race on
// the same CMsg at once (the socket serialises its own sends internally).
//
// See docs/packet-send.md for the derivation and the wiring diagram.
// ============================================================================

#include <cstdint>

namespace PacketSend
{

	// Largest message we will build/forward. Mirrors the capture limit; the
	// socket's own staging buffer is 0x800 (2048) bytes, so staying under
	// that is what matters.
	const int kMaxMessageBytes = 1024;

	// Body bytes kept per slot. The body starts at the message id, so it is
	// the wire length minus the 4-byte [vtable][length] prefix.
	// The body is [u16 id][payload] and sits inside a wire packet of
	// [u16 len][u16 id][payload], so the body is the message minus the 2-byte
	// length field - not minus the 4-byte [vtable][len] prefix that precedes
	// it in the CMsg object.
	const int kMaxBodyBytes = kMaxMessageBytes - 2;

	// Number of distinct packet ids that can be armed for replay at once.
	const int kMaxSlots = 16;

	struct Slot
	{
		bool     used;                 // a live CMsg has been seen for this id
		uint16_t messageId;            // e.g. 0x0833
		uint32_t vtable;               // captured from the live CMsg (msg[0..3])
		uint16_t length;               // wire length as captured (body + 4)
		uint16_t bodyBytes;            // bytes valid in body[] (starts at the id)
		uint8_t  body[kMaxBodyBytes];  // [id(2)][protobuf payload...]
		uint32_t lastSeenTick;         // GetTickCount() of the last capture
		uint32_t captures;             // how many sends have been folded in
	};

	// ---- capture side (called from the DoSendMsg hook) --------------------
	// Records the live CMsg for its id. Called on every outgoing send; cheap
	// (a memcpy of a small object) and safe from the game thread.
	//
	//   socket  - the CMyClientSocket* the message was sent on (may be null)
	//   msg     - the live CMsg* (may be null)
	void ObserveSend(void* socket, const void* msg);

	// ---- query side (called from the overlay) -----------------------------
	// Copies the current slot for an id into `out`. Returns false when that
	// id has never been captured.
	bool GetSlot(uint16_t messageId, Slot& out);

	// Lists the ids that currently have a captured message.
	int  ListArmed(uint16_t* ids, int maxIds);

	// True when we have a socket and at least one armed id - i.e. a send can
	// actually go out.
	bool CanSend(uint16_t messageId);

	// ---- send side (called from the overlay) ------------------------------
	// Rebuilds the captured message with the slot's stored vtable, applies
	// `body`/`bodyBytes` and `length`, then hands it to the real DoSendMsg.
	//
	// Returns the value DoSendMsg returned:
	//   0  success
	//   4  the socket reported a send error
	//  -1  we refused (no slot, no socket, or the message did not fit)
	//  -2  the body's own id does not match the slot whose vtable we hold
	//
	// `bodyOverride` (optional) replaces the body for this send only; pass
	// nullptr to use exactly what is stored in the slot. The override MUST
	// begin with the same 2-byte id the slot was captured under - see the
	// `-2` case above.
	int SendSlot(uint16_t messageId, const uint8_t* bodyOverride, int bodyOverrideBytes);

	// Number of sends this module has issued (for the UI).
	long TotalSent();

	// Number of sends that were refused before reaching the socket.
	long TotalRefused();

	// ---- config -----------------------------------------------------------
	// The id the builder arms by default (the jump / CMsgAction packet).
	const uint16_t kDefaultBuildId = 0x0833;

} // namespace PacketSend
