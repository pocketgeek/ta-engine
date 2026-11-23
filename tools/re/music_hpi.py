"""Minimal read-only TA HPI v1 lister/extractor for research (no data copied into engine).
Usage: music_hpi.py ARCHIVE list [regex]
       music_hpi.py ARCHIVE get PATH OUTFILE"""
import sys, struct, zlib, re

class Hpi:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(20)
        magic, ver, dsize, hkey, start = struct.unpack('<4sIIII', h)
        assert magic == b'HAPI' and ver == 0x00010000, (magic, hex(ver))
        self.key = (~((hkey << 2) | (hkey >> 6))) & 0xff if hkey else 0
        self.dir = bytearray(dsize)
        self.dir[start:] = self.read(start, dsize - start)
        self.start = start

    def read(self, pos, n):
        self.f.seek(pos); b = bytearray(self.f.read(n))
        if self.key:
            for i in range(len(b)):
                t = (pos + i) ^ self.key
                b[i] = (t ^ ~b[i]) & 0xff
        return b

    def cstr(self, o):
        e = self.dir.index(0, o); return self.dir[o:e].decode('latin1')

    def walk(self, off=None, prefix=''):
        if off is None: off = self.start
        n, lst = struct.unpack_from('<II', self.dir, off)
        for i in range(n):
            no, do, fl = struct.unpack_from('<IIB', self.dir, lst + 9 * i)
            name = prefix + self.cstr(no)
            if fl == 1: yield from self.walk(do, name + '/')
            else:
                dofs, size, comp = struct.unpack_from('<IIB', self.dir, do)
                yield name, dofs, size, comp

    def extract(self, dofs, size, comp):
        if comp == 0: return bytes(self.read(dofs, size))
        nch = (size + 65535) // 65536
        sizes = struct.unpack('<%dI' % nch, self.read(dofs, 4 * nch))
        pos = dofs + 4 * nch; out = bytearray()
        for cs in sizes:
            ch = self.read(pos, cs); pos += cs
            assert ch[:4] == b'SQSH'
            method, enc = ch[5], ch[6]
            csize, dsize = struct.unpack_from('<II', ch, 7)
            data = bytearray(ch[19:19 + csize])
            if enc:
                for i in range(len(data)): data[i] = ((data[i] - i) ^ i) & 0xff
            if method == 2: out += zlib.decompress(bytes(data))
            elif method == 1: out += lz77(data, dsize)
            else: out += data
        return bytes(out[:size])

def lz77(src, dsize):
    out = bytearray(); win = bytearray(4096); wp = 1; ip = 0
    while True:
        tag = src[ip]; ip += 1
        for bit in range(8):
            if not (tag >> bit) & 1:
                c = src[ip]; ip += 1
                out.append(c); win[wp] = c; wp = (wp + 1) & 0xfff
            else:
                cnt = src[ip] | (src[ip + 1] << 8); ip += 2
                off = cnt >> 4
                if off == 0: return bytes(out)
                n = (cnt & 0xf) + 2
                for _ in range(n):
                    c = win[off]; off = (off + 1) & 0xfff
                    out.append(c); win[wp] = c; wp = (wp + 1) & 0xfff
            if len(out) >= dsize: return bytes(out)

if __name__ == '__main__':
    h = Hpi(sys.argv[1])
    if sys.argv[2] == 'list':
        rx = re.compile(sys.argv[3], re.I) if len(sys.argv) > 3 else None
        for name, d, s, c in h.walk():
            if not rx or rx.search(name): print(name, s, c)
    else:
        for name, d, s, c in h.walk():
            if name.lower() == sys.argv[3].lower():
                open(sys.argv[4], 'wb').write(h.extract(d, s, c)); break
        else: sys.exit('not found')
