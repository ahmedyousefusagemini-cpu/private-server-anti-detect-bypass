"""Deep-decode the Conquer packet log.

Wire format: [uint16 total_len][uint16 msg_id][protobuf body]

Produces a per-message-ID report: how often it flows in each direction, its
size range, the protobuf fields it carries (with real values), any embedded
strings, and the opening sequence of the session.
"""
import re, sys, collections, os

hdr = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]\s+(SEND|RECV)\s+len=(\d+)\s+id=0x([0-9A-Fa-f]+)')
row = re.compile(r'^\s+([0-9A-F]{4})\s+((?:[0-9A-F]{2}\s+)+)\s*\|')


def parse(path):
    pkts, cur = [], None
    with open(path, 'r', errors='replace') as f:
        for line in f:
            m = hdr.match(line)
            if m:
                cur = {'dir': m.group(5), 'len': int(m.group(6)), 'id': int(m.group(7), 16),
                       't': f"{m.group(1)}:{m.group(2)}:{m.group(3)}.{m.group(4)}", 'data': bytearray()}
                pkts.append(cur)
                continue
            r = row.match(line)
            if r and cur is not None:
                cur['data'] += bytes.fromhex(r.group(2).replace(' ', ''))
    return pkts


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


def fields(body, maxn=64):
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
            out.append((f, 'V', v))
        elif w == 1:
            if i + 8 > len(body):
                break
            out.append((f, 'F', body[i:i + 8].hex()))
            i += 8
        elif w == 2:
            n, i = rv(body, i)
            if n is None or i + n > len(body):
                break
            out.append((f, 'L', body[i:i + n]))
            i += n
        elif w == 5:
            if i + 4 > len(body):
                break
            out.append((f, 'F', body[i:i + 4].hex()))
            i += 4
        else:
            break
    return out


def describe(f, w, v, depth=0):
    if w == 'V':
        return f"f{f}={v}"
    if w == 'F':
        return f"f{f}=0x{v}"
    # length-delimited
    if len(v) == 0:
        return f"f{f}=<empty>"
    printable = sum(1 for c in v if 32 <= c < 127)
    if printable >= len(v) * 0.9:
        s = v.decode('ascii', 'replace')
        if len(s) > 60:
            s = s[:57] + '...'
        return f'f{f}="{s}"'
    if depth < 1:
        sub = fields(v, 12)
        if sub and len(sub) >= 1:
            inner = ", ".join(describe(sf, sw, sv, depth + 1) for sf, sw, sv in sub[:8])
            more = f", +{len(sub)-8}" if len(sub) > 8 else ""
            return f"f{f}={{ {inner}{more} }}"
    return f"f{f}=<{len(v)}B {v[:12].hex()}>"


def main():
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None

    pkts = parse(path)
    agg = collections.defaultdict(list)
    for p in pkts:
        agg[(p['dir'], p['id'])].append(p)

    by_id = collections.defaultdict(dict)
    for (d, i), lst in agg.items():
        by_id[i][d] = lst

    L = []
    A = L.append
    A(f"# Packet ID reference — decoded from `{os.path.basename(path)}`")
    A("")
    A(f"{len(pkts)} packets, {len(agg)} distinct (direction, id) pairs.")
    A("")
    A("Envelope is `[uint16 total_len][uint16 msg_id]`; the body is Protocol")
    A("Buffers. Below, `fN` is the protobuf field number, `V` varint, `L`")
    A("length-delimited (string or nested message), `F` fixed-width.")
    A("")
    A("## Opening sequence (login handshake)")
    A("")
    A("```")
    for p in pkts[:14]:
        body = bytes(p['data'][4:])
        dec = ", ".join(describe(f, w, v) for f, w, v in fields(body, 8)) or "(non-protobuf / opaque)"
        A(f"{p['t']}  {p['dir']:<4} 0x{p['id']:04X} len={p['len']:<5} {dec}")
    A("```")
    A("")
    A("## All message IDs")
    A("")

    for i in sorted(by_id):
        dirs = by_id[i]
        total = sum(len(v) for v in dirs.values())
        A(f"### 0x{i:04X}  ({i})   {total} packets")
        A("")
        for d in ('SEND', 'RECV'):
            if d not in dirs:
                continue
            lst = dirs[d]
            lens = [p['len'] for p in lst]
            rng = f"{min(lens)}" if min(lens) == max(lens) else f"{min(lens)}-{max(lens)}"
            A(f"- **{d}** x{len(lst)}  size {rng} B")
            sample = max(lst, key=lambda p: len(p['data']))
            body = bytes(sample['data'][4:])
            dec = ", ".join(describe(f, w, v) for f, w, v in fields(body, 14))
            A(f"  - `{dec}`" if dec else "  - (opaque / non-protobuf)")
            strings = sorted({s.decode('ascii', 'replace') for s in re.findall(rb'[ -~]{5,}', body)})
            if strings:
                shown = strings[:4]
                extra = f" (+{len(strings)-4} more)" if len(strings) > 4 else ""
                A(f"  - strings: {' | '.join(repr(s) for s in shown)}{extra}")
        A("")

    text = "\n".join(L) + "\n"
    if out_path:
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(text)
        print(f"wrote {out_path} ({len(text)} bytes)")
    else:
        print(text)


main()
