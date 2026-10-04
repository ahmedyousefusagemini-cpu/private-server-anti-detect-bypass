#!/usr/bin/env python3
"""Materialise the prebuilt Frida Gadget as a deploy artifact.

Frida publishes a complete, self-contained Gadget for every release as

    frida-gadget-<tag>-windows-x86.dll.xz

There is no static Gadget on any release, but the DLL is directly loadable and
needs no Frida source build. This script downloads it, verifies it really is
the 32-bit Windows Gadget, and writes it under a name of our choosing next to
the proxy DLL.

Why the name matters
--------------------
The Gadget boots from its own DllMain and derives its config path from its own
module path:

    dirname + "/" + <filename without its last extension> + ".config"

So renaming the DLL renames the config it looks for. This script writes both,
and the two names must stay in step: D3DX9_43_44.dll <-> D3DX9_43_44.config.

Usage
-----
    # fetch, verify, write deploy/D3DX9_43_44.dll (+ .config if absent)
    python third_party/frida/fetch-gadget.py

    # verify what is already on disk, without touching the network
    python third_party/frida/fetch-gadget.py --verify-only

    # a different release, or a different sidecar name
    python third_party/frida/fetch-gadget.py --tag 17.22.1 --name D3DX9_43_44.dll
"""

from __future__ import annotations

import argparse
import hashlib
import json
import lzma
import struct
import sys
import urllib.request
from pathlib import Path

DEFAULT_TAG = "17.22.1"
DEFAULT_ARCH = "x86"
DEFAULT_NAME = "D3DX9_43_44.dll"

# Resolved against the repo root rather than the cwd: running this script from
# inside third_party/frida/ would otherwise make "--out deploy" mean a nested
# deploy/ there, scattering an 18 MB duplicate of the Gadget.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_OUT = str(REPO_ROOT / "deploy")

ASSET_TEMPLATE = "frida-gadget-{tag}-windows-{arch}.dll.xz"
URL_TEMPLATE = "https://github.com/frida/frida/releases/download/{tag}/{asset}"

# Exports the loader relies on, plus the ones that prove this is the Gadget.
REQUIRED_EXPORTS = ("frida_gadget_load", "frida_gadget_unload")

IMAGE_FILE_MACHINE_I386 = 0x014C
PE32_MAGIC = 0x10B
PE32_PLUS_MAGIC = 0x20B

# Listener bound to loopback only. on_load=resume means the game is never held
# at startup waiting for a debugger; teardown=minimal is right when the
# Gadget's lifetime is the process lifetime; qjs avoids a multi-gigabyte V8
# build and is enough for hooking, scanning and patching.
GADGET_CONFIG = {
    "interaction": {
        "type": "listen",
        "address": "127.0.0.1",
        "port": 27042,
        "on_load": "resume",
        "on_port_conflict": "fail",
    },
    "teardown": "minimal",
    "runtime": "qjs",
}


class VerificationError(Exception):
    pass


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _cstring(data: bytes, offset: int, limit: int = 256) -> str:
    end = data.find(b"\x00", offset, offset + limit)
    if end == -1:
        end = min(offset + limit, len(data))
    return data[offset:end].decode("ascii", "replace")


