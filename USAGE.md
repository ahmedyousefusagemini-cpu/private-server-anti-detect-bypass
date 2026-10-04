# How to use it

This is the end-to-end workflow: build the proxy, fetch the Gadget, deploy four
files, launch the game, attach, and drive the target.

The proxy itself does nothing but forward D3DX calls. The payload is the
**Frida Gadget**, a sidecar DLL that the proxy loads from a worker thread. Once
loaded, Frida runs *inside* `Conquer.exe` and you talk to it over a loopback
socket.

```
Conquer.exe
  └─ loads  D3DX9_43.dll        our proxy (DllMain spawns a worker thread)
       ├─ forwards every D3DX export ──> D3DX9_43_org.dll   (the real one)
       └─ worker thread: LoadLibrary("D3DX9_43_44.dll")
            └─ D3DX9_43_44.dll   the Frida Gadget
                 └─ reads D3DX9_43_44.config
                      └─ listens on 127.0.0.1:27042
                           └─ your Frida client attaches here
```

---

## 1. One-time setup

### Build the proxy

On the machine that has Visual Studio 2022 with the C++ workload:

```
build.bat                  # Release x86 -> Release\D3DX9_43.dll
```

### Fetch the Gadget

Anywhere with Python 3 (no Frida build required):

```
python third_party/frida/fetch-gadget.py
```

This downloads `frida-gadget-17.22.1-windows-x86.dll.xz`, checks it really is
the 32-bit PE32 Gadget exporting `frida_gadget_load` / `frida_gadget_unload`,
and writes two files into `deploy/`:

| File | What it is |
|---|---|
| `deploy/D3DX9_43_44.dll` | the Gadget, ~18 MB |
| `deploy/D3DX9_43_44.config` | its sidecar config (listener, port, runtime) |

> The Gadget derives its config path from **its own filename**: rename the DLL
> and you must rename the config to match. `D3DX9_43_44.dll` reads
> `D3DX9_43_44.config` from the same folder.

### Install the host-side Frida

On the machine you will run the client from:

```
python -m pip install frida==17.22.1
```

Pin it to the same version as the Gadget. A client newer than the agent usually
works, the reverse often does not.

---

## 2. Deploy

Put **four** files next to `Conquer.exe`:

```
Conquer.exe
D3DX9_43.dll          <- built by this repo (the proxy)
D3DX9_43_org.dll      <- the game's original D3DX9_43.dll, renamed
D3DX9_43_44.dll       <- the Gadget      (from deploy/)
D3DX9_43_44.config    <- its config      (from deploy/)
```

The proxy is optional at runtime: if `D3DX9_43_44.dll` is absent, the game runs
normally and no Frida listener appears. That makes it easy to ship a clean build
and add the Gadget only when you are debugging.

---

## 3. Launch and attach

Start the game. Then, from the machine running the client:

```
python scripts/attach.py
```

You should see:

```
connected to 127.0.0.1:27042
  process : Conquer.exe (pid 4321)
  arch    : ia32  pointerSize 4  runtime qjs
  frida   : 17.22.1
```

and then a Python REPL with `rpc` bound to the in-process agent. If there is
more than one process on the endpoint, pass `--process Conquer.exe` (or a pid).

Prefer the stock Frida CLI? It works too:

```
frida-ps -H 127.0.0.1:27042                 # list what the endpoint exposes
frida -H 127.0.0.1:27042 -l scripts/agent.js <name>
```

### Proving it works

`scripts/smoke_test.py` walks the layers and stops at the first that breaks, so
the failure names itself instead of surfacing as one opaque error:

```
python scripts/smoke_test.py
```

```
[1] Listener reachable
  [ok]  TCP connect succeeded

[2] Remote device answers
  [ok]  processes: Gadget(15412)

[3] Attach and load the agent
  [ok]  attached to Gadget (pid 15412), loaded agent.js

[4] Agent answers
  [ok]  ping: arch=ia32 pointerSize=4 runtime=QJS frida=17.22.1

[5] Primitives work in-process
  [ok]  target is 32-bit: arch=ia32 pointerSize=4
  [ok]  writeValue / readValue round trip: wrote 1234567, read 1234567
  [ok]  exact scan finds the sentinel: candidates=1 found=true
  [ok]  snapshot + changed refine narrows to one slot: candidates=1
  [ok]  Interceptor hook fires: calls=3
```

Stage 5 is the real proof, and it is safe against a live game: it allocates its
own scratch memory, writes a sentinel, scans for it, narrows by `changed`, then
hooks `kernel32!GetTickCount` and **calls it itself** — so the result never
depends on what the game happens to be doing. It reads and writes no game state.

Use `--stop-at 4` to check only the connection and script loading. From the
`attach.py` REPL, `rpc.selfTest()` runs the same in-process checks.

### If it does not connect

