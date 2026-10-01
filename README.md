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

An [Dear ImGui](https://github.com/ocornut/imgui) window drawn on top of the
game from inside the `EndScene` hook, using the official `imgui_impl_dx9` and
`imgui_impl_win32` backends (vendored under `ConquerDX9Hook/libs/imgui`). The
DX9 backend wraps its draw calls in a `D3DSBT_ALL` state block, so the game's
render state is restored untouched.

The window is a tabbed **bot panel** shell. Every tab shares the same frame
(hint line, FPS readout, `Save Settings` button, tab bar); the packet logger
lives in the **Packets** tab, which the panel opens on by default.

```
+-- Manager ---------------------------------------------------------------[X]--+
| PRESS [INSERT] to toggle overlay.                                              |
| FPS: 128.4                                                                     |
| [ Save Settings ]  Autosaves shortly after changes                             |
+--------------------------------------------------------------------------------+
|  Player | Map | [ Packets ] | Misc | Plugins                                   |
+--------------------------------------------------------------------------------+
| CAPTURING | SEND 412  RECV 1180  dropped 0        [F9] pause  [F10] clear      |
| #      TIME       DIR   LEN  ID      MESSAGE         BYTES                     |
| 000412 03:14.882  SEND  35   0x0001  Talk            23 00 01 00 0C 00 ...     |
| 000413 03:14.905  RECV  31   0x0002  UserInfo        1F 00 02 00 04 00 ...     |
+--------------------------------------------------------------------------------+
| #000412  SEND  len=35  id=0x0001  Talk  t=03:14.882                            |
| protobuf body follows the 4-byte header (len, id):                             |
| 0000  23 00 01 00 0C 00 00 00  41 42 43 44 45 46 47 48  |#.......ABCDEFGH|     |
| 0010  49 4A 4B 4C 4D 4E 4F 50  51 52 53 00 00 00 00 00  |IJKLMNOPQRS.....|     |
+--------------------------------------------------------------------------------+
| showing 1024 of 1592 retained  |  following newest  |  click a row for the dump |
+--------------------------------------------------------------------------------+
```

| Tab | State |
|-----|-------|
| `Player`  | placeholder (status / inventory / skills) |
| `Map`     | the template's Overview / Entities / Travel / Minimap layout |
| `Packets` | **the working packet logger** |
| `Misc`    | interface toggles + diagnostics |
| `Plugins` | plugin list placeholder |

### Filtering the packet list

The Packets tab has a filter bar above the table. Every clause is ANDed and a
clause at its default is ignored:

- **search** — substring of the name, `CMsg` class, meaning or `0xNNNN` id
  (`Aa` makes it case-sensitive)
- **id** — `0x0800` / `0800h` / `2048` for an exact id, `0x07*` for a prefix
- **class** — substring of the `CMsg` class, with a quick-pick combo built from
  the classes present in the current capture
- **direction** — `Any dir` / `SEND` / `RECV`
- **Known only** — hide ids with no recovered name or class
- **from / to** — a window in seconds since capture start (`0` = unbounded)

`Copy N rows` puts every visible row on the clipboard as TSV; `Copy hex` copies
the selected packet's hex dump; `Reset` clears the bar. The `Save Settings`
button (and a ~1.5 s autosave) persists the filter and selected tab to
`overlay.ini` next to the game exe.

SEND rows are amber, RECV rows are green. The **MESSAGE** column is the
human-readable name recovered from the client's own dispatch table (see
[docs/packet-catalog.md](docs/packet-catalog.md)); unknown ids fall back to
`Unknown (0xNNNN)`. Columns are resizable by dragging their edge.

See [docs/overlay-imgui.md](docs/overlay-imgui.md) for how the ImGui backends
are wired into the game's hook and reset path.

### Controls

| input | action |
|---|---|
| `Insert` | show / hide the panel |
| `F8` | show / hide the panel (alias) |
| `F9` | pause / resume capture |
| `F10` | clear the list |
| `Esc` | hide the panel |
| mouse wheel | scroll (scrolling up detaches auto-follow) |
| click a row | select it — hex dump appears below the table |
| drag the title bar | move the panel |
| drag a column edge | resize that column |
| middle click | hide the panel |

The panel is a normal ImGui window, so it can also be closed with its title-bar
button and resized from any edge.

The hotkeys are **polled with `GetAsyncKeyState` once per frame**, not handled
in the window procedure. Conquer drives its keyboard through DirectInput, so
`WM_KEYDOWN` is not reliably delivered to the window the overlay subclasses —
a message-based handler simply never fires. Polling the physical key state
works regardless of how the client consumes input. Keys are only acted on
while the game process owns the foreground, so the overlay will not toggle
while you are typing in another window.

ImGui itself decides whether to consume mouse and keyboard input: clicking the
panel does not also move your character, and typing in a text field does not
reach the game.

## The log file

Every captured packet is also appended to `packets.log` next to the game
executable, so traffic can be grepped or diffed after the session:

```
===== session started 2026-10-01 02:14:07 =====
[02:14:07.882] SEND len=35    id=0x0001  Talk
  0000  23 00 01 00 0C 00 00 00  41 42 43 44 45 46 47 48  |#.......ABCDEFGH|
  0010  49 4A 4B 4C 4D 4E 4F 50  51 52 53 00 00 00 00 00  |IJKLMNOPQRS.....|
[02:14:07.905] RECV len=31    id=0x0002  UserInfo
  0000  1F 00 02 00 04 00 00 00  0D 0A 00 00 00 00 00 00  |................|
```

Each header line ends with the recovered message name, so the log is readable
without the window. The file is flushed after every packet, so a crash
mid-session does not lose what was already captured.

For a much richer offline decode - protobuf field breakdowns, per-id totals, a
full catalogue of every id seen - see [`tools/packet_log.py`](tools/packet_log.py)
and [docs/packet-catalog.md](docs/packet-catalog.md).

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
      packet_names.h         generated id -> CMsg<Name> table (see tools/gen_names.py)
      imgui_bridge.cpp/.h    Dear ImGui lifecycle + input (see docs/overlay-imgui.md)
      packet_overlay.cpp/.h  the in-game window (ImGui)
  libs/minhook/              MinHook (built from source, not the prebuilt lib)
  libs/imgui/                Dear ImGui v1.92.9b (core + dx9/win32 backends)
tools/
  packet_log.py              offline decoder for packets.log (names + protobuf)
  gen_names.py               regenerates packet_names.h from tools/data/*.tsv
  gen_catalog.py             regenerates docs/packet-catalog.md
  data/ids_recv.tsv          id -> CMsg<Name> (recovered, 470 entries)
  data/overrides.tsv         curated friendly names + descriptions
docs/packet-hooks.md         Ghidra derivation of the two hook addresses
docs/packet-catalog.md       every message id, its name and its meaning
docs/overlay-imgui.md        how ImGui is wired into the hook / reset path
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