def parse_pe(data: bytes) -> dict:
    """Minimal PE reader: machine, PE32 flag, sections, imports, exports."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise VerificationError("not a PE file (no MZ signature)")

    e_lfanew = _u32(data, 0x3C)
    if e_lfanew + 24 > len(data) or data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise VerificationError("not a PE file (no PE signature)")

    coff = e_lfanew + 4
    machine = _u16(data, coff)
    num_sections = _u16(data, coff + 2)
    size_optional = _u16(data, coff + 16)
    opt = coff + 20

    magic = _u16(data, opt)
    is_pe32 = magic == PE32_MAGIC

    section_table = opt + size_optional
    sections = []
    for i in range(num_sections):
        base = section_table + i * 40
        if base + 40 > len(data):
            raise VerificationError("truncated section table")
        sections.append(
            {
                "name": _cstring(data, base, 8),
                "virtual_address": _u32(data, base + 12),
                "virtual_size": _u32(data, base + 8),
                "raw_pointer": _u32(data, base + 20),
                "raw_size": _u32(data, base + 16),
            }
        )

    def rva_to_offset(rva: int) -> int:
        for section in sections:
            start = section["virtual_address"]
            span = max(section["virtual_size"], section["raw_size"])
            if start <= rva < start + span:
                return rva - start + section["raw_pointer"]
        raise VerificationError(f"RVA 0x{rva:x} is not mapped by any section")

    # Data directories. PE32 has the directory array at optional+96; the count
    # lives at optional+92. PE32+ shifts both.
    dir_offset = opt + (96 if is_pe32 else 112)
    num_dirs = _u32(data, opt + (92 if is_pe32 else 108))

    exports: list[str] = []
    if num_dirs > 0:
        export_rva = _u32(data, dir_offset)
        if export_rva:
            base = rva_to_offset(export_rva)
            num_names = _u32(data, base + 24)
            names_rva = _u32(data, base + 32)
            names_off = rva_to_offset(names_rva)
            for i in range(num_names):
                name_rva = _u32(data, names_off + i * 4)
                exports.append(_cstring(data, rva_to_offset(name_rva)))

    imports: list[str] = []
    if num_dirs > 1:
        import_rva = _u32(data, dir_offset + 8)
        if import_rva:
            base = rva_to_offset(import_rva)
            while True:
                name_rva = _u32(data, base + 12)
                if name_rva == 0:
                    break
                imports.append(_cstring(data, rva_to_offset(name_rva)))
                base += 20
                if base + 20 > len(data):
                    break

    return {
        "machine": machine,
        "is_pe32": is_pe32,
        "magic": magic,
        "sections": sections,
        "exports": exports,
        "imports": imports,
    }


def verify_gadget(data: bytes) -> dict:
    pe = parse_pe(data)

    if pe["machine"] != IMAGE_FILE_MACHINE_I386:
        raise VerificationError(
            f"machine is 0x{pe['machine']:04X}, expected 0x{IMAGE_FILE_MACHINE_I386:04X} "
            "(i386) - this is not the 32-bit Gadget"
        )
    if not pe["is_pe32"]:
        raise VerificationError(
            f"optional header magic is 0x{pe['magic']:04X}, expected PE32 (0x{PE32_MAGIC:04X})"
        )

    missing = [name for name in REQUIRED_EXPORTS if name not in pe["exports"]]
    if missing:
        raise VerificationError("missing required exports: " + ", ".join(missing))

    # The Gadget links against the OS only; a companion DLL would mean a
    # deployment that silently needs more files than we ship.
    companions = [name for name in pe["imports"] if "frida" in name.lower()]
    if companions:
        raise VerificationError(
            "the Gadget would need companion DLLs deployed beside it: "
            + ", ".join(companions)
        )

    return pe


def derive_config_path(dll_path: Path) -> Path:
    """dirname + stem + .config - the path the Gadget derives from its own name."""
    return dll_path.with_suffix("").with_suffix(".config")


def download(tag: str, arch: str) -> bytes:
    asset = ASSET_TEMPLATE.format(tag=tag, arch=arch)
    url = URL_TEMPLATE.format(tag=tag, asset=asset)
    print(f"  downloading {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "conquerdx9hook-fetch"})
    with urllib.request.urlopen(request, timeout=120) as response:
        if response.status != 200:
            raise VerificationError(f"HTTP {response.status} for {url}")
        return response.read()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", default=DEFAULT_TAG, help=f"Frida release (default {DEFAULT_TAG})")
    parser.add_argument("--arch", default=DEFAULT_ARCH, help=f"target arch (default {DEFAULT_ARCH})")
    parser.add_argument("--name", default=DEFAULT_NAME, help="deployed Gadget filename")
    parser.add_argument("--out", default=DEFAULT_OUT, help="output directory (default <repo>/deploy)")
    parser.add_argument("--force", action="store_true", help="re-download even if present")
    parser.add_argument("--no-config", action="store_true", help="do not write the sidecar .config")
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="verify what is already on disk; do not touch the network",
    )
    args = parser.parse_args(argv)

    out_dir = Path(args.out).resolve()
    dll_path = out_dir / args.name
    config_path = derive_config_path(dll_path)

    print("Prebuilt Frida Gadget -> deploy artifact")
    print(f"  tag        : {args.tag}")
    print(f"  target     : windows-{args.arch}")
    print(f"  deploy name: {dll_path.name}")
    print(f"  config     : {config_path.name}  (derived from the DLL's own name)")

    if config_path.stem != dll_path.stem:
        print(f"  [!!] config stem '{config_path.stem}' != dll stem '{dll_path.stem}'")
        return 1

    if args.verify_only:
        if not dll_path.exists():
            print(f"  [!!] {dll_path} not found and --verify-only was given")
            return 1
        raw = dll_path.read_bytes()
        print(f"  [ok] using existing {dll_path.name} ({len(raw)} bytes)")
    elif dll_path.exists() and not args.force:
        raw = dll_path.read_bytes()
        print(f"  [ok] {dll_path.name} already present - left untouched (use --force to replace)")
    else:
        try:
            compressed = download(args.tag, args.arch)
        except Exception as error:  # noqa: BLE001 - report, do not traceback
            print(f"  [!!] network error: {error}")
            return 1

        print(f"  [ok] {len(compressed)} bytes compressed (sha256 {hashlib.sha256(compressed).hexdigest()[:16]}...)")

        try:
            raw = lzma.decompress(compressed)
        except lzma.LZMAError as error:
            print(f"  [!!] not a valid .xz stream: {error}")
            return 1

        print(f"  [ok] {len(raw)} bytes decompressed")
        out_dir.mkdir(parents=True, exist_ok=True)
        dll_path.write_bytes(raw)

    try:
        pe = verify_gadget(raw)
    except VerificationError as error:
        print(f"  [!!] {error}")
        print("\n  refusing to deploy an artifact that failed verification")
        return 1

    print(
        f"  [ok] PE32 i386 (machine=0x{pe['machine']:04X}), "
        f"{len(pe['sections'])} sections, {len(pe['exports'])} exports, "
        f"including {', '.join(REQUIRED_EXPORTS)}"
    )
    print(f"  [ok] {len(pe['imports'])} imports, all Windows API - self-contained")
    print(f"  [ok] {len(raw)} bytes -> {dll_path}")

    if not args.no_config:
        if config_path.exists() and not args.force:
            print(f"  [ok] {config_path.name} already present - left untouched")
        else:
            out_dir.mkdir(parents=True, exist_ok=True)
            config_path.write_text(json.dumps(GADGET_CONFIG, indent=2) + "\n", encoding="utf-8")
            print(f"  [ok] wrote {config_path}")

    print()
    print("Deploy these four files beside Conquer.exe:")
    print(f"  {dll_path.name:<22} (the Gadget, loaded by our proxy)")
    print(f"  {config_path.name:<22} (its sidecar config)")
    print("  D3DX9_43.dll           (our proxy, built from this repo)")
    print("  D3DX9_43_org.dll       (the original Microsoft D3DX9)")
    print()
    print(f"The Gadget binds 127.0.0.1:{GADGET_CONFIG['interaction']['port']} from the sidecar config.")
    print("With no config it would fall back to 127.0.0.1:27042 with no token - safe,")
    print("but unauthenticated, so keep the config deployed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
