"""Parse an Intel HEX text (optionally with a binary header before the first ':'), verify every
record checksum, report address ranges, and write a flat binary. usage: hex2bin.py <in> <out.bin>"""
import sys, re

raw = open(sys.argv[1], 'rb').read()
start = raw.index(b':')
print("header bytes before HEX:", raw[:start].hex(' '))
text = raw[start:].decode('ascii', 'replace')
mem, upper, bad, n = {}, 0, 0, 0
for line in re.findall(r':[0-9A-Fa-f]+', text):
    b = bytes.fromhex(line[1:])
    n += 1
    if sum(b) & 0xFF: bad += 1; continue
    ln, addr, typ, data = b[0], (b[1] << 8) | b[2], b[3], b[4:4 + b[0]]
    if typ == 0:
        for i, x in enumerate(data): mem[upper + addr + i] = x
    elif typ == 4: upper = int.from_bytes(data, 'big') << 16
    elif typ == 2: upper = int.from_bytes(data, 'big') << 4
    elif typ == 1: break
print(f"records {n}, bad checksums {bad}, data bytes {len(mem)}")
assert bad == 0, "checksum errors: decryption is wrong"
lo, hi = min(mem), max(mem)
# report contiguous ranges
ranges, s = [], lo
for a in range(lo, hi + 1):
    if a not in mem:
        if s is not None: ranges.append((s, a - 1)); s = None
    elif s is None: s = a
ranges.append((s, hi))
for r in ranges: print(f"  range 0x{r[0]:08x}-0x{r[1]:08x} ({r[1]-r[0]+1} bytes)")
out = bytes(mem.get(a, 0xFF) for a in range(lo, hi + 1))
open(sys.argv[2], 'wb').write(out)
print(f"wrote {sys.argv[2]}: base 0x{lo:08x}, {len(out)} bytes")
