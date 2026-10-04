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
| `scanStart(type, options)` | exact search, or snapshot when `options.value` is omitted |
| `scanRefine(scanId, mode, options)` | narrow a snapshot |
| `scanResults(scanId, offset?, limit?)` | page candidates with live values |
| `scanStop(scanId)` | release a scan |
| `findPointersTo(target, options?)` | pointer-sized matches for `target` |
| `hookExport(module, symbol, signature, options?)` | Interceptor hook, records calls |
| `traceEvents(traceId, offset?, limit?)` | page recorded calls |
| `traceStop(traceId)` | detach and return all events |

`scripts/agent.js` is the source of truth — read it to see exactly what each
call returns.

---

## 6. Troubleshooting

**`unable to connect to remote frida-server`.** Nothing is listening on the
port, so the failure is upstream of the client. Run `python scripts/preflight.py`
and follow what it reports. The two causes that account for almost all of it:
the deployed `D3DX9_43.dll` predates the Gadget loader, or the game was started
before the current DLL was deployed — **a process keeps the DLL it mapped at
startup, so replacing the file on disk does nothing until you restart the game.**

**`attach.py` connects but every call fails.** Version skew between the client
and the Gadget. Pin `frida==17.22.1`.

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
