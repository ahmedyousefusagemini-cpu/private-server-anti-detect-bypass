#pragma once

// ============================================================================
// PacketCapture - plaintext network packet capture for Conquer.exe
// ----------------------------------------------------------------------------
// Rather than hooking ws2_32 (which only ever sees the ENCRYPTED bytes), this
// hooks the client's OWN send/receive functions, so the buffers observed are
// the plaintext packets:
//
//   * outgoing  - read BEFORE the TQ cipher runs on the way out
//   * incoming  - read AFTER the cipher has run on the way in
//
// Everything below comes from static analysis of Conquer.exe build 7952
// (image base 0x00400000). See docs/packet-hooks.md for the full derivation
// and the re-derivation procedure after a client update.
//
//   SEND  CMyClientSocket::DoSendMsg        @ 0x012799A1  (RVA 0x00E799A1)
//         __thiscall int DoSendMsg(CMyClientSocket* this, CMsg* msg)
//         CMsg layout: [0..3]=vtable, [4..5]=uint16 total length, [6..]=body.
//         The method memcpy's msg+4 into its send staging buffer and only
//         THEN calls the cipher (CMyClientSocket+0x1CD8), so hooking the
//         entry yields the outgoing plaintext.
//
//   RECV  CMyClientSocket packet helper      @ 0x00D0C67B  (RVA 0x0090C67B)
//         __cdecl uint16 GetMsgType(const uint8_t* packet, int len)
//         Reached from CMyClientSocket::DoReceive exactly once per packet,
//         after decryption, with the plaintext packet in the buffer
//         (packet[0..1] = length, packet[2..3] = message id).
//
// Both hook points are reached for every server type the client supports
// (the type-2 path decrypts via FUN_01279f68 but still funnels into the same
// helper), so a single pair of hooks covers all connections.
//
// Captured packets go to two places:
//   * a fixed-size ring buffer in memory, read once per frame by the overlay
//     (packet_overlay.cpp)
//   * an append-only text log next to the game exe (packets.log), so traffic
//     can be diffed / grepped after the session
//
// Configuration lives in packet_log.ini next to the game exe; it is created
// with defaults on first run.
// ============================================================================

#include <cstdint>

namespace PacketCapture {

	// Bytes kept per packet. Packets larger than this are truncated in the
	// ring (the on-wire length is still recorded).
	const int kMaxStoredBytes = 512;

	// Packets retained before the oldest is recycled.
	const int kRingSize = 1024;

	enum Direction : uint8_t
	{
		DirectionSend = 0,
		DirectionRecv = 1
	};

	struct Entry
	{
		uint32_t seq;          // monotonic capture index (never reused)
		uint32_t tick;         // GetTickCount() at capture time
		uint16_t length;       // full packet length as it appears on the wire
		uint16_t messageId;    // packet[2..3]
		uint8_t  direction;    // Direction
		uint16_t storedBytes;  // bytes actually copied into data[]
		uint8_t  data[kMaxStoredBytes];
	};

	// Snapshot of the ring for the renderer. Valid only between
	// BeginRead()/EndRead(); the ring is written by the game's network
	// threads, so the lock must be held while iterating.
	struct Snapshot
	{
		const Entry* ring;   // kRingSize entries
		long     head;       // index the next packet will be written to
		long     count;      // entries currently filled (<= kRingSize)
		uint32_t total;      // packets captured since startup (monotonic)
	};

	// Installs the hooks and loads packet_log.ini. Call once, after
	// MH_Initialize() and after the loader lock has been released.
	// Idempotent.
	void Install();

	// Capture control, driven by the overlay's hotkeys.
	bool IsPaused();
	void SetPaused(bool paused);
	void Clear();

	// Totals since startup (not affected by Clear()).
	long TotalSend();
	long TotalRecv();

	// Dropped because the capture was paused or a packet failed validation.
	long TotalDropped();

	Snapshot BeginRead();
	void EndRead();

	// Maps a sequence number to its index in the ring, or -1 if it has been
	// recycled. Caller must hold the read lock.
	long IndexOfSeq(const Snapshot& snapshot, uint32_t seq);

	// ---- configuration (packet_log.ini, next to the game exe) -------------
	//   enabled=1    install the hooks at all
	//   file=1       append captured packets to packets.log
	//   filebytes=N  bytes dumped per packet in the log (16 .. 2048)
	//   overlay=1    show the in-game window on startup
	bool FileLoggingEnabled();
	bool OverlayEnabledAtStartup();
	const char* LogFilePath();

	// ---- send support (used by packet_send.cpp) --------------------------
	// The real CMyClientSocket::DoSendMsg, captured by the MinHook trampoline.
	// Null until Install() has run and the SEND hook is in place. PacketSend
	// uses it to re-issue a message through the client's own code path, so
	// the size check, cipher and socket write all behave exactly as they do
	// for a message the game built itself.
	typedef int(__thiscall* DoSendMsgFn)(void* self, void* msg);
	DoSendMsgFn RealDoSendMsg();

	// The CMyClientSocket* seen on the most recent outgoing send. Needed
	// because DoSendMsg is __thiscall - the socket is the implicit `this`.
	// Null until at least one packet has been sent this session.
	void* LastSendSocket();
}
