"""Decode the Conquer packet log into a per-message-ID report.

Wire format: [uint16 total_len][uint16 msg_id][protobuf body]
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
    """read varint -> (value, next_index) or (None, i)"""
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


def fields(body, depth=0):
    """Yield (field_number, wire_type, value) for top-level protobuf fields."""
    out, i = [], 0
    while i < len(body):
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
            out.append((f, 0, v))
        elif w == 1:
            if i + 8 > len(body):
                break
            out.append((f, 1, body[i:i + 8].hex()))
            i += 8
        elif w == 2:
            n, i = rv(body, i)
            if n is None or i + n > len(body):
                break
            out.append((f, 2, body[i:i + n]))
            i += n
        elif w == 5:
            if i + 4 > len(body):
                break
            out.append((f, 5, body[i:i + 4].hex()))
            i += 4
        else:
            break
    return out


def printable(chunk, minlen=3):
    return re.findall(rb'[ -~]{%d,}' % minlen, chunk)


def main():
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None

    pkts = parse(path)
    agg = collections.defaultdict(list)
    for p in pkts:
        agg[(p['dir'], p['id'])].append(p)

    lines = []
    A = lines.append

    A(f"# Packet inventory - {os.path.basename(path)}")
    A("")
    A(f"{len(pkts)} packets, {len(agg)} distinct (direction, id) pairs.")
    A("")
    A("Body is Protocol Buffers. Fields shown as `fN:wiretype`; `L` = length-delimited")
    A("(string/sub-message), `V` = varint, `F` = fixed.")
    A("")
    A("| id | dir | count | len | protobuf fields | strings in payload |")
    A("|---|---|---|---|---|---|")
    for (d, i), lst in sorted(agg.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        lens = [p['len'] for p in lst]
        rng = f"{min(lens)}" if min(lens) == max(lens) else f"{min(lens)}-{max(lens)}"
        # union of top-level field signatures across all samples
        sig = collections.Counter()
        strings = set()
        for p in lst:
            body = bytes(p['data'][4:])
            for f, w, v in fields(body):
                sig[(f, w)] += 1
            for s in printable(body):
                strings.add(s.decode('ascii', 'replace'))
        fsig = " ".join(f"f{f}:{'VLF'[w] if w < 3 else '?'}" for f, w in sorted(sig))
        strs = " | ".join(sorted(strings)[:6])
        if len(strings) > 6:
            strs += f" (+{len(strings)-6})"
        A(f"| 0x{i:04X} | {d} | {len(lst)} | {rng} | {fsig} | {strs} |")

    text = "\n".join(lines) + "\n"
    if out_path:
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(text)
        print(f"wrote {out_path} ({len(text)} bytes)")
    else:
        print(text)


main()
