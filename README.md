# D3DX9_43 side-loading proxy

A minimal **DLL side-loading proxy**. It builds a `D3DX9_43.dll` that a game
loads *instead of* the real Direct3D 9 extension library, while every one of
the real library's exports is forwarded straight through to the original. The
proxy module is then free to run its own code inside the target process.

This repo is deliberately scoped to the proxy mechanism only — the forwarding
DLL and its entry point. There is no hooking, no overlay and no game logic
here; those belong in a separate payload you add on top.

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
there.

## Requirements

- Windows x86 target (the game is 32-bit)
- Visual Studio 2022 with the "Desktop development with C++" workload
- The original `D3DX9_43.dll` present as `D3DX9_43_org.dll`
