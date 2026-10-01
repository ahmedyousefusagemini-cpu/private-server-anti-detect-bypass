#!/usr/bin/env python3
"""packet_log.py - meaningful Conquer packet logger / decoder.

Turns a raw capture into a human-readable, annotated packet stream.

Two input formats are auto-detected:

  * hook format  (packets.log)  - the current D3DX9 hook output:
        [HH:MM:SS.mmm] SEND len=472 id=0x0796
          0000  D8 01 96 07 95 CF D2 E8  EA D6 04 4D F6 97 A3 4E  |........|
  * rich format  (packet.log)   - an older capture that already carries names:
        tick,DIR,len,type,sock,hex,ascii,Name

For every packet it prints the message NAME (from the Ghidra-recovered
id -> CMsg<class> map), the direction, length, and a decoded protobuf body.
Non-protobuf bodies are shown as a hex preview.

Usage
-----
    python packet_log.py <logfile> [options]

Options
-------
    --full            decode every packet (default: collapse repeats)
    --dir S|R        only show SEND / only show RECV
    --id 0x0796       only show a given message id (repeatable, comma-ok)
    --limit N         stop after N shown packets
    --summary         print a per-id summary instead of the stream
    --catalog         print the full id -> name catalogue and exit
    --json            emit JSON lines (machine readable)
    --no-strings      omit protobuf string fields
    --color           force ANSI colour

The id -> name map is loaded from tools/data/ids_recv.tsv (470 entries,
recovered from CNetMsg::CreateNetMsg in Conquer.exe) merged with the curated
tools/data/overrides.tsv (directions + plain-English meanings).
"""

import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")

# ---------------------------------------------------------------------------
# id -> name catalogue
# ---------------------------------------------------------------------------

