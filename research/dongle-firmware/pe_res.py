"""List (and optionally dump) PE resources. Read-only on the input exe.
usage: pe_res.py <exe> [outdir]   -- outdir given: write each non-image resource blob there."""
import struct, sys, os

path = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else None
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
magic = struct.unpack_from('<H', d, pe + 24)[0]
ddir = pe + 24 + (96 if magic == 0x10b else 112)
rva_res, _ = struct.unpack_from('<II', d, ddir + 2 * 8)
secs = []
for i in range(nsec):
    _, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, pe + 24 + optsz + 40 * i)
    secs.append((va, max(vs, rs), ra))
def off(rva):
    for va, sz, ra in secs:
        if va <= rva < va + sz: return rva - va + ra
    raise ValueError(hex(rva))
base = off(rva_res)
TYPES = {1:'CURSOR',2:'BITMAP',3:'ICON',4:'MENU',5:'DIALOG',6:'STRING',9:'ACCEL',10:'RCDATA',12:'GROUP_CURSOR',14:'GROUP_ICON',16:'VERSION',24:'MANIFEST',240:'DLGINIT',241:'TOOLBAR'}
def name_of(entry_name):
    if entry_name & 0x80000000:
        o = base + (entry_name & 0x7fffffff)
        n = struct.unpack_from('<H', d, o)[0]
        return d[o + 2:o + 2 + 2 * n].decode('utf-16-le')
    return entry_name
def walk(o, path):
    _, _, _, _, nn, ni = struct.unpack_from('<IIHHHH', d, base + o)
    for i in range(nn + ni):
        nm, ptr = struct.unpack_from('<II', d, base + o + 16 + 8 * i)
        p = path + [name_of(nm)]
        if ptr & 0x80000000:
            yield from walk(ptr & 0x7fffffff, p)
        else:
            drva, dsz = struct.unpack_from('<II', d, base + ptr)
            yield p, off(drva), dsz
items = list(walk(0, []))
for p, o, sz in items:
    t = TYPES.get(p[0], p[0]) if isinstance(p[0], int) else p[0]
    head = d[o:o + 16].hex(' ')
    print(f"{str(t):14} {str(p[1]):24} lang={p[2]!s:5} size={sz:9d} @0x{o:08x}  {head}")
    if outdir and t not in ('ICON', 'GROUP_ICON', 'CURSOR', 'GROUP_CURSOR', 'MANIFEST', 'VERSION', 'DIALOG', 'STRING', 'MENU', 'ACCEL', 'TOOLBAR', 'DLGINIT'):
        os.makedirs(outdir, exist_ok=True)
        fn = os.path.join(outdir, f"{t}_{p[1]}.bin".replace('/', '_'))
        open(fn, 'wb').write(d[o:o + sz])
