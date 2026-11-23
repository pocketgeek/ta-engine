"""Minimal static-analysis helpers for TotalA.exe (observe only; nothing from
the binary is copied into the engine). Usage: import pe; b = pe.Bin(path)."""
import struct, re, pefile, capstone

class Bin:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = bytearray(self.pe.get_memory_mapped_image())
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False
        t = [s for s in self.pe.sections if s.Name.startswith(b'.text')][0]
        self.text = (self.base + t.VirtualAddress, self.base + t.VirtualAddress + t.Misc_VirtualSize)
        self.imports = {}
        for e in self.pe.DIRECTORY_ENTRY_IMPORT:
            for i in e.imports:
                self.imports[i.address] = (e.dll.decode(), i.name.decode() if i.name else i.ordinal)

    def rd(self, va, n): return bytes(self.img[va - self.base: va - self.base + n])
    def u32(self, va): return struct.unpack('<I', self.rd(va, 4))[0]
    def cstr(self, va):
        o = va - self.base; e = self.img.index(0, o); return self.img[o:e].decode('latin1')

    def strings(self, pat, minlen=4):
        rx = re.compile(rb'[\x20-\x7e]{%d,}' % minlen)
        out = []
        for m in rx.finditer(self.img):
            s = m.group().decode('latin1')
            if re.search(pat, s, re.I): out.append((self.base + m.start(), s))
        return out

    def xrefs(self, va):
        """Code locations holding va as a 32-bit immediate/displacement."""
        needle = struct.pack('<I', va); out = []
        lo, hi = self.text
        o = lo - self.base
        seg = bytes(self.img[o: hi - self.base])
        i = seg.find(needle)
        while i >= 0:
            out.append(lo + i); i = seg.find(needle, i + 1)
        return out

    def dis(self, va, n=40, stop_ret=False):
        lines = []
        for ins in self.md.disasm(self.rd(va, n * 8), va):
            s = f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}'
            m = re.search(r'\[0x([0-9a-f]+)\]', ins.op_str)
            if m and int(m.group(1), 16) in self.imports: s += f'   ; {self.imports[int(m.group(1),16)]}'
            for h in re.findall(r'0x([0-9a-f]{6,8})', ins.op_str):
                v = int(h, 16)
                if self.base + 0xfc000 <= v < self.base + 0x12c000:
                    try:
                        c = self.cstr(v)
                        if len(c) >= 3 and all(32 <= ord(ch) < 127 for ch in c): s += f'   ; "{c[:60]}"'
                    except Exception: pass
            lines.append(s)
            if len(lines) >= n or (stop_ret and ins.mnemonic == 'ret'): break
        return '\n'.join(lines)

    def func_start(self, va, maxback=0x2000):
        """Heuristic: walk back to the nearest 'push ebp; mov ebp,esp' or post-int3/ret padding."""
        for a in range(va, va - maxback, -1):
            b = self.rd(a, 3)
            if b[:3] == b'\x55\x8b\xec': return a
            if self.rd(a - 1, 1) in (b'\xcc', b'\xc3', b'\x90') and self.rd(a, 1) in (b'\x55', b'\x53', b'\x56', b'\x57', b'\x83', b'\x81', b'\x8b', b'\x6a', b'\x51'):
                pass
        return None
