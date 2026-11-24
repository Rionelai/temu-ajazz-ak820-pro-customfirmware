"""Read-only scan of a Windows exe for hints of embedded keyboard firmware."""
import re, sys, struct

path = sys.argv[1]
data = open(path, 'rb').read()
print(f"== {path} ({len(data)} bytes)")

# Interesting ASCII / UTF-16 strings
pat = re.compile(rb'(?i)(vid|pid|hfd|8k|iap|isp|boot|upgrad|firmware|\.bin|version|0c45|3151|4348|1a86|ch32|wch|sonix|sn32|update|flash|erase|checksum|crc)')
seen = set()
for enc, rx in (('ascii', rb'[\x20-\x7e]{6,}'), ('utf16', rb'(?:[\x20-\x7e]\x00){6,}')):
    for m in re.finditer(rx, data):
        s = m.group().decode('utf-16-le' if enc == 'utf16' else 'ascii', 'replace')
        if pat.search(s.encode()) and s not in seen and len(s) < 160:
            seen.add(s)
            print(f"  [{enc} @0x{m.start():08x}] {s}")

# PE sections with entropy, to spot packed blobs
def entropy(b):
    if not b: return 0
    import math
    c = [0]*256
    for x in b: c[x] += 1
    return -sum(n/len(b)*math.log2(n/len(b)) for n in c if n)

pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe+6)[0]
optsz = struct.unpack_from('<H', data, pe+20)[0]
sec = pe + 24 + optsz
print("  sections:")
for i in range(nsec):
    name, vsize, vaddr, rsize, raddr = struct.unpack_from('<8sIIII', data, sec + 40*i)
    blob = data[raddr:raddr+rsize]
    print(f"    {name.rstrip(b'\0').decode(errors='replace'):8} raw=0x{raddr:08x} size={rsize:9d} entropy={entropy(blob[:2_000_000]):.2f}")
overlay = max(struct.unpack_from('<I', data, sec+40*i+20)[0] + struct.unpack_from('<I', data, sec+40*i+16)[0] for i in range(nsec))
print(f"  overlay (data appended after PE): {len(data)-overlay} bytes")
