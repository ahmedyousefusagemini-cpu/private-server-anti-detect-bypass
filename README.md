# ConquerDX9.Hook — packet monitor

A `D3DX9_43.dll` proxy that loads into Conquer.exe and shows the client's
**plaintext** network traffic in an in-game window.

The client encrypts every packet with the TQ stream cipher before it reaches
`ws2_32`, so hooking `send`/`recv` only ever shows ciphertext. Instead this
hooks the client's own socket layer:

| direction | hook target | why |
|---|---|---|
| outgoing | `CMyClientSocket::DoSendMsg` @ `0x012799A1` | reads the packet buffer *before* the cipher runs |
| incoming | `GetMsgType` helper @ `0x00D0C67B` | called from `DoReceive` once per packet, *after* decryption |

See [docs/packet-hooks.md](docs/packet-hooks.md) for the full Ghidra
derivation, the cipher layout, and how to re-derive the addresses after a
client update.

## The window

Drawn on top of the game from inside the `EndScene` hook. Rendering is plain
GDI into a 32-bit DIB, uploaded to a `D3DPOOL_MANAGED` texture and drawn as a
screen-space quad — no ImGui, no D3DX, no extra dependencies.

```
+-- Conquer Packet Monitor -------------------- SEND 412  RECV 1180  dropped 0 --+
| #      TIME       DIR   LEN   ID      BYTES                                    |
| 000412 03:14.882  SEND  0x23  0x0001  23 00 01 00 0C 00 00 00 41 42 43 44 ...  |
| 000413 03:14.905  RECV  0x1F  0x0002  1F 00 02 00 04 00 00 00 0D 0A 00 00 ...  |
+--------------------------------------------------------------------------------+
| #000412  SEND  len=35  id=0x0001  t=03:14.882                                  |
| 0000  23 00 01 00 0C 00 00 00  41 42 43 44 45 46 47 48  |#.......ABCDEFGH|     |
| 0010  49 4A 4B 4C 4D 4E 4F 50  51 52 53 00 00 00 00 00  |IJKLMNOPQRS.....|     |
+--------------------------------------------------------------------------------+
| showing 1024 of 1592 retained  |  wheel scrolls  |  click a row for the hex dump |
+--------------------------------------------------------------------------------+
```

SEND rows are orange, RECV rows are green. The packet id column is the `uint16`
immediately after the length header.

### Controls

| input | action |
|---|---|
| `F8` | show / hide the window |
| `F9` | pause / resume capture |
| `F10` | clear the list |
| `Esc` | hide the window |
| mouse wheel | scroll (scrolling up detaches auto-follow) |
| click a row | select it — hex dump appears in the bottom pane |
| drag the title bar | move the window |
| middle click | hide the window |

The window swallows mouse input while the cursor is over it, so clicking a row
does not also move your character.

## The log file

Every captured packet is also appended to `packets.log` next to the game
executable, so traffic can be grepped or diffed after the session:

```
===== session started 2026-10-01 02:14:07 =====
[02:14:07.882] SEND len=35 id=0x0001
  0000  23 00 01 00 0C 00 00 00  41 42 43 44 45 46 47 48  |#.......ABCDEFGH|
  0010  49 4A 4B 4C 4D 4E 4F 50  51 52 53 00 00 00 00 00  |IJKLMNOPQRS.....|
[02:14:07.905] RECV len=31 id=0x0002
  0000  1F 00 02 00 04 00 00 00  0D 0A 00 00 00 00 00 00  |................|
```

The file is flushed after every packet, so a crash mid-session does not lose
what was already captured.

## Configuration

`packet_log.ini`, created next to the game executable on first run:

```ini
enabled=1      ; 1 = install the hooks at all
overlay=1      ; 1 = show the in-game window on startup
file=1         ; 1 = append every packet to packets.log
filebytes=256  ; bytes dumped per packet in the log (16 .. 2048)
```

If the log file grows faster than you want, lower `filebytes` or set `file=0`
and use the window only.

## Layout

```
ConquerDX9Hook/
  src/
    dllmain.cpp              proxy entry point + hook bootstrap thread
    hooks/
      common.h               shared types (EndScene/Reset signatures, window info)
      utils.cpp              memory pattern scanner, window lookup
      proxy.cpp              D3DX9_43 export forwarding
      log.cpp / log.h        hook_init.log diagnostics
      directx_hooks.cpp      EndScene / Reset / WndProc hooks
      packet_capture.cpp/.h  packet hooks + ring buffer + packets.log writer
      packet_overlay.cpp/.h  the in-game window
  libs/minhook/              MinHook (built from source, not the prebuilt lib)
docs/packet-hooks.md         Ghidra derivation of the two hook addresses
```

## Build

```
build.bat                  # Release x86 -> Release\D3DX9_43.dll
build.bat Debug x86
```

`build.bat` locates MSBuild with `vswhere` and pins toolset `v143`, so it works
from any VS2022 install that has the "Desktop development with C++" workload.
It then copies the DLL over the RDP share to `\\tsclient\H\client\Env_DX9`.

Deploy `D3DX9_43.dll` next to `Conquer.exe`. Delete `hook_init.log` and
`packets.log` between runs if you want clean output.

## Failure behaviour

Both hook targets are verified against a byte signature before MinHook patches
them. On a client build that does not match, the hook is skipped and the reason
is written to `hook_init.log`:

```
[Capture] SEND signature mismatch at 012799A1 - client build differs, hook skipped
```

The client is never patched blindly, so a mismatched build degrades to "no
packets shown" rather than a crash.
