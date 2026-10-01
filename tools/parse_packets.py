import re, sys, collections

path = sys.argv[1]
hdr = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]\s+(SEND|RECV)\s+len=(\d+)\s+id=0x([0-9A-Fa-f]+)')
row = re.compile(r'^\s+([0-9A-F]{4})\s+((?:[0-9A-F]{2}\s+)+)\s*\|')

pkts = []
cur = None
with open(path, 'r', errors='replace') as f:
    for line in f:
        m = hdr.match(line)
        if m:
            cur = {'dir': m.group(5), 'len': int(m.group(6)),
                   'id': int(m.group(7), 16), 'data': bytearray(),
                   't': f"{m.group(1)}:{m.group(2)}:{m.group(3)}.{m.group(4)}"}
            pkts.append(cur)
            continue
        r = row.match(line)
        if r and cur is not None:
            cur['data'] += bytes.fromhex(r.group(2).replace(' ', ''))

print(f"total packets parsed: {len(pkts)}")
print()

agg = collections.defaultdict(list)
for p in pkts:
    agg[(p['dir'], p['id'])].append(p)

print(f"distinct (dir,id) pairs: {len(agg)}")
print()
print(f"{'id':>7} {'dir':<5} {'count':>7} {'len(min/max)':>13}  sample payload (first 40 bytes)")
print('-' * 110)
for (d, i), lst in sorted(agg.items(), key=lambda kv: (-len(kv[1]))):
    lens = [p['len'] for p in lst]
    sample = max(lst, key=lambda p: len(p['data']))
    hexs = sample['data'][:40].hex(' ')
    print(f"0x{i:04X} {d:<5} {len(lst):>7} {min(lens):>6}/{max(lens):<6}  {hexs}")
