#!/usr/bin/env python3
"""Check every link in the Frida-Gadget chain and name the broken one.

`attach.py` failing with "unable to connect to remote frida-server" only tells
you the last link is down; the cause is usually three links earlier. This walks
the whole chain and reports each step:

    1. the four deployed files exist beside the game
    2. the proxy DLL actually contains the Gadget loader
       (the single most common cause: a stale D3DX9_43.dll from before the
        loader was added - it forwards D3DX fine and never starts Frida)
    3. the Gadget is the 32-bit PE32 build exporting frida_gadget_load
    4. the sidecar config name matches the Gadget's own filename
    5. something is listening on the controller port
    6. the game process is running

Usage
-----
    python scripts/preflight.py
    python scripts/preflight.py --deploy-dir "H:/client/Env_DX9"
    python scripts/preflight.py --port 27042
"""

from __future__ import annotations

import argparse
import importlib.util
import socket
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PE_HELPER = REPO_ROOT / "third_party" / "frida" / "fetch-gadget.py"

OK = "  [ok]  "
BAD = "  [!!]  "
WARN = "  [--]  "

PROXY_NAME = "D3DX9_43.dll"
GADGET_NAME = "D3DX9_43_44.dll"
ORIGINAL_NAME = "D3DX9_43_org.dll"
GAME_NAME = "Conquer.exe"


def load_pe_helper():
    spec = importlib.util.spec_from_file_location("fetch_gadget", PE_HELPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def probe_tcp(host: str, port: int, timeout: float = 1.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def running_processes(names: set[str]) -> list[str]:
    try:
        result = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return []

    found = []
    for line in result.stdout.splitlines():
        if not line.startswith('"'):
            continue
        image = line.split('"')[1]
        if image.lower() in names:
            found.append(image)
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--deploy-dir", default="H:/client/Env_DX9", help="folder holding the game")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=27042)
    parser.add_argument("--proxy-name", default=PROXY_NAME)
    parser.add_argument("--gadget-name", default=GADGET_NAME)
    parser.add_argument("--game", default=GAME_NAME)
    args = parser.parse_args(argv)

    pe = load_pe_helper()
    deploy = Path(args.deploy_dir)
    problems: list[str] = []
    actions: list[str] = []

    print(f"Frida Gadget preflight - {deploy}")
    print("=" * 68)

    # 1. deployed files -----------------------------------------------------
    print("[1] Deployed files")
    proxy_path = deploy / args.proxy_name
    gadget_path = deploy / args.gadget_name
    config_path = gadget_path.with_suffix("").with_suffix(".config")
    original_path = deploy / ORIGINAL_NAME

    if not deploy.is_dir():
        print(BAD + f"deploy folder does not exist: {deploy}")
        problems.append("deploy folder missing")
        actions.append(f"Check the path, or pass --deploy-dir. Current: {deploy}")
        report(problems, actions)
        return 2

    for path, required in (
        (proxy_path, True),
        (gadget_path, True),
        (config_path, True),
        (original_path, False),
    ):
        if path.exists():
            print(OK + f"{path.name} ({path.stat().st_size} bytes)")
        elif required:
            print(BAD + f"{path.name} MISSING")
            problems.append(f"{path.name} missing")
        else:
            print(WARN + f"{path.name} missing (only needed for the D3DX passthrough)")

    if not gadget_path.exists():
        actions.append("Run: python third_party/frida/fetch-gadget.py")
    if not config_path.exists():
        actions.append(
            f"Create {config_path.name} - the Gadget derives its config name from "
            "its own filename"
        )

    # 2. does the proxy actually contain the loader? ------------------------
    print()
    print("[2] Proxy DLL contains the Gadget loader")
    if not proxy_path.exists():
        print(BAD + "cannot check - proxy DLL missing")
    else:
        raw = proxy_path.read_bytes()
        has_marker = b"DX9HOOK_GADGET_LOADER_V1" in raw
        has_api = b"LoadLibraryExW" in raw
        has_string = args.gadget_name.encode("utf-16-le") in raw
        if has_marker and has_api and has_string:
            print(
                OK
                + "loader present (DX9HOOK_GADGET_LOADER_V1 marker, "
                "LoadLibraryExW import, gadget path string)"
            )
        else:
            print(BAD + "loader NOT present - this is a forwarder-only build")
            missing = []
            if not has_marker:
                missing.append("no DX9HOOK_GADGET_LOADER_V1 marker")
            if not has_api:
                missing.append("no LoadLibraryExW import")
            if not has_string:
                missing.append(f"no {args.gadget_name!r} string")
            print("        " + "; ".join(missing))
            problems.append("proxy DLL predates the Gadget loader")
            actions.append(
                "Rebuild the proxy with src/gadget_loader.cpp + the updated "
                "src/dllmain.cpp, redeploy D3DX9_43.dll, then RESTART the game."
            )
            actions.append(
                "If your build host builds from its own copy of the repo, make sure "
                "the new files reached it - a stale tree builds silently and forwards "
                "D3DX exactly as before, so nothing looks wrong."
            )

    # 3. gadget identity ----------------------------------------------------
    print()
    print("[3] Gadget is the 32-bit build")
    if not gadget_path.exists():
        print(BAD + "cannot check - Gadget missing")
    else:
        try:
            info = pe.verify_gadget(gadget_path.read_bytes())
            print(
                OK
                + f"PE32 i386, machine=0x{info['machine']:04X}, "
                f"{len(info['exports'])} exports, {len(info['imports'])} imports"
            )
        except pe.VerificationError as error:
            print(BAD + str(error))
            problems.append("Gadget failed verification")
            actions.append("Re-run the fetch, without --verify-only.")

    # 4. config name coupling ----------------------------------------------
    print()
    print("[4] Sidecar config name matches the Gadget filename")
    if gadget_path.exists():
        expected = gadget_path.with_suffix("").with_suffix(".config")
        if expected.name == config_path.name:
            print(OK + f"{gadget_path.name} -> {expected.name}")
        else:
            print(BAD + f"{gadget_path.name} would read {expected.name}, not {config_path.name}")
            problems.append("config name does not match the Gadget filename")
    else:
        print(WARN + "cannot check without the Gadget")

    # 5. listener -----------------------------------------------------------
    print()
    print(f"[5] Listener on {args.host}:{args.port}")
    listening = probe_tcp(args.host, args.port)
    if listening:
        print(OK + "port is open")
    else:
        print(BAD + "nothing is listening")
        problems.append("no listener on the controller port")

    # 6. game process -------------------------------------------------------
    print()
    print("[6] Game process")
    found = running_processes({args.game.lower(), "conquer.exe"})
    if found:
        print(OK + f"{', '.join(sorted(set(found)))} is running")
        if not listening:
            print(
                "        running but not listening: the process started before the "
                "current DLL was deployed"
            )
            actions.append(
                "Restart the game. A process keeps the DLL it mapped at startup; "
                "replacing the file on disk does not affect a running process."
            )
    else:
        print(WARN + f"{args.game} is not running")
        if not listening:
            actions.append(f"Start {args.game}, then run this again.")

    report(problems, actions)
    return 1 if problems else 0


def report(problems: list[str], actions: list[str]) -> None:
    print()
    print("=" * 68)
    if not problems:
        print("All checks passed. The chain is up - attach with scripts/attach.py")
        return

    print(f"{len(problems)} problem(s):")
    for problem in problems:
        print(f"  - {problem}")
    if actions:
        print()
        print("Next:")
        for index, action in enumerate(dict.fromkeys(actions), start=1):
            print(f"  {index}. {action}")


if __name__ == "__main__":
    sys.exit(main())
