#!/usr/bin/env python3
"""Attach to the Frida Gadget inside the game and drive the agent.

The Gadget is a *remote device* as far as Frida is concerned - it is not a
process Frida spawned or attached to locally - so the first step is always
DeviceManager.add_remote_device(). Everything after that is ordinary Frida.

Usage
-----
    # interactive: loads scripts/agent.js and drops you into a Python REPL
    # with `rpc` bound to the agent's exports
    python scripts/attach.py

    # one-shot call, for scripting
    python scripts/attach.py --call ping --args '[]'
    python scripts/attach.py --call scanStart --args '["int32", {"value": 100}]'

    # a token-protected or non-default listener
    python scripts/attach.py --port 27042 --token hunter2

Inside the REPL
---------------
    >>> rpc.ping()
    >>> rpc.modules()
    >>> scan = rpc.scanStart("int32", {"value": 100})
    >>> rpc.scanResults(scan["scanId"])
    >>> rpc.writeValue("0x1234abcd", "int32", 9999)
"""

from __future__ import annotations

import argparse
import code
import json
import socket
import sys
from pathlib import Path

try:
    import frida
except ImportError:  # pragma: no cover - environment guard
    sys.stderr.write(
        "FATAL: the 'frida' Python package is not installed.\n"
        "       Install the host environment first:\n"
        "         python -m pip install frida\n"
    )
    sys.exit(1)


def probe_tcp(host: str, port: int, timeout: float = 1.5) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def no_listener_help(host: str, port: int) -> None:
    sys.stderr.write(
        f"\nNothing is listening on {host}:{port}, so there is nothing to attach to.\n"
        "The client is fine - the failure is upstream. Usual causes, in order:\n"
        "  1. the deployed D3DX9_43.dll predates the Gadget loader (it forwards\n"
        "     D3DX perfectly and never starts Frida, so nothing looks broken)\n"
        "  2. the game was started before the current DLL was deployed\n"
        "  3. D3DX9_43_44.dll or its .config is missing / misnamed\n"
        "  4. the Gadget is not the 32-bit build\n"
        f"\nDiagnose the whole chain with:\n  python {Path(__file__).with_name('preflight.py')}\n"
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1", help="Gadget bind address")
    parser.add_argument("--port", type=int, default=27042, help="Gadget controller port")
    parser.add_argument("--token", default=None, help="shared secret, if the Gadget was configured with one")
    parser.add_argument("--process", default=None, help="process name or pid to attach to")
    parser.add_argument(
        "--agent",
        default=str(Path(__file__).resolve().with_name("agent.js")),
        help="agent script to load (default scripts/agent.js)",
    )
    parser.add_argument("--call", default=None, help="call a single rpc export and exit")
    parser.add_argument("--args", default="[]", help="JSON array of arguments for --call")
    parser.add_argument("--quiet", action="store_true", help="suppress the connection banner")
    return parser.parse_args(argv)


def connect(endpoint: str, token: str | None):
    manager = frida.get_device_manager()
    try:
        return manager.add_remote_device(endpoint, token=token)
    except TypeError:
        # Older frida-python has no token kwarg.
        if token:
            sys.stderr.write("WARNING: this frida build cannot pass a token; ignoring --token\n")
        return manager.add_remote_device(endpoint)


def pick_process(device, wanted: str | None):
    processes = device.enumerate_processes()
    if not processes:
        raise SystemExit(
            f"the endpoint {device.id} exposed no processes - "
            "is the target running with the embedded Gadget listening?"
        )

    if wanted:
        for process in processes:
            if process.name == wanted or str(process.pid) == str(wanted):
                return process
        available = ", ".join(f"{p.name}({p.pid})" for p in processes)
        raise SystemExit(f"no process matching {wanted!r}; available: {available}")

    if len(processes) == 1:
        return processes[0]

    available = ", ".join(f"{p.name}({p.pid})" for p in processes)
    raise SystemExit(
        f"the endpoint exposed more than one process; pass --process. Available: {available}"
    )


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    endpoint = f"{args.host}:{args.port}"
    agent_path = Path(args.agent)
    if not agent_path.exists():
        raise SystemExit(f"agent script not found: {agent_path}")

    # Fail with a diagnosis rather than a raw traceback. add_remote_device()
    # does not connect eagerly, so without this the failure surfaces much later
    # as a bare ServerNotRunningError from the first RPC.
    if not probe_tcp(args.host, args.port):
        no_listener_help(args.host, args.port)
        return 2

    device = connect(endpoint, args.token)
    try:
        target = pick_process(device, args.process)
    except frida.ServerNotRunningError:
        no_listener_help(args.host, args.port)
        return 2

    session = device.attach(target.pid)

    script = session.create_script(agent_path.read_text(encoding="utf-8"))

    def on_message(message, data):
        if message.get("type") == "error":
            sys.stderr.write("script error: " + message.get("description", "?") + "\n")
        elif not args.quiet:
            sys.stderr.write("agent message: " + json.dumps(message) + "\n")

    script.on("message", on_message)
    script.load()
    rpc = script.exports_sync

    if not args.quiet:
        info = rpc.ping()
        print(f"connected to {endpoint}")
        print(f"  process : {target.name} (pid {target.pid})")
        print(f"  arch    : {info['arch']}  pointerSize {info['pointerSize']}  runtime {info['runtime']}")
        print(f"  frida   : {info['fridaVersion']}")

    if args.call:
        try:
            call_args = json.loads(args.args)
        except json.JSONDecodeError as error:
            raise SystemExit(f"--args is not valid JSON: {error}")
        result = getattr(rpc, args.call)(*call_args)
        print(json.dumps(result, indent=2, default=str))
        session.detach()
        return 0

    banner = (
        "\nAgent loaded. `rpc` is the agent's rpc.exports; `session`, `script`,\n"
        "`device` and `frida` are also bound. Try:\n"
        "    rpc.ping()\n"
        "    rpc.modules()\n"
        "    rpc.scanStart('int32', {'value': 100})\n"
        "\nCtrl-Z / exit() to leave (the script is unloaded on the way out).\n"
    )
    code.interact(
        banner=banner,
        local={
            "frida": frida,
            "device": device,
            "session": session,
            "script": script,
            "rpc": rpc,
            "agent": rpc,
        },
    )

    try:
        script.unload()
    except Exception:  # noqa: BLE001 - best effort on the way out
        pass
    session.detach()
    return 0


if __name__ == "__main__":
    sys.exit(main())
