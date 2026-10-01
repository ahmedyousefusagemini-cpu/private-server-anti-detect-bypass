# Sending packets: the jump sender

The overlay can now *send* packets, not just observe them. This note records
why the implementation looks the way it does, because the obvious approach —
hand it a byte buffer — does not work.

## The obstacle: `DoSendMsg` validates before it sends

`CMyClientSocket::DoSendMsg` (`0x012799A1`, RVA `0x00E799A1`) is the single
place every outgoing packet leaves through. Ghidra's decompilation of it is
short and decisive:

```c
undefined4 __thiscall DoSendMsg(int param_1 /*this*/, int *param_2 /*msg*/)
{
  uVar2 = (**(code **)(*param_2 + 8))();       // CMsg::GetSize()  - VIRTUAL
  uVar6 = (uint)*(ushort *)(param_2 + 1);      // *(u16*)(msg + 4)
  uVar2 = uVar2 & 0xffff;
  if (uVar6 != uVar2) {
    uVar1 = (**(code **)(*param_2 + 4))();     // used only to format the log
    FUN_01223336("CMyClientSocket::DoSendMsg Check Size Failed %u", uVar1);
    return 0;                                  // <-- returns WITHOUT sending
  }
  ...
  memcpy(this + 0x1008, param_2 + 1, uVar2);   // plaintext body -> staging
  ...
  FUN_01279c91(this + 0x1008, uVar2, 1);       // TQ cipher
  return FUN_012790a2(this + 0x1008, uVar6);   // raw send()
}
```

Two things follow:

1. **The first 4 bytes of a `CMsg` are a vtable pointer.** `DoSendMsg` makes a
   *virtual call* through it (`[vtable+8]`). A raw `malloc`'d buffer would jump
   through garbage and fault.
2. **There is no way to skip the check.** It runs before `memcpy`, before the
   cipher, before the socket write. GetSize() must equal the length field.

So the only requirements on a message are:

* `msg[0..3]` points at a **real vtable** whose `[+8]` slot returns a length.
* `*(u16*)(msg+4)` equals that length.

The body bytes themselves are **never parsed** by `DoSendMsg`. The cipher does
not care what they mean either. That is what makes replay viable.

## The message layout

```
offset   size   meaning
------   ----   ------------------------------------------------
  +0      4     vtable pointer          (validated: virtual GetSize())
  +4      2     uint16 wire length      (validated: == GetSize())
  +6      2     uint16 message id       (e.g. 0x0833)
  +8      n     protobuf payload
```

`DoSendMsg` `memcpy`s `length` bytes starting at `+4`, so the wire packet is
`[len][id][payload]` and the id is part of the length. A 46-byte jump packet is
therefore 4 bytes of vtable, then 46 bytes of wire data:

```
vtable  EF BE AD DE
wire    2E 00 33 08 08 8E CB 52 38 F9 02 40 E2 01 48 90 ...
        ^len=46 ^id=0x0833 ^payload...
```

## The approach: capture and replay

1. **Learn a vtable.** `HookedDoSendMsg` already sees every live `CMsg*`. It
   calls `PacketSend::ObserveSend(socket, msg)`, which on the first send of each
   id copies the object into a slot, keeping the genuine vtable pointer:

   ```
   slot.vtable     = *(u32*)(msg + 0)
   slot.bodyBytes  = *(u16*)(msg + 4) - 4     // id + payload
   slot.body       = copy of msg[6 .. 6+bodyBytes)
   ```

   Nothing is invented. The vtable is whatever the client itself used.

2. **Rebuild and re-send.** `PacketSend::SendSlot()` lays the same shape into a
   module-owned scratch buffer, with the (possibly edited) body, and calls the
   real `DoSendMsg` through MinHook's trampoline:

   ```
   out[0..3] = slot.vtable
   out[4..5] = bodyBytes + 4
   out[6..]  = body            (id + payload)
   result    = g_realDoSendMsg(socket, out)
   ```

   Because the vtable is real, `GetSize()` resolves to real code, the size check
   passes, and the message travels the normal path — cipher included.

3. **The socket.** `DoSendMsg` is `__thiscall`, so the `CMyClientSocket*` is the
   implicit `this`. `HookedDoSendMsg` stores the most recent one; the game
   recreates the socket on reconnect, so it is refreshed on every send rather
   than cached once.

## Why the packet cannot be built from scratch

The captured `0x0833` body is 42 bytes: nine clean protobuf varint fields, then
a 13-byte tail that is **not** protobuf:

```
body+ 0  tag 0x08  field 1  = 1353102
body+ 4  tag 0x38  field 7  = 377
body+ 7  tag 0x40  field 8  = 226
body+10  tag 0x48  field 9  = 6666000
body+15  tag 0x60  field 12 = 137
body+18  tag 0x68  field 13 = 0
body+20  tag 0x70  field 14 = 378
body+23  tag 0x78  field 15 = 216
body+26  tag 0x88  field 17 = 1
body+28  tag 0xFC  field 31  wiretype 4  <-- not a varint; parse stops here
tail     fc 50 a0 01 ff ff ff ff ff ff ff ff ff 01   (14 bytes)
```

`0xFF` is not a legal protobuf tag, so the tail is either a different
serialization or obfuscated. Since the field numbering also does not match
other public Conquer clients' jump packets (which use contiguous fields 1–11
with `mode = 19` in field 1), a synthetic body would be a guess. Replaying a
real capture avoids the guess entirely.

## What the UI exposes

* **Packets tab → Packet Builder.** Type an id, press *Load from Capture* to
  arm it from the live traffic, edit the decoded varint fields or the raw hex,
  then *Send* (optionally N times with a gap). "Use Selected" loads the packet
  highlighted in the table.
* **Map tab → Travel.** The `Speedhack If No Players Nearby` checkbox from the
  original template now drives a real sender: a repeat interval, a per-jump
  "lead" applied to the timestamp field, and an auto-jump toggle.

## Caveats

* **Capture first.** Until the client itself has sent a given id, there is no
  vtable to borrow and the builder refuses (it reports `refused` in the status
  bar rather than sending something invalid).
* **The id is part of the body.** `SendSlot` writes `bodyBytes + 4` as the
  length, so the id at `body[0..1]` is transmitted too — it must match the
  message type the vtable belongs to. Editing the id without also editing the
  vtable would send a mismatch; the builder therefore keeps them paired by
  loading one template at a time.
* **Server-side validation still applies.** A well-formed packet is not an
  accepted packet. The client only guarantees the message leaves correctly.
