# In-game overlay (Dear ImGui)

The packet viewer that draws on top of the game is built with
[Dear ImGui](https://github.com/ocornut/imgui). This replaces the older
hand-rolled GDI-to-texture renderer.

- ImGui version: **v1.92.9b** (vendored under `ConquerDX9Hook/libs/imgui`)
- Renderer backend: `imgui_impl_dx9` (DirectX 9)
- Platform backend: `imgui_impl_win32`

Only the files that are actually compiled were vendored (the core `.cpp`
files, the two backends we use, and their headers). `imgui_demo.cpp` and the
example projects are intentionally omitted. The upstream `LICENSE.txt` is kept
next to the sources.

## Why the backends are not used verbatim

`imgui_impl_dx9` is used as-is, but `imgui_impl_win32`'s per-frame entry point
is **not** called. Instead `src/hooks/imgui_bridge.cpp` fills `ImGuiIO`
directly. Two reasons:

1. **No application loop.** The overlay draws inside the game's `EndScene`
   hook. There is no `while (running) { … }` of our own, so nothing would call
   `ImGui_ImplWin32_NewFrame()` at a predictable time.
2. **Device resets.** On a resolution change the game destroys and recreates
   its swap chain. `ImGui_ImplWin32_NewFrame()` caches `io.DisplaySize` from
   the window client rect; the bridge instead re-derives the size and the
   main viewport every frame straight from the live swap chain.

Everything else - input queueing, the `VK_*` → `ImGuiKey` mapping, DPI, IME and
clipboard handling - is still the official `ImGui_ImplWin32_WndProcHandler()`,
so keyboard/character handling behaves exactly as upstream.

## Data flow

```
            game EndScene hook              window procedure hook
                    │                                │
                    ▼                                ▼
   PacketOverlay::OnEndScene            PacketOverlay::OnWindowMessage
                    │                                │
                    │                     ImGuiBridge::ProcessMessage
                    │                                │
                    │            ImGui_ImplWin32_WndProcHandler  (queue event)
                    │                                │
                    │                     return swallow? ──► game
                    ▼
          ImGuiBridge::NewFrame   (DisplaySize, DeltaTime, key mods, mouse pos)
                    │
                    ├─ ImGui::NewFrame()
                    ├─ PacketOverlay::DrawPanel()   (tables, hex dump)
                    ├─ ImGui::Render()
                    └─ ImGuiBridge::RenderDrawData()
                              │
                              └─ ImGui_ImplDX9_RenderDrawData()
                                       (saves D3D9 state via a state block,
                                        draws, then restores it)
```

The DX9 backend takes a `D3DSBT_ALL` state block around its draw calls, so the
game's render state is untouched.

## Controls

| Key / input   | Action |
|---------------|--------|
| `Insert`      | show / hide the panel |
| `F8`          | show / hide the panel (alias) |
| `F9`          | pause / resume capture |
| `F10`         | clear the list |
| `Esc`         | hide the panel |
| mouse wheel   | scroll the packet list / hex dump |
| click a row   | show that packet in the hex pane |
| drag the title bar | move the panel |
| drag a column edge | resize that column |

The hotkeys are **polled** with `GetAsyncKeyState` rather than handled as
window messages: the client drives its keyboard through DirectInput, so
`WM_KEYDOWN` is not reliably delivered to the window we subclass. Handling them
in both places would toggle twice per press.

## Panel layout

The window is a tabbed "bot panel" shell. The frame is the same on every tab:

```
PRESS [INSERT] to toggle overlay.
FPS: 128.4
[ Save Settings ]  Autosaves shortly after changes
──────────────────────────────────────────────────
 Player | Map | Packets | Misc | Plugins
──────────────────────────────────────────────────
 <tab content>
```

The packet logger lives in the **Packets** tab (the shell opens on it by
default). It shows a toolbar (`CAPTURING`/`PAUSED` + SEND/RECV/dropped totals),
a **filter bar**, the live 7-column capture table
(`# / TIME / DIR / LEN / ID / MESSAGE / BYTES`), the hex/ASCII dump of the
selected packet, and a status bar.

### Filtering

Every clause is ANDed, and a clause left at its neutral value is not applied —
so an untouched filter bar shows the whole ring. The master `Filter` checkbox
turns the whole bar on and off.

| control | matches |
|---------|---------|
| search box | substring of the name, `CMsg` class, meaning, or `0xNNNN` id |
| `Aa` | make the search box case-sensitive |
| id box | `0x0800` / `0800h` (hex) or `2048` (decimal) exact id; append `*` for a prefix (`0x07*`); anything else is a substring match |
| class box | substring of the `CMsg` class name |
| class combo | quick-pick from the classes actually present in the current capture |
| direction combo | `Any dir` / `SEND` / `RECV` |
| `Known only` | hide ids with no recovered name or class |
| from / to | seconds since capture start; `0` means unbounded |

`Copy N rows` copies every visible row to the clipboard as TSV (with a header
row, and `class` / `meaning` columns). `Copy hex` copies the selected packet's
hex dump plus a `#`-comment header. `Reset` clears the whole bar.

### Settings

The `Save Settings` button writes the filter state and selected tab to
**`overlay.ini`** next to the game exe. The panel also autosaves ~1.5 s after
the last change (that is what "Autosaves shortly after changes" refers to), and
reads the file back on the first frame.

### Catalogue metadata

Beyond the id → name map, `packet_names.h` now carries, per id: the `CMsg`
class name, the recorded direction and a one-line meaning. All of it is
generated from the same TSVs by `tools/gen_names.py`, so filtering by class or
meaning works for anything the catalogue knows about.

The other tabs are presentational shells for now:

| Tab | State |
|-----|-------|
| `Player`  | placeholders for status / inventory / skills |
| `Map`     | the template's Overview / Entities / Travel / Minimap layout |
| `Packets` | **the working packet logger** |
| `Misc`    | interface toggles + diagnostics |
| `Plugins` | plugin list placeholder |

Tab selection is driven by `g_requestedTab`, a one-shot "force this tab open"
request consumed by `BeginPanelTab()`. This matters because
`ImGuiTabItemFlags_SetSelected` is **sticky**: it queues focus toward the
flagged tab every frame it is passed, so applying it permanently to a fixed tab
would fight the user every time they clicked a different one. The request is
cleared as soon as the target tab opens. `g_activeTab` records which tab's
contents are showing, for logic that cares what the user is looking at.

## Files

| File | Role |
|------|------|
| `src/hooks/imgui_bridge.h` / `.cpp` | ImGui lifecycle + input; the only file that touches ImGui internals |
| `src/hooks/packet_overlay.h` / `.cpp` | the UI itself (tabbed panel shell + packet table + hex dump) |
| `src/hooks/directx_hooks.cpp` | forwards messages to `PacketOverlay::OnWindowMessage`, calls `OnEndScene`, and drives the reset path |
| `libs/imgui/…` | vendored Dear ImGui v1.92.9b |

## Building

The project file already lists every ImGui source and adds
`$(ProjectDir)libs\imgui` to the include path, so a normal build picks it up:

```
build.bat            # Release x86 -> Release\D3DX9_43.dll
```

Nothing else has to be installed - the DX9 and Win32 backends link `gdi32` and
`dwmapi` through `#pragma comment(lib, …)`, and XInput is loaded dynamically.

## Updating ImGui

1. Download the new release tarball.
2. Copy these into `libs/imgui` (overwriting):
   `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`,
   `imgui.h`, `imgui_internal.h`, `imconfig.h`,
   `imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h`, `LICENSE.txt`.
3. Copy `backends/imgui_impl_dx9.{cpp,h}` and
   `backends/imgui_impl_win32.{cpp,h}` into `libs/imgui/backends`.
4. If the project file lists a newly added file, add it there too; otherwise
   nothing else changes.
