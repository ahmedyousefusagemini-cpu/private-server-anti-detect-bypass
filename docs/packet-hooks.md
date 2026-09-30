# Conquer.exe packet hooks — derivation notes

How the two hook addresses in `packet_capture.cpp` were found, and how to
re-derive them if the client is ever updated.

Target: **Conquer.exe build 7952**, `x86:LE:32:default`, image base
`0x00400000`, 208,908 functions, `H:\client\Env_DX9\Conquer.exe`.

Everything below is static analysis only (Ghidra). No debugger was attached to
the client.

---

## 1. Why not hook `ws2_32`

`ws2_32.dll` is where the bytes hit the wire — i.e. *after* the client has
encrypted them and *before* it has decrypted the reply. Hooking `send`/`recv`
therefore only ever shows ciphertext.

The client imports WS2_32 **by ordinal** (not by name), so the import table has
no `"send"`/`"recv"` strings at all:

| import | ordinal | IAT slot |
|---|---|---|
| `send`  | 19 | `0x015008A8` |
| `recv`  | 16 | `0x015008B0` |

`get_xrefs_to` on the `send`/`recv` externals gives every call site in the
binary:

```
send : FUN_010b9b99, FUN_012790a2, FUN_0121fac1
recv : FUN_0127980f, FUN_0127946d, FUN_010bc5f4, FUN_0121fac1
```

`FUN_0121fac1` turns out to be the **updater**, not the game protocol — it
opens a socket, sends a small text request and `strstr`s the reply for
`"UPDATE"`. Ignore it.

That leaves the pair around `0x01279xxx`, which is the real client socket.

---

## 2. Identifying the network layer

RTTI and log strings pin the classes down:

| symbol | address |
|---|---|
| `.?AVCMyClientSocket@@` (type descriptor name) | `0x01A63CE4` |
| `.?AVCMyNetwork@@` | `0x01A5D750` |
| `.?AVCCipher@@` | `0x01A63DFC` |
| `"CMyClientSocket::DoSendMsg Check Size Failed %u"` | `0x0175F998` |
| `"CMyClientSocket::DoReceive CheckMsg Failed"` | `0x0175FAB0` |
| `"DoReceive1 recv return 0 break"` / `"…return -1 break"` | `0x0175FA30`, `0x0175FA50` |
| `"CMyNetwork::FuncRecvSend DoSendMsg Failed - LastError[%d]"` | `0x01735DA0` |

`decompile_function` on `FUN_0127946d` returns source paths, which is the
cleanest confirmation of what we are looking at:

```
g:\core\cqcore\pcsocketcore\cqcore\classes\myclientsocket.cpp
```

So the module is the classic Conquer `CMyClientSocket` (`myclientsocket.cpp`),
with `CMyNetwork::FuncRecvSend` as the reader/writer thread.

---

## 3. The cipher

`FUN_01279C91` is the TQ stream cipher. Disassembly:

```
01279c96  MOV  EDX,ECX                     ; EDX = cipher context
01279cac  MOV  EAX,[EDX]                   ; i
01279cae  MOV  AL,[EAX+EDX+0x8]            ; key1[i]
01279cb2  XOR  byte ptr [ESI+EDI],AL       ; buf[k] ^= key1[i]
01279cb5  MOV  EAX,[EDX+4]                 ; j
01279cb8  MOV  AL,[EAX+EDX+0x108]          ; key2[j]
01279cbf  XOR  byte ptr [ESI+EDI],AL       ; buf[k] ^= key2[j]
01279cc7  INC  dword ptr [EDX]             ; ++i, wraps at 0x100
01279cd0  INC  dword ptr [EDX+4]           ; ++j, wraps at 0x100
01279cdc  MOV  AL,[ESI+EDI]
01279ce1  SHR  AL,4
01279ce4  SHL  CL,4
01279ce7  ADD  CL,AL                       ; nibble swap
01279ce9  XOR  CL,0xAB
01279cec  MOV  byte ptr [ESI+EDI],CL
01279d07  RET  0xC
```

Signature: `__thiscall void cipher(void* ctx /*ECX*/, uint8_t* buf, int len, char commit)`.

Context layout: `+0x00` = index i, `+0x04` = index j, `+0x08` = key1[256],
`+0x108` = key2[256]. `commit == 0` restores i/j (peek mode).

**The client keeps two separate contexts**, which is how one function serves
both directions:

| direction | cipher context | call site |
|---|---|---|
| encrypt (send) | `CMyClientSocket + 0x1CD8` | `0x01279AC9` |
| decrypt (recv) | `CMyClientSocket + 0x1EE0` | `0x01279567`, `0x01279683` |

Callers of `FUN_01279C91`: `FUN_0127946d` (recv), `FUN_012799a1` (send),
`FUN_01279ae8` (first-message init).

We deliberately do **not** hook the cipher. It needs direction detection and
runs on every byte; hooking the callers gives us the same plaintext at a much
saner granularity.

---

## 4. Wire format

Every packet is `[uint16 total_length][uint16 message_id][body]`, where
`total_length` includes the 4-byte header. This falls out of the receive path:

```
iVar6 = (local_58 - 2) + (local_4c & 0xffff);   // read len-2 more after the header
```

