#!/usr/bin/env python3
"""Generate docs/packet-catalog.md from the recovered id tables.

Reads:
  tools/data/ids_recv.tsv   <hex-id> TAB CMsg<Name>          (recovered map)
  tools/data/overrides.tsv  <id> TAB <dir> TAB <name> TAB <desc>
                            (curated friendly names / descriptions)

Writes:
  docs/packet-catalog.md

Run from the repository root:
  python tools/gen_catalog.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IDS = os.path.join(HERE, "data", "ids_recv.tsv")
OVERRIDES = os.path.join(HERE, "data", "overrides.tsv")
OUT = os.path.join(ROOT, "docs", "packet-catalog.md")


def load_ids():
    """hex id (int) -> CMsg<Name>."""
    result = {}
    if not os.path.exists(IDS):
        return result
    with open(IDS, encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            try:
                ident = int(parts[0], 16)
            except ValueError:
                continue
            result[ident] = parts[1].strip()
    return result


def load_overrides():
    """id (int) -> (direction, name, description)."""
    result = {}
    if not os.path.exists(OVERRIDES):
        return result
    with open(OVERRIDES, encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 4:
                continue
            try:
                ident = int(parts[0], 16)
            except ValueError:
                continue
            result[ident] = (parts[1].strip(), parts[2].strip(), parts[3].strip())
    return result


DIRECTION_WORD = {"S": "C→S", "R": "S→C", "B": "both"}


def main():
    ids = load_ids()
    overrides = load_overrides()

    all_ids = sorted(set(ids) | set(overrides))

    lines = []
    lines.append("# Packet catalogue")
    lines.append("")
    lines.append("Every message id this client knows about, with the name recovered")
    lines.append("from the binary and (where known) what it is for.")
    lines.append("")
    lines.append("**How the names were recovered.** The receive dispatcher is")
    lines.append("`CNetMsg::CreateNetMsg` at `0x00f41fce` - a single `switch(msgId)`")
    lines.append("with 470 cases. Each case calls a constructor whose body is")
    lines.append("`*this = CMsg<Name>::vftable;`, which yields the class name. Send")
    lines.append("ids are written by each class's `Create()` into `msg+6`; the send and")
    lines.append("receive numbering is the same. See [packet-hooks.md](packet-hooks.md)")
    lines.append("for the hook addresses and how to re-derive them.")
    lines.append("")
    lines.append("**Wire format.**")
    lines.append("")
    lines.append("```")
    lines.append("[uint16 total_len][uint16 msg_id][protobuf body]")
    lines.append("```")
    lines.append("")
    lines.append("**Direction** is `C→S` (client to server only), `S→C` (server to")
    lines.append("client only) or `both`, as observed. Ids with no direction have not")
    lines.append("been seen on the wire yet - they come from the dispatch table alone.")
    lines.append("")
    seen_in_table = len([i for i in overrides if i in ids])
    lines.append(f"Total ids: **{len(all_ids)}**.")
    lines.append("")
    lines.append(f"- {len(ids)} recovered from the receive dispatch table")
    lines.append(f"- {len(overrides)} annotated with a friendly name and description")
    lines.append(f"- {seen_in_table} of the annotated ids also appear in the dispatch table; "
                 "the rest are either login-server ids handled outside it or send-only ids "
                 "resolved from their `Create()` site")
    lines.append("")

    # ---- annotated section ------------------------------------------------
    lines.append("## Annotated")
    lines.append("")
    lines.append("| id | dir | name | class | meaning |")
    lines.append("|----|-----|------|-------|---------|")

    def sort_key(ident):
        return ident

    for ident in sorted(overrides, key=sort_key):
        direction, name, desc = overrides[ident]
        cls = ids.get(ident, "")
        cls_cell = ("`%s`" % cls) if cls else "-"
        desc_clean = desc.replace("|", "\\|")
        lines.append("| `0x%04X` | %s | %s | %s | %s |"
                     % (ident, DIRECTION_WORD.get(direction, direction),
                        name, cls_cell, desc_clean))

    lines.append("")

    # ---- everything else --------------------------------------------------
    remaining = [i for i in all_ids if i not in overrides]
    lines.append("## Recovered from the binary (no annotation yet)")
    lines.append("")
    lines.append("These ids exist in the client's dispatch table but have not been")
    lines.append("seen in a captured session, so only the class name is known.")
    lines.append("")
    lines.append("| id | class |")
    lines.append("|----|-------|")
    for ident in remaining:
        lines.append("| `0x%04X` | `%s` |" % (ident, ids.get(ident, "")))
    lines.append("")

    lines.append("---")
    lines.append("")
    lines.append("Regenerate this file with `python tools/gen_catalog.py` after")
    lines.append("updating `tools/data/ids_recv.tsv` or `tools/data/overrides.tsv`.")
    lines.append("")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("wrote %s (%d ids: %d annotated, %d unannotated)"
          % (OUT, len(all_ids), len(overrides), len(remaining)))


if __name__ == "__main__":
    main()
