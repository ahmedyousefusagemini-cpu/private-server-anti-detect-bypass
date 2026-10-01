# Send-side wire-id resolution — Conquer.exe build 7952

Scope: resolve the four outbound ids **0x0796, 0x097D, 0x089D, 0x0805**, prove
whether send numbering equals receive numbering, and narrate the login
handshake. All addresses are VAs (image base 0x00400000), Ghidra program
`Conquer.exe`; binary on disk `H:\client\Env_DX9\Conquer.exe` (29,954,480 B).

---

## 1. Verdict on the four ids

| id | result | name / meaning | evidence |
|----|--------|----------------|----------|
| **0x089D** | **RESOLVED (hard)** | client **machine/hardware-fingerprint** message (a "DxCheck"-style upload) | `Create` at **0x00d95b1f** stores `0xBC`(188) at `[+0]` and **`0x089D`** at `[+2]`; `ParseFromArray` at **0x00d95c3a** checks `GetType()==0x89d`; vtable **0x016ff7c8**. Matches log SEND 0x089D len=188 with the hex string `502E91C8CA8E` produced by the in-function format `"%02X%02X%02X%02X%02X%02X"` (@0x016ff7e4). |
| **0x0796** | **RESOLVED (inferred, high confidence)** | **CMsgAccountEx** — the 472-byte account/auth blob ("Login Send 1") | Only in-exe 472-byte login upload is `CMsgAccountEx::Create` = **0x00f89208**, body length **0x1d8 = 472**; built by `ndac.dll` `Ordinal_55`. Class name string `"Login Send 1 : CMsgAccountEx"` @0x01733948; vtable **0x01725218**. PacketViewer annotation: *"LoginAuth C->S — Login/auth upload (472B) … Account auth blob"* and *"Triggers 0x0796/0x097D"*. The literal `0x0796` is **not** present in `.text`/`.vlizer`/any DLL — it is assigned at runtime inside `ndac.dll`. |
| **0x097D** | **RESOLVED (inferred, high confidence)** | **login-server connect/auth reply** (52-byte), the `ndac.dll` counterpart of CMsgConnect | Log SEND 0x097D len=52, immediately after RECV 0x0665 (324 B SessionData). The in-exe game connect reply `CMsgConnect::Create` = **0x00f4119e** writes id **0x41C** with size 0x30 (48 B body → **52 B total**), published by `CMsgConnectEx::Process` = **0x00f5f823**. Same 52-byte shape, different numbering → the login path uses the `ndac.dll` id 0x097D. Literal not in any binary. |
| **0x0805** | **NOT RESOLVED — absent from the capture** | unknown; likely unused on this build | `0x0805` appears in **no** captured packet (`docs/packet-ids.md`: SEND ids go 0x0796, 0x097D, 0x089D … with no 0x0805) and is a structural hole in the receive switch (`0x0803 → 0x0807`, cases 0x0804/0x0805/0x0806 absent). No `MOV reg,imm32` / `PUSH imm32` / `[+6]` store anywhere. |

**Bottom line:** 0x089D is proven statically; 0x0796 and 0x097D are pinned by
size + class-name + tool-annotation triangulation (the ids themselves live in
the delay-loaded `ndac.dll`, which is **not on disk**); 0x0805 cannot be
resolved because it never appears on the wire in this capture.

---

## 2. Why three ids are not in the exe (structural reason)

`CNetMsg`/`CMsg` base classes do **not** override `GetType()`; the base
implementation (`0x00d0c687`) is `return *(u16*)(this+6)`. The wire id is
therefore a **stored** value, written at `msg+6` inside each class's `Create`.
So an id is discoverable in the image **only if its `Create` is in the image**.