`frida.ServerNotRunningError: unable to connect to remote frida-server` only
means the *last* link is down — the cause is usually three links earlier. Run
the preflight, which checks every step and names the broken one:

```
python scripts/preflight.py
```

```
[1] Deployed files ................. all four present
[2] Proxy DLL contains the loader .. <-- the usual culprit
[3] Gadget is the 32-bit build ..... PE32 i386, 970 exports
[4] Config name matches the Gadget . D3DX9_43_44.dll -> D3DX9_43_44.config
[5] Listener on 127.0.0.1:27042 .... port is open
[6] Game process ................... Conquer.exe is running
```

The failure mode worth knowing: **a forwarder-only build of the proxy is
completely healthy.** It loads, the game runs, every D3DX call works — it just
never starts Frida. So if you build from a stale source tree, nothing looks
wrong and you only find out when the client cannot connect. `build.bat` now
greps the linked DLL for a `DX9HOOK_GADGET_LOADER_V1` marker and fails the
build if it is missing, and `preflight.py` checks the same marker on the
deployed file.

---

## 4. The three jobs

### Hook a function

Find a target, then hook it and record calls. `resolveExport` gives you a
verified address; `hookExport` installs the `Interceptor` hook.

```python
rpc.resolveExport("Conquer.exe", "SomeExportedFunction")
# -> '0x0041a2b0'

trace = rpc.hookExport("Conquer.exe", "SomeExportedFunction",
                       "int(int, pointer)", {"maxEvents": 50})
# -> {'traceId': 1, 'target': '0x0041a2b0', ...}

# ... let the game run ...

rpc.traceEvents(trace["traceId"])
rpc.traceStop(trace["traceId"])     # detaches and returns every event
```

The signature is a `NativeFunction` type list — `int`, `uint`, `pointer`,
`float`, `double`, `int8`…`uint64`, `bool`, `void`. Anything outside that set is
rejected rather than guessed.

To discover exports to hook:

```python
[p for p in rpc.modules() if "Conquer" in p["name"]]
```

### Search memory for a value

**Known value** — a byte-pattern search, fast and snapshot-free:

```python
scan = rpc.scanStart("int32", {"value": 100, "protection": "rw-"})
# -> {'scanId': 1, 'candidates': 812, 'regionCount': 37, ...}

rpc.scanResults(scan["scanId"], 0, 20)
```

**Unknown value** — for something you can see but cannot read, like a health
bar. Snapshot, change the value in-game, then narrow:

```python
scan = rpc.scanStart("int32", {})                 # snapshot, no value
# ... take damage in game ...
rpc.scanRefine(scan["scanId"], "decreased")       # -> candidates: 812 -> 6
# ... take damage again ...
rpc.scanRefine(scan["scanId"], "decreased")       # -> candidates: 6 -> 1

rpc.scanResults(scan["scanId"])
rpc.scanStop(scan["scanId"])                      # release the snapshot
```

Refines compose — each one compares against the previous snapshot and then
advances it. Modes: `changed`, `unchanged`, `increased`, `decreased`, `exact`,
`changed_by`, `increased_by`, `decreased_by` (the `*_by` modes need `delta`).

Finding what points at an address:

```python
rpc.findPointersTo("0x0a1b2c3d")     # candidates, not proven references
```

### Change a value

```python
rpc.readValue("0x0a1b2c3d", "int32")            # -> {'value': 57}
rpc.writeValue("0x0a1b2c3d", "int32", 9999)     # now it is 9999
```

Types: `int8 uint8 int16 uint16 int32 uint32 int64 uint64 float double pointer
utf8 utf16 bytes`. Signed and unsigned are distinct and nothing is coerced
silently. Reads are capped at 64 KiB.

---

## 5. Agent reference

All of these are callable as `rpc.<name>(...)`.

| Method | Purpose |
|---|---|
| `ping()` | arch, pointer size, runtime, frida version |
| `modules()` | loaded modules: name, base, size, path |
| `ranges(protection)` | memory ranges, e.g. `"r-x"`, `"rw-"` |
| `threads()` | thread id, name, state |
| `readValue(address, type, length?)` | typed read |
| `writeValue(address, type, value)` | typed write |
| `resolveExport(module, symbol)` | export -> address |
| `exports(module, filter?)` | list a module's exports — find something to hook |
| `scanStart(type, options)` | exact search, or snapshot when `options.value` is omitted |
| `scanRefine(scanId, mode, options)` | narrow a snapshot |
| `scanResults(scanId, offset?, limit?)` | page candidates with live values |
| `scanStop(scanId)` | release a scan |
| `findPointersTo(target, options?)` | pointer-sized matches for `target` |
| `hookExport(module, symbol, signature, options?)` | Interceptor hook, records calls |
| `hookAddress(address, signature, options?)` | hook a raw address (Ghidra-found function) |
| `symbolize(address)` | address -> module, module offset, nearest symbol |
| `disassemble(address, count?)` | decode instructions with bytes |
| `registers(threadId)` | CPU context of a thread |
| `backtrace(threadId, limit?)` | symbolized call stack |
| `callNative(address, retType, argTypes, args)` | invoke a function inside the target |
| `protect(address, size, protection)` | change page protection |
| `traceEvents(traceId, offset?, limit?)` | page recorded calls |
| `traceStop(traceId)` | detach and return all events |
| `selfTest()` | run the stage-5 checks in-process and return pass/fail |