and out of `FUN_00d0c67b`, which is literally
`return *(uint16*)(packet + 2);` — i.e. read the message id that follows the
length.

---

## 5. Hook point A — outgoing (`DoSendMsg`)

`FUN_012799A1`, reached through the `CMyClientSocket` vtable (hence zero static
callers). Disassembly, trimmed:

```
012799aa  MOV  EDI,[EBP+8]        ; EDI = CMsg*
012799b3  LEA  EBX,[EDI+4]        ; EBX = msg + 4   <-- packet buffer
012799b9  CALL [EAX+8]            ; CMsg::GetSize()
012799bc  MOVZX ECX,word [EBX]    ; length = *(uint16*)(msg+4)
012799c8  CMP  ECX,EAX            ; must equal GetSize()
012799ef  LEA  EBX,[ESI+0x1008]   ; staging buffer
01279a05  CALL 0x0126e679         ; memcpy(this+0x1008, msg+4, length)
01279a0a  MOV  ECX,[ESI+0x1c08]   ; server type
   ... type 1/3 ...
01279ac2  PUSH EBX                ; buf  = this+0x1008
01279ac1  PUSH EDI                ; len
01279abf  PUSH 1                  ; commit
01279ac3  LEA  ECX,[ESI+0x1cd8]   ; encrypt context
01279ac9  CALL 0x01279c91         ; <-- ENCRYPT happens here
01279ad5  CALL 0x012790a2         ; raw send()
```

`CMsg` layout: `[0..3]` vtable, `[4..5]` `uint16` total length, `[6..]` body.
Because the cipher runs on `this+0x1008` **after** the memcpy, hooking the
function *entry* and reading `msg+4` yields the outgoing plaintext.

- **Address `0x012799A1` → RVA `0x00E799A1`**
- `__thiscall int DoSendMsg(CMyClientSocket* this, CMsg* msg)`
- Prologue `55 8B EC 83 EC 0C 53 56 57 8B 7D 08`

## 6. Hook point B — incoming (`DoReceive`)

`FUN_0127946D` is `CMyClientSocket::DoReceive`. It reads the 2-byte header,
decrypts it, reads the rest into `this+0x80A`, decrypts in place, then hands the
finished plaintext packet to a tiny helper:

```
01279677  PUSH 1
01279679  PUSH [EBP-0x4c]         ; len  = *(uint16*)packet
0127967c  LEA  ECX,[EDI+0x1ee0]   ; decrypt context
01279682  PUSH EBX                ; buf  = this+0x80a
01279683  CALL 0x01279c91         ; <-- DECRYPT happens here
01279688  MOVZX EAX,word [EBP-0x48]
0127968c  LEA  EBX,[EDI+0x808]    ; plaintext packet
01279692  PUSH EAX
01279693  PUSH EBX
01279694  CALL 0x00d0c67b         ; GetMsgType(packet, len)
```

`FUN_00D0C67B` in full:

```
00d0c67b  PUSH EBP
00d0c67c  MOV  EBP,ESP
00d0c67e  MOV  EAX,[EBP+8]        ; packet
00d0c681  MOV  AX,word [EAX+2]    ; return *(uint16*)(packet+2)
00d0c685  POP  EBP
00d0c686  RET
```

It has exactly one caller (`DoReceive`) and is reached once per packet with the
**already decrypted** buffer, so it is the cleanest possible receive-side
probe.

- **Address `0x00D0C67B` → RVA `0x0090C67B`**
- `__cdecl uint16 GetMsgType(const uint8_t* packet, int len)` (caller cleans
  the stack — the call site is followed by two `POP ECX`)
- Prologue `55 8B EC 8B 45 08 66 8B 40 02 5D C3`

The type-2 server path decrypts via `FUN_01279f68` instead, but still funnels
into this same helper, so one hook covers every server type.

---

## 7. Re-deriving after a client update

1. `search_strings "CMyClientSocket::DoSendMsg Check Size Failed"` and
   `"DoReceive1 recv return 0 break"`; `get_xrefs_to` each string to land in
   the two methods.
2. In `DoSendMsg`, find the `memcpy` whose source is `msg+4` and whose length
   is `*(uint16*)(msg+4)`; the function entry is the hook address.
3. In `DoReceive`, find the call immediately after the final cipher call that
   takes `this+0x808` as its first argument; that callee is the receive probe
   (verify it is `MOV EAX,[EBP+8]; MOV AX,[EAX+2]; RET`).
4. Update the two RVAs **and** the two signature arrays in
   `packet_capture.cpp`. If a signature does not match, the hook is skipped and
   the reason is written to `hook_init.log` — the client is never patched
   blindly.

RVAs are used rather than absolute addresses so the hooks still resolve if the
executable is ever built with a dynamic base.

---

## 8. Known gaps

- The handshake path (`CMyClientSocket::DoReceiveShakeHand`, `FUN_0127980F`)
  does not go through `GetMsgType`, so handshake frames are not captured.
- `InitFirstMsg` (`FUN_01279AE8`) encrypts the first 12 bytes directly; that
  first frame is likewise not captured.
- The capture hooks the *client's* view. Frames the client never sends (e.g.
  server-side pushes that arrive before the socket is up) are obviously absent.