The login/account ids are produced by `ndac.dll` (delay-load name read from
`ImgDelayDescr_019ec050` → DLL-name RVA `0x012f6e80` → string **`ndac.dll`**).
Entry points `Ordinal_8` / `Ordinal_55` are delay-load thunks
(`0x01278d34` = `MOV EAX,0x1a64030; JMP 0x00d0c4f1` →
`___delayLoadHelper2_8(&ImgDelayDescr_019ec050, …)`). `ndac.dll` is resolved at
runtime and is not present in `H:\client\Env_DX9\`, so its id constants cannot
be scanned.

**Ruling-out evidence for the literals** (all negative):
- `MOV EAX/ECX/EDX, imm32` for 0x0796/0x097D/0x089D/0x0805 in `.text` and
  `.vlizer`: **none** are real id stores.
- `PUSH imm32`, `CMP`, `66 C7 [reg+disp], imm16`, `66 B8+r, imm16`, direct or
  indirect `MOV word [reg+6], r16`: **none**.
- Every `.dll`/`.exe` in the client dir was scanned the same way. Apparent hits
  (`Conquer.exe` 0x42c1a9, 0x715532, 0x994f5b, 0x49d3a0, 0x1041b2) were
  disassembled and are **coincidental bytes inside other instructions**
  (e.g. 0x42c1a9 = `MOV EDI,0x1503d6c`; 0x715532 = `PUSH 0x15b1bc8`). A later
  VA↔file-offset error was caught: the real VA of the 0x089D store is
  **0x00d95b4e**, not 0x994f4e.

---

## 3. Proof of the send-side Create pattern (0x089D)

`Create` @ **0x00d95b1f** (Ghidra did not auto-define it; range
0x00d95b1f–0x00d95c38, `RET 0x14`):

```
0x00d95b43  CALL 0x00d0c68c                 ; clear [+4]=0, wipe 0x3fc body bytes
0x00d95b48  MOV  EAX,[ESI+0x404]            ; EAX -> header area
0x00d95b4e  MOV  ECX,0xbc                   ; 188 = total length
0x00d95b58  MOV  word ptr [EAX],CX          ; [+0] = size (188)
0x00d95b5b  MOV  ECX,0x89d                  ; WIRE ID
0x00d95b60  MOV  EAX,[ESI+0x404]
0x00d95b66  MOV  word ptr [EAX+2],CX        ; [+2] = id 0x089D  (== msg+6)
...
0x00d95c11  PUSH 0x016ff7e4                 ; "%02X%02X%02X%02X%02X%02X"
0x00d95c19  CALL 0x00470529                 ; format 6 raw bytes -> 12 hex chars
```

`ParseFromArray` @ **0x00d95c3a**:
`FUN_010b89bc(protobuf)` then `(**(*this+4))() == 0x89d` — self-identifies by id.
Vtable **0x016ff7c8**: `0x00d8ccec`(dtor) `0x00d0c687`(GetType)
`0x00d0c676`(GetSize) `0x00d95c3a`(Parse) `0x010befa9` `0x010d9966`(Send)
`0x010d0b2e`(Process). Object size 0x408.

Semantics: writes `[+0x04]` dword, `[+0x47]`=0, `[+0x88]`/`[+0x8a]` words,
`[+0x8c]` dword, `[+0x90..0x95]` six bytes rendered as `%02X`*6, `[+0xb8]`=0.
The `\\.\Pipe\zfDxCheck-%s` string in the account path (0x0173396c region) is
consistent with a Dx/anti-cheat fingerprint upload.

---

## 4. Send-side id → name table

Send numbering **equals** receive numbering: the same class supplies the id on
both directions (e.g. `CMsgWalk` = **0x0898** both ways, `Create` @ 0x00eb442c).
Confirmed by 0x089D: `Create` writes 0x089D and the packet is observed as a
SEND with that id.

| id | name | create / evidence |
|------|------|-------------------|
| 0x0796 | **CMsgAccountEx** (account/auth blob, 472 B) | `FUN_00f89208` (body 0x1d8); ndac `Ordinal_55`; vtable 0x01725218 |
| 0x0898 | CMsgWalk | `FUN_00eb442c` (`network\msgwalk.cpp`) |
| 0x089D | machine-fingerprint / DxCheck upload (188 B) | `0x00d95b1f`; parse `0x00d95c3a`; vtable 0x016ff7c8 |
| 0x097D | login connect/auth reply (52 B, ndac) | mirrors `CMsgConnect` `0x00f4119e` (id 0x41C, 52 B) |
| 0x0833 | CMsgAction (attack/interact action) | PacketViewer annotation; 255 sends |
| 0x0838 | CMsg… (combat/pos sync) | observed SEND |
| 0x0867 | toggle (`f1=0,f2=271`) | observed SEND |
| 0x07E4 | echoed 32 B pair | observed SEND |
| 0x0817 | `f1=1` (6 B) | observed SEND |
| 0x08B2 | `f1=7,f2=37341` | observed SEND |
| 0x08DE | 172 B state blob (`f1=0xa469ee00`) | observed SEND |
| 0x0923 | 40–136 B (echoed) | observed SEND |
| 0x0988 | 25 B walk/move (`f1… f4=448,f5=270,f8=3`) | observed SEND |
| 0x0942 | 111–901 B data upload | observed SEND |
| 0x09F9 | `f1=4,f9=1,f10=4` | observed SEND |
| 0x0A2C | `f1=2,f2=<pid>` | observed SEND |
| 0x0A36 | CMsgItemPing (12 B) | receive `fullmap.tsv`; also SEND |
| 0x08F6 | 520 B frame | observed SEND |

(The full 193-entry `id → Create-site` table produced from `.text`/`.vlizer`
byte-scanning lives in `send_ids_scan.txt`; every entry there has a concrete
`MOV …,imm16` + `MOV word [reg+6],…` store, so it is hard evidence.)

---

## 5. Login handshake narrative

Envelope `[u16 total_len][u16 msg_id][protobuf body]`. The 0x4xx/0x6xx ids are
the **login server** (server type != 2 branch of `CNetMsg::CreateNetMsg`
= `FUN_00f41fce`); the 0x8xx/0xAxx ids are the **game server**.

| step | dir | len | id | what it carries / why |
|------|-----|-----|----|------------------------|
| 1 | RECV | 8 | 0x0423 | **CMsgEncryptCode** (ctor `0x010accf2`, Process `0x010cb5e7`). Server seed: 16-byte key table @`DAT_019fb238`; seeds the client crypto. Logs "Login Receive 1". |
| 2 | **SEND** | 472 | **0x0796** | **CMsgAccountEx** (`Create` `0x00f89208`, ndac `Ordinal_55`). Account/auth blob ("Login Send 1"): reads `res.dat`, builds a 0x1d8-byte body, pulls the client key via `FUN_01113208`, files the packet header from `[+0x404]+0x114`. Opaque (non-protobuf) → fields `f2571974644978`/`f5025199` look random. |
| 3 | RECV | 8 | 0x0674 | **CMsgPreLoginResp** (ctor `0x00fbdbf4`, Process `0x00fc316c`). Server's pre-login acknowledgement of the account blob. |
| 4 | RECV | 324 | 0x0665 | **CMsgConnectEx** (ctor `0x00f32257`, Process `0x00f5f823`). 324 B = "SessionData": *repeated 8-byte key blocks + 32-char hex session hash + pad* — the server's session challenge / server list + key material. |
| 5 | **SEND** | 52 | **0x097D** | Login-side connect/auth **reply** to the session challenge. 52 B = 48-byte body + 4-byte header, same shape as `CMsgConnect::Create` `0x00f4119e` (which writes id 0x41C, size 0x30), but the login/ndac numbering is 0x097D. |
| 6 | RECV | 55 | 0x0939 | **CMsgTalk** (`fullmap.tsv` 0x0939 = CMsgTalk). First server chat/system line (`f1=0xFFFFFF`, `f2` channel…). Marks the connection reaching the game world. |
| 7 | RECV | 12 | 0x0953 | **CMsgServerInfo** (0x0953 = CMsgServerInfo). Compact server info/ack. |
| 8 | RECV | 516 | 0x093E | **CMsgUserCityInfo** (0x093E = CMsgUserCityInfo). 516 B = the player's city/zone payload (large world/state block). |
| 9 | **SEND** | 188 | **0x089D** | **Machine-fingerprint / DxCheck upload** (`Create` `0x00d95b1f`). Fires ~14 s after login (02:15:11 vs 14:57), embedding 6 raw bytes hex-encoded (`%02X`*6 → `502E91C8CA8E`). Anti-cheat client attestation once the world is entered. |

Why the order matters: 0x0423 hands the client the crypto seed; the client
answers the account challenge with the 472 B CMsgAccountEx (0x0796); the server
then sends the pre-login ACK (0x0674) plus the 324 B session challenge
(0x0665), which the client answers with the 52 B 0x097D; the world opens
(0x0939/0x0953/0x093E), and only then does the client upload its
hardware/anti-cheat fingerprint (0x089D).

---

## 6. Exact evidence addresses

- **0x00d95b1f** — Create for 0x089D (`MOV ECX,0x89d` @ **0x00d95b5b**;
  size `MOV ECX,0xbc` @ **0x00d95b4e**).
- **0x00d95c3a** — ParseFromArray for 0x089D (`GetType()==0x89d`).
- **0x016ff7c8** — vtable of the 0x089D class; `"%02X"*6` string @ **0x016ff7e4**.
- **0x00f89208** — CMsgAccountEx::Create (body `0x1d8` = 472) → ndac `Ordinal_55`.
- **0x01725218** — CMsgAccountEx::vftable (GetType = base `0x00d0c687`).
- **0x01733948** — `"Login Send 1 : CMsgAccountEx"`; dispatch in `FUN_010262fb`
  (mode 3) alongside `CMsgAccountPoker`/`CMsgAccountByQRCode`.
- **0x00f4119e** — CMsgConnect::Create (id **0x41C**, `0x30` body → 52 B);
  **0x00f3221b** — CMsgConnect ctor; **0x00f5f823** — CMsgConnectEx::Process
  (calls 0x00f3221b → 0x00f4119e → SendMsg **0x010d9966** @0x00f5fb00).
- **0x00d0c687** — base `GetType()` = `return *(u16*)(this+6)`.
- **0x00d0c68c** — Create helper: clear `[+4]` and 0x3fc body bytes.
- **0x01278d34 / 0x01278d3e** — ndac `Ordinal_55` delay-load thunk
  (`ImgDelayDescr_019ec050`, DLL name `ndac.dll` @ 0x016f6e80).
- **`docs/packet-ids.md`** — captured SEND 0x0796×2 (472 B), 0x097D×2 (52 B),
  0x089D×1 (188 B, string `502E91C8CA8E`); **no 0x0805**.
- **`H:\client\Env_DX9\PacketViewer.exe`** — annotations `LoginAuth C->S
  (472B)`, `SessionData (324B) … Triggers 0x0796/0x097D`, `AuthReply (48B)`.

## 7. Residual uncertainty

- 0x0796 and 0x097D: the **classes** are certain (CMsgAccountEx; login connect
  reply) and the **ids match the capture**, but the literal id assignment sits
  in `ndac.dll`, which is absent → these two are labelled INFERRED, not proven
  by a `return 0xNNNN`/store in the image.
- 0x0805: **unresolved and unresolvable** from the available evidence — it is
  not on the wire in this capture and not present as a constant anywhere,
  including every shipped DLL. It is most likely an unused/legacy id on this
  build.