`scripts/agent.js` is the source of truth — read it to see exactly what each
call returns.

---

## 6. Ghidra and Frida together

Ghidra does the static half; Frida does everything dynamic. The two meet at an
address.

**The Ghidra side** is already set up: project `PrivateClientServer`, program
`Conquer.exe`, `x86:LE:32:default`, image base **`0x00400000`**, 208,956
functions analysed. The MCP bridge is connected to it, so the analysis tools are
live.

**The address handoff.** `Conquer.exe` is a non-relocated executable at a fixed
base, so an address means the same thing in both tools:

```
Frida  0x41f43a          -> module Conquer.exe, moduleOffset 0x1f43a
Ghidra 0x0041f43a        -> the same instruction
```

`rpc.symbolize(addr)` gives you the module and the module-relative offset; add
the module base and you have the Ghidra address. Because the base is
`0x400000`, for `Conquer.exe` the two are literally the same number.

**The loop:**

1. Find the function in Ghidra (decompile, follow xrefs, read the callers).
2. Note its address — e.g. `0x0041f43a`.
3. Hook it in Frida by address, since internal functions have no export:

```python
t = rpc.hookAddress("0x41f43a", "int(int, pointer)", {"captureContext": True, "captureBacktrace": True})
# ... exercise the game ...
rpc.traceStop(t["traceId"])
```

4. `captureContext` records the registers at each hit and `captureBacktrace`
   records the call stack — which is what tells you *who* called it and *with
   what*, the thing a native debugger would have shown you.

**Verifying a hook target before trusting it:** `rpc.disassemble(addr, 8)` shows
the real instructions at that address, so you can confirm you are at a function
prologue and not mid-instruction. `rpc.callNative(addr, retType, argTypes, args)`
lets you call a game function directly and see what it returns.

---

## 7. Troubleshooting

**`unable to connect to remote frida-server`.** Nothing is listening on the
port, so the failure is upstream of the client. Run `python scripts/preflight.py`
and follow what it reports. The two causes that account for almost all of it:
the deployed `D3DX9_43.dll` predates the Gadget loader, or the game was started
before the current DLL was deployed — **a process keeps the DLL it mapped at
startup, so replacing the file on disk does nothing until you restart the game.**

**`attach.py` connects but every call fails.** Version skew between the client
and the Gadget. Pin `frida==17.22.1`.

**Your own JS fails with `Module.findExportByName is not a function`.** Frida 17
removed the static `Module.findExportByName`, `Module.getExportByName` and
`Module.enumerateExports`. Per-module lookup now lives on the `Module` instances
returned by `Process.enumerateModules()` / `Process.getModuleByName()`:

```js
const k32 = Process.getModuleByName('kernel32.dll');
k32.getExportByName('GetTickCount');     // -> NativePointer
k32.enumerateExports();                  // -> [{type, name, address}, ...]

Module.getGlobalExportByName('GetTickCount');   // still static
```

Only `Module.load`, `Module.getGlobalExportByName` and
`Module.findGlobalExportByName` remain static, and the global ones **throw**
rather than return null when the symbol is absent. `scripts/agent.js` tries both
shapes, so `rpc.resolveExport` and `rpc.exports` work on old and new Frida alike.

**No listener after launching the game.** The Gadget was not found or failed to
load. Check with DebugView: the loader logs to `OutputDebugStringA/W` as
`[dx9hook] ...`, starting with `gadget loader active: DX9HOOK_GADGET_LOADER_V1`
when the new build is running. The usual causes are a missing
`D3DX9_43_44.dll`, a mismatched `.config` name, or a 64-bit Gadget.

**`--verify-only` says the Gadget is missing required exports.** You have the
wrong file. Re-run the fetch without `--verify-only`.

**Port already in use.** Something else holds 27042. Either free it or change
`interaction.port` in `D3DX9_43_44.config` (and pass `--port` to `attach.py`).

**Scan is slow or reports `truncated`.** Widen the filter — pass explicit
`regions`, or raise `maxBytes` / `maxCandidates`. File-backed pages are excluded
by default because scanning module code for runtime values is rarely what you
want; pass `includeFileBacked: true` to change that.

**Anti-cheat / detection.** The Gadget listens on loopback and appears in the
module list. If that matters, the alternative is to link the Gadget statically
into the proxy so there is a single self-contained DLL and no extra file — see
the notes in `third_party/frida/apply_patches.py`.
