# D3DX9_43 side-loading proxy

A minimal **DLL side-loading proxy**. It builds a `D3DX9_43.dll` that a game
loads *instead of* the real Direct3D 9 extension library, while every one of
the real library's exports is forwarded straight through to the original. The
proxy module is then free to run its own code inside the target process.

This repo is deliberately scoped to the proxy mechanism and its payload hook.
The forwarding DLL and its entry point are here, plus one payload wired into
it: the **Frida Gadget**, loaded from a worker thread. See
[USAGE.md](USAGE.md) for the full workflow.

## How it works

Windows resolves a DLL import by searching the application directory **before**
the system directory. So if a `D3DX9_43.dll` sits next to `Conquer.exe`, the
loader picks it up ahead of the copy in `C:\Windows\SysWOW64`.

The proxy exploits that ordering:

1. Rename the game's original `D3DX9_43.dll` to `D3DX9_43_org.dll` (same folder).
2. Build this project (`Release|x86`) — the output is `D3DX9_43.dll`.
3. Drop the built `D3DX9_43.dll` next to `Conquer.exe`.

When the game imports a `D3DX*` symbol it now binds to **our** module. Each
export is a *linker-level forwarder* — `#pragma comment(linker, "/export:...")`
in [`src/proxy.cpp`](ConquerDX9Hook/src/proxy.cpp) — that points at the same
named export in `D3DX9_43_org.dll`:

```c
#pragma comment(linker, "/export:D3DXCreateTexture=D3DX9_43_org.D3DXCreateTexture")
```

The loader resolves those forwarders, so no wrappers, no signatures and no
trampolines are needed: calls behave exactly as before. The only code of ours
that actually runs is `DllMain`.

## Layout

```
ConquerDX9Hook/
  src/
    dllmain.cpp      proxy entry point (DLL_PROCESS_ATTACH)
    proxy.cpp        D3DX9_43 export forwarders -> D3DX9_43_org
  version.h          project metadata
ConquerDX9.Hook.sln
build.bat            Release x86 -> Release\D3DX9_43.dll
```

## Build

```
build.bat                  # Release x86 -> Release\D3DX9_43.dll
build.bat Debug x86
```

`build.bat` locates MSBuild with `vswhere` and pins toolset `v143`, so it works
from any VS2022 install that has the "Desktop development with C++" workload.
It then copies the built DLL over the RDP share to `\\tsclient\H\client\Env_DX9`.

Deploy the built `D3DX9_43.dll` next to `Conquer.exe`, keeping the renamed
`D3DX9_43_org.dll` beside it.

## Adding your own code

`DllMain` runs under the loader lock, so keep it trivial — do not create
windows, load modules or block on other threads from inside it. To add a
payload, spawn a worker thread from `DLL_PROCESS_ATTACH` and do the real work
there. `src/gadget_loader.cpp` does exactly this and is the worked example.

## The Frida Gadget payload

`DllMain` hands a worker thread the job of loading `D3DX9_43_44.dll` — the
Frida Gadget — from beside the proxy. The Gadget then boots Frida inside the
game process and listens on `127.0.0.1:27042`, so you can hook functions,
scan memory for values, and patch them, all in-process and with no external
injector.

```
python third_party/frida/fetch-gadget.py   # fetch + verify the Gadget into deploy/
python scripts/attach.py                   # attach and drive the in-process agent
```

The sidecar is optional: with `D3DX9_43_44.dll` absent, the game runs exactly
as before and no listener appears. Full instructions, worked examples and a
troubleshooting table are in **[USAGE.md](USAGE.md)**.

## Layout additions

```
scripts/
  agent.js           in-process tooling (rpc.exports): scan, read/write, hook
  attach.py          connect to the Gadget and drive the agent
  preflight.py       check every link in the chain and name the broken one
deploy/
  D3DX9_43_44.config sidecar config for the Gadget
third_party/frida/
  fetch-gadget.py    download + verify the prebuilt 32-bit Gadget
```

`build.bat` asserts that the linked DLL contains the loader (it greps for the
`DX9HOOK_GADGET_LOADER_V1` marker) and fails the build otherwise. A
forwarder-only build of this DLL is completely healthy — it loads, the game
runs, every D3DX call works — so a stale source tree would otherwise ship a
proxy that silently never starts Frida.

## Requirements

- Windows x86 target (the game is 32-bit)
- Visual Studio 2022 with the "Desktop development with C++" workload
- The original `D3DX9_43.dll` present as `D3DX9_43_org.dll`
