#!/usr/bin/env python3
"""Prove the attach works, one layer at a time.

A bare `ServerNotRunningError` covers everything from "the game is not running"
to "the agent threw on line 3". This walks the layers and stops at the first
that breaks, so you always know which one it is:

    [1] listener reachable         TCP connect to host:port
    [2] remote device answers      add_remote_device + enumerate_processes
    [3] attach and load agent      session.attach + create_script + load
    [4] agent answers              ping() returns the target's identity
    [5] primitives actually work   in-process self test: read/write/scan/hook

Stages 1-4 need the Gadget loaded and listening. Stage 5 additionally needs the
agent's primitives to function; it only touches memory the agent allocates for
itself, so it is safe to run against a live game.

Usage
-----
    python scripts/smoke_test.py
    python scripts/smoke_test.py --process Conquer.exe --port 27042
    python scripts/smoke_test.py --stop-at 4     # connection only, no self test
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import attach  # noqa: E402 - local helper module, must come after sys.path

OK = "  [ok]  "
BAD = "  [!!]  "


def stage(number: int, title: str) -> None:
    print(f"[{number}] {title}")


def ok(text: str) -> None:
    print(OK + text)


def bad(text: str) -> None:
    print(BAD + text)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=27042)
    parser.add_argument("--token", default=None)
    parser.add_argument("--process", default=None, help="process name or pid")
    parser.add_argument(
        "--agent",
        default=str(Path(__file__).resolve().with_name("agent.js")),
        help="agent script to load",
    )
    parser.add_argument(
        "--stop-at",
        type=int,
        default=5,
        choices=(1, 2, 3, 4, 5),
        help="stop after this stage (default 5 = run everything)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    agent_path = Path(args.agent)
    if not agent_path.exists():
        raise SystemExit(f"agent script not found: {agent_path}")

    print(f"Attach smoke test - {args.host}:{args.port}")
    print("=" * 68)

    # [1] listener ----------------------------------------------------------
    stage(1, "Listener reachable")
    if not attach.probe_tcp(args.host, args.port):
        bad(f"nothing listening on {args.host}:{args.port}")
        attach.no_listener_help(args.host, args.port)
        return 1
    ok("TCP connect succeeded")
    if args.stop_at < 2:
        return 0

    # [2] remote device -----------------------------------------------------
    print()
    stage(2, "Remote device answers")
    try:
        device = attach.connect(f"{args.host}:{args.port}", args.token)
        processes = device.enumerate_processes()
    except Exception as error:  # noqa: BLE001 - report, do not traceback
        bad(f"{type(error).__name__}: {error}")
        return 1
    if not processes:
        bad("the endpoint exposed no processes")
        return 1
    ok("processes: " + ", ".join(f"{p.name}({p.pid})" for p in processes))
    if args.stop_at < 3:
        return 0

    # [3] attach and load ---------------------------------------------------
    print()
    stage(3, "Attach and load the agent")
    try:
        target = attach.pick_process(device, args.process)
    except SystemExit as error:
        bad(str(error))
        return 1
    try:
        session = device.attach(target.pid)
        script = session.create_script(agent_path.read_text(encoding="utf-8"))
        script.load()
        rpc = script.exports_sync
    except Exception as error:  # noqa: BLE001
        bad(f"{type(error).__name__}: {error}")
        return 1
    ok(f"attached to {target.name} (pid {target.pid}), loaded {agent_path.name}")
    if args.stop_at < 4:
        session.detach()
        return 0

    # [4] agent answers -----------------------------------------------------
    print()
    stage(4, "Agent answers")
    try:
        info = rpc.ping()
    except Exception as error:  # noqa: BLE001
        bad(f"{type(error).__name__}: {error}")
        session.detach()
        return 1
    ok(
        f"ping: arch={info['arch']} pointerSize={info['pointerSize']} "
        f"runtime={info['runtime']} frida={info['fridaVersion']}"
    )
    if info["arch"] != "ia32" or info["pointerSize"] != 4:
        bad(f"expected a 32-bit target (ia32 / 4), got {info['arch']} / {info['pointerSize']}")
    if args.stop_at < 5:
        session.detach()
        print()
        print("Stopped at stage 4 as requested: connection and script loading work.")
        return 0

    # [5] primitives --------------------------------------------------------
    print()
    stage(5, "Primitives work in-process")
    try:
        result = rpc.selfTest()
    except Exception as error:  # noqa: BLE001
        bad(f"{type(error).__name__}: {error}")
        session.detach()
        return 1

    for entry in result["checks"]:
        (ok if entry["ok"] else bad)(f"{entry['name']}: {entry['detail']}")

    session.detach()

    print()
    print("=" * 68)
    if result["ok"]:
        print("All stages passed - the attach works end to end.")
        return 0
    print("Attached, but the in-process self test failed. See the [!!] lines above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