def load_catalog():
    """Return {id:int -> {name, dir, desc}} from the two data files."""
    cat = {}

    recv = os.path.join(DATA, "ids_recv.tsv")
    if os.path.exists(recv):
        with open(recv, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 2 or not parts[0].startswith("0x"):
                    continue
                try:
                    mid = int(parts[0], 16)
                except ValueError:
                    continue
                name = parts[1].strip()
                # strip the 'CMsg' prefix for readability
                if name.startswith("CMsg"):
                    name = name[4:]
                cat[mid] = {"name": name, "dir": "R", "desc": ""}

    ov = os.path.join(DATA, "overrides.tsv")
    if os.path.exists(ov):
        with open(ov, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 3 or not parts[0].startswith("0x"):
                    continue
                raw = parts[0]
                if "_" in raw:            # 0x092E_1 style placeholder -> skip
                    continue
                try:
                    mid = int(raw, 16)
                except ValueError:
                    continue
                e = cat.setdefault(mid, {"name": "", "dir": "?", "desc": ""})
                e["dir"] = parts[1].strip()
                e["name"] = parts[2].strip() or e["name"]
                e["desc"] = parts[3].strip() if len(parts) > 3 else ""
    return cat


CATALOG = load_catalog()


# ---------------------------------------------------------------------------
# log parsing
# ---------------------------------------------------------------------------

HOOK_HDR = re.compile(
    r"^\[(\d\d:\d\d:\d\d\.\d\d\d)\]\s+(SEND|RECV)\s+len=(\d+)\s+id=0x([0-9A-Fa-f]+)"
)
HOOK_ROW = re.compile(r"^\s+([0-9A-F]{4})\s+((?:[0-9A-Fa-f]{2}\s+){1,16})\s*\|")
RICH_LINE = re.compile(
    r"^(\d+),(SEND-ENC|SEND|RECV),(\d+),(0x[0-9A-Fa-f]+),([0-9A-Fa-f]*),([0-9A-Fa-f]*),\"(.*)\",(.*)$"
)


def parse_hook(path):
    pkts, cur = [], None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = HOOK_HDR.match(line)
            if m:
                cur = {"t": m.group(1), "dir": m.group(2), "len": int(m.group(3)),
                       "id": int(m.group(4), 16), "data": bytearray(),
                       "name": None}
                pkts.append(cur)
                continue
            r = HOOK_ROW.match(line)
            if r and cur is not None:
                cur["data"] += bytes.fromhex(r.group(2).replace(" ", ""))
    return pkts


def parse_rich(path):
    pkts = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = RICH_LINE.match(line.rstrip("\n"))
            if not m:
                continue
            d = m.group(2)
            if d == "SEND-ENC":
                continue                      # encrypted mirror, not meaningful
            pkts.append({"t": "", "dir": d, "len": int(m.group(3)),
                         "id": int(m.group(4), 16),
                         "data": bytearray.fromhex(m.group(6)),
                         "name": m.group(8).strip()})
    return pkts


def detect_format(path):
    """Sniff the first ~40 lines for the hook vs rich signature."""
    with open(path, "r", errors="replace") as f:
        for _ in range(40):
            line = f.readline()
            if not line:
                break
            if HOOK_HDR.match(line):
                return "hook"
            if RICH_LINE.match(line.rstrip("\n")):
                return "rich"
    return "hook"


def load(path):
    fmt = detect_format(path)
    return (parse_rich(path) if fmt == "rich" else parse_hook(path)), fmt


# ---------------------------------------------------------------------------
# protobuf decoding (wire types only - no .proto needed)
# ---------------------------------------------------------------------------

def rv(b, i):
    v, s, n = 0, 0, 0
    while i < len(b):
        x = b[i]; i += 1
        v |= (x & 0x7F) << s
        n += 1
        if not (x & 0x80):
            return v, i
        s += 7
        if n > 10:
            break
    return None, i


def fields(body, maxn=48):
    out, i = [], 0
    while i < len(body) and len(out) < maxn:
        tag, i = rv(body, i)
        if tag is None or tag == 0:
            break
        f, w = tag >> 3, tag & 7
        if f == 0:
            break
        if w == 0:
            v, i = rv(body, i)
            if v is None:
                break
            out.append((f, "V", v))
        elif w == 1:
            if i + 8 > len(body):
                break
            out.append((f, "F", body[i:i + 8].hex()))
            i += 8
        elif w == 2:
            n, i = rv(body, i)
            if n is None or i + n > len(body):
                break
            out.append((f, "L", body[i:i + n]))
            i += n
        elif w == 5:
            if i + 4 > len(body):
                break
            out.append((f, "F", body[i:i + 4].hex()))
            i += 4
        else:
            break
    return out


def is_text(v):
    if not v:
        return False
    printable = sum(1 for c in v if 32 <= c < 127 or c in (9, 10, 13))
    return printable >= len(v) * 0.9


def describe(f, w, v, depth=0, show_strings=True):
    if w == "V":
        return "f%d=%d" % (f, v)
    if w == "F":
        return "f%d=0x%s" % (f, v)
    if len(v) == 0:
        return "f%d=<empty>" % f
    if is_text(v) and show_strings:
        s = v.decode("ascii", "replace")
        if len(s) > 56:
            s = s[:53] + "..."
        return 'f%d="%s"' % (f, s)
    if depth < 1:
        sub = fields(v, 10)
        if sub:
            inner = ", ".join(describe(sf, sw, sv, depth + 1, show_strings)
                              for sf, sw, sv in sub[:6])
            more = ", ..." if len(sub) > 6 else ""
            return "f%d={ %s%s }" % (f, inner, more)
    return "f%d=<%dB %s>" % (f, len(v), v[:10].hex())


def decode_body(body, show_strings=True):
    """Decode the protobuf body (everything after the 4-byte header)."""
    if len(body) <= 4:
        return None, []
    payload = body[4:]
    fs = fields(payload, 20)
    if not fs:
        return None, []
    txt = ", ".join(describe(f, w, v, 0, show_strings) for f, w, v in fs[:12])
    strings = sorted({s.decode("ascii", "replace")
                      for s in re.findall(rb"[ -~]{5,}", payload)}) if show_strings else []
    return txt, strings


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------

CLR = {"reset": "\033[0m", "dim": "\033[2m", "red": "\033[31m",
       "green": "\033[32m", "yellow": "\033[33m", "cyan": "\033[36m",
       "bold": "\033[1m"}


def c(s, key, use):
    return CLR[key] + s + CLR["reset"] if use else s


def meaning_for(pkt):
    e = CATALOG.get(pkt["id"])
    name = (e or {}).get("name") or ""
    if not name and pkt.get("name"):
        name = pkt["name"]
    if not name:
        name = "Unknown"
    desc = (e or {}).get("desc", "")
    return name, desc


def render_stream(pkts, args):
    use_color = args.color or (sys.stdout.isatty() and not args.json)
    shown = 0
    for p in pkts:
        if args.dir and not p["dir"].startswith(args.dir):
            continue
        if args.id and p["id"] not in args.id:
            continue
        if args.limit and shown >= args.limit:
            break
        name, desc = meaning_for(p)
        dcol = "yellow" if p["dir"] == "SEND" else "green"
        head = "%s %4s len=%-4d %-4s id=0x%04X" % (
            c(p["dir"], dcol, use_color), "", p["len"], "", p["id"])
        head = head.replace("   ", " ")  # tidy
        line = "%s  %s  %s  %s" % (
            c(p["dir"], dcol, use_color),
            c("0x%04X" % p["id"], "cyan", use_color),
            c("%-18s" % name, "bold", use_color),
            c("len=%d" % p["len"], "dim", use_color))
        if p["t"]:
            line = c(p["t"], "dim", use_color) + "  " + line
        print(line)
        if desc:
            print("        " + c(desc, "dim", use_color))
        txt, strings = decode_body(bytes(p["data"]), not args.no_strings)
        if txt:
            print("        pb: " + txt)
        elif p["data"]:
            print("        raw: " + bytes(p["data"][:32]).hex(" "))
        for s in strings[:2]:
            print("        txt: " + repr(s))
        shown += 1
    return shown


def render_summary(pkts, args):
    agg = collections.defaultdict(list)
    for p in pkts:
        agg[(p["dir"], p["id"])].append(p)
    print("%-6s %-5s %-7s %-12s %s" % ("id", "dir", "count", "len", "name / meaning"))
    print("-" * 96)
    for (d, i), lst in sorted(agg.items(), key=lambda kv: -len(kv[1])):
        lens = [x["len"] for x in lst]
        rng = "%d" % lens[0] if min(lens) == max(lens) else "%d-%d" % (min(lens), max(lens))
        name, desc = meaning_for(lst[0])
        print("%-6s %-5s %-7d %-12s %s%s" % (
            "0x%04X" % i, d, len(lst), rng, name,
            ("  -- " + desc) if desc and not args.no_strings else ""))
    print("-" * 96)
    print("%d packets, %d distinct (dir,id) pairs" % (len(pkts), len(agg)))


def render_json(pkts, args):
    shown = 0
    for p in pkts:
        if args.dir and not p["dir"].startswith(args.dir):
            continue
        if args.id and p["id"] not in args.id:
            continue
        if args.limit and shown >= args.limit:
            break
        name, desc = meaning_for(p)
        txt, strings = decode_body(bytes(p["data"]), not args.no_strings)
        print(json.dumps({"t": p["t"], "dir": p["dir"], "id": "0x%04X" % p["id"],
                          "name": name, "desc": desc, "len": p["len"],
                          "pb": txt, "strings": strings[:3]}))
        shown += 1


def render_catalog(args):
    print("%-6s %-3s %-20s %s" % ("id", "dir", "name", "meaning"))
    print("-" * 100)
    for mid in sorted(CATALOG):
        e = CATALOG[mid]
        print("%-6s %-3s %-20s %s" % ("0x%04X" % mid, e["dir"], e["name"], e["desc"]))
    print("-" * 100)
    print("%d ids in catalogue" % len(CATALOG))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile", nargs="?")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--dir", choices=["S", "R", "SEND", "RECV"])
    ap.add_argument("--id", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--catalog", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--no-strings", action="store_true")
    ap.add_argument("--color", action="store_true")
    args = ap.parse_args()

    if args.dir == "SEND":
        args.dir = "S"
    elif args.dir == "RECV":
        args.dir = "R"

    if args.id:
        ids = set()
        for tok in args.id.split(","):
            tok = tok.strip()
            if tok:
                ids.add(int(tok, 16) if tok.lower().startswith("0x") else int(tok, 16))
        args.id = ids
    else:
        args.id = None

    if args.catalog:
        render_catalog(args)
        return
    if not args.logfile:
        ap.error("logfile required (or use --catalog)")

    pkts, fmt = load(args.logfile)
    if args.json:
        render_json(pkts, args)
    elif args.summary:
        render_summary(pkts, args)
    else:
        n = render_stream(pkts, args)
        print("\n-- %d packets parsed (%s format), %d shown --" % (len(pkts), fmt, n))


if __name__ == "__main__":
    main()
