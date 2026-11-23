"""Unicorn harness: run TotalA.exe's own TDF/FBI/weapon parsers on real data and
dump what they store (observe only; nothing from the binary goes into the engine).

usage: data_emu.py tdf            # TDF syntax edge cases via retail getters
       data_emu.py unit NAME      # run unit parser 0x42bf40 on units/NAME.fbi
       data_emu.py weapon FILE N  # run weapon parser 0x42e440 on section N of weapons/FILE.tdf
       data_emu.py move           # run moveinfo loader 0x42d2e0 on gamedata/moveinfo.tdf
"""
import sys, struct, os
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.x86_const import *

EXE = '/home/pocket_geek/ta_install/TotalA.exe'
DATA = '/home/pocket_geek/TA/assets/tadata'
b = pe.Bin(EXE)

GAME, STACK, SCR, STOP, HEAP = 0x10000000, 0x12000000, 0x13000000, 0x14000000, 0x20000000
GAMESZ, HEAPSZ = 0x400000, 0x4000000
NOOP_IMPORTS = {'EnterCriticalSection': 1, 'LeaveCriticalSection': 1, 'InitializeCriticalSection': 1, 'HeapFree': 3}


class Emu:
    def __init__(self):
        u = self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        size = (len(b.img) + 0xfff) & ~0xfff
        u.mem_map(b.base, size)
        u.mem_write(b.base, bytes(b.img))
        for a, n in ((GAME, GAMESZ), (STACK, 0x100000), (SCR, 0x10000), (STOP, 0x1000), (HEAP, HEAPSZ)):
            u.mem_map(a, n)
        u.mem_write(STOP, b'\xf4')
        self.w32(0x511de8, GAME)
        self.brk = HEAP + 0x10
        self.fpcw = 0x27f
        self.stubs = {}
        self.files = {}
        u.hook_add(UC_HOOK_CODE, self._code)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self._bad)
        self.iat = {}
        for slot, (dll, name) in b.imports.items():
            fake = SCR + 0x8000 + len(self.iat) * 4
            self.iat[fake] = name
            self.w32(slot, fake)
        malloc1 = lambda e: e.ret(e.alloc(e.arg(0)), 0)          # cdecl(size)
        for a in (0x4d83c0, 0x4d8660, 0x4e8890, 0x4b4f10):
            self.stubs[a] = malloc1
        self.stubs[0x4d83b0] = lambda e: e.ret(e.alloc(e.arg(1)), 0)  # (tag, size)
        def realloc(e):
            old, n = e.arg(0), e.arg(2)
            p = e.alloc(n)
            if old: e.u.mem_write(p, bytes(e.u.mem_read(old, min(n, 0x10000))))
            e.ret(p, 0)
        self.stubs[0x4d84a0] = realloc
        for a in (0x4d8670, 0x4d85a0, 0x4b4f20, 0x4d8780, 0x4d8710):
            self.stubs[a] = lambda e: e.ret(0, 0)

    def alloc(self, n):
        p = self.brk; self.brk = (self.brk + n + 0x1f) & ~0xf
        self.u.mem_write(p, b'\0' * max(n, 1))
        return p

    def _bad(self, u, access, addr, size, value, _):
        print('unmapped access', hex(addr), 'at eip', hex(u.reg_read(UC_X86_REG_EIP)))
        return False

    def w32(self, a, v): self.u.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def r32(self, a): return struct.unpack('<I', self.u.mem_read(a, 4))[0]
    def s32(self, a): return struct.unpack('<i', self.u.mem_read(a, 4))[0]
    def s16(self, a): return struct.unpack('<h', self.u.mem_read(a, 2))[0]
    def u16(self, a): return struct.unpack('<H', self.u.mem_read(a, 2))[0]
    def u8(self, a): return self.u.mem_read(a, 1)[0]
    def f32(self, a): return struct.unpack('<f', self.u.mem_read(a, 4))[0]
    def cstr(self, a, n=256):
        s = bytes(self.u.mem_read(a, n)); return s[:s.index(0)].decode('latin1') if 0 in s else s.decode('latin1')
    def arg(self, i): return self.r32(self.u.reg_read(UC_X86_REG_ESP) + 4 + 4 * i)
    def put(self, s):
        if isinstance(s, str): s = s.encode('latin1')
        p = self.alloc(len(s) + 1); self.u.mem_write(p, s + b'\0'); return p

    def ret(self, eax, nargs):
        u = self.u; esp = u.reg_read(UC_X86_REG_ESP)
        ra = self.r32(esp)
        u.reg_write(UC_X86_REG_EAX, eax & 0xffffffff)
        u.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * nargs)
        u.reg_write(UC_X86_REG_EIP, ra)

    def _code(self, u, addr, size, _):
        if addr in self.stubs:
            self.stubs[addr](self)
        elif addr in self.iat and self.iat[addr] in NOOP_IMPORTS:
            self.ret(0, NOOP_IMPORTS[self.iat[addr]])
        elif addr in self.iat:
            raise RuntimeError('unstubbed import %s (from %s)' % (self.iat[addr], hex(self.r32(u.reg_read(UC_X86_REG_ESP)))))

    def call(self, fn, args=(), ecx=0):
        u = self.u
        esp = STACK + 0xf0000
        for a in reversed(args):
            esp -= 4; self.w32(esp, a)
        esp -= 4; self.w32(esp, STOP)
        u.reg_write(UC_X86_REG_ESP, esp)
        u.reg_write(UC_X86_REG_ECX, ecx)
        u.reg_write(UC_X86_REG_FPCW, self.fpcw)  # retail CRT init 0x4e4320 -> _controlfp(_PC_53)
        u.emu_start(fn, STOP, count=50_000_000)
        return u.reg_read(UC_X86_REG_EAX)

    # --- TDF object helpers (12-byte object: root, cur, crc) -------------------------
    def tdf_from_text(self, text, name='<test>'):
        obj = self.alloc(16)
        self.call(0x4c2ea0, ecx=obj)
        buf = self.put(text)
        self.call(0x4c3120, [buf, len(text), 0, self.put(name)], ecx=obj)
        return obj

    def hook_tdf_load(self, mapping):
        """Replace TDF_load(path) (thiscall, ret 4) by in-memory parse of mapping[path.lower()]."""
        def load(e):
            path = e.cstr(e.arg(0)).replace('/', '\\').lower()
            text = mapping(path)
            obj = e.u.reg_read(UC_X86_REG_ECX)
            if text is None:
                e.ret(0, 1); return
            buf = e.put(text)
            # emulate: obj->parse(buf,len,0,name) then return 1
            sub = Emu.__new__(Emu)
            esp = e.u.reg_read(UC_X86_REG_ESP); ra = e.r32(esp)
            # rewrite frame: [ra][path] -> [ra2=STUBRET][buf][len][0][name]
            e.w32(SCR + 0x7000, 0xc3c03158)  # not used; placeholder
            esp_new = esp + 8 - 20
            e.w32(esp_new, SCR + 0x7100)
            e.w32(esp_new + 4, buf); e.w32(esp_new + 8, len(text)); e.w32(esp_new + 12, 0)
            e.w32(esp_new + 16, e.put(path))
            e._pending_ra = ra
            e.u.reg_write(UC_X86_REG_ESP, esp_new)
            e.u.reg_write(UC_X86_REG_EIP, 0x4c3120)
        def back(e):
            esp = e.u.reg_read(UC_X86_REG_ESP)
            e.u.reg_write(UC_X86_REG_EAX, 1)
            e.u.reg_write(UC_X86_REG_EIP, e._pending_ra)
        self.stubs[0x4c2f60] = load
        self.stubs[SCR + 0x7100] = back

    def get_int(self, obj, key, d=0):
        return self.call(0x4c46c0, [self.put(key), d & 0xffffffff], ecx=self.r32(obj + 4))
    def get_str(self, obj, key, n=64, d=''):
        dst = self.alloc(n)
        r = self.call(0x4c48c0, [dst, self.put(key), n, self.put(d)], ecx=self.r32(obj + 4))
        return r, self.cstr(dst)
    def get_fixed(self, obj, key, d=0):
        out = self.alloc(4)
        self.call(0x4c4800, [out, self.put(key), d], ecx=self.r32(obj + 4))
        return self.s32(out)
    def enter(self, obj, sect):
        return self.call(0x4c3410, [self.put(sect)], ecx=obj)
    def reset(self, obj): self.call(0x4c3e10, ecx=obj)


def read(p):
    with open(p, 'rb') as f: return f.read().decode('latin1')


# --------------------------------------------------------------------------- tests
def t_tdf():
    e = Emu()
    txt = ('// line comment [FAKE]{a=1;}\n'
           '[S]\n{\n  Key = 12;\n  key=13;\n  KEY = 14 ;\n  hex=0x1F;\n  flt=2.9;\n  neg=-2.9;\n'
           '  sp = a b  c ;\n  /* block\n  dropped=1; */ after=5;\n  url=http://x;\n  lead= +7;\n'
           '  big=4294967297;\n  fx=1.5;\n  fxn=-0.00001;\n  long=0123456789abcdef0123456789ABCDEF0123;\n'
           '  [SUB]{ k=1; }\n}\n[s]\n{ key=99; other=1; }\n[S2]{ a=1; } trailing=3;\n')
    o = e.tdf_from_text(txt)
    print('enter S:', e.enter(o, 'S'))
    for k in ('key', 'KEY', 'Key', 'hex', 'flt', 'neg', 'after', 'dropped', 'lead', 'big', 'missing'):
        print(f'  GetInt({k!r}, def=-7) =', e.s32(0) if False else struct.unpack('<i', struct.pack('<I', e.get_int(o, k, -7)))[0])
    for k in ('sp', 'url', 'long'):
        print(f'  GetStr({k!r}, max=16) =', e.get_str(o, k, 16))
    for k in ('fx', 'fxn', 'flt', 'neg', 'missing'):
        print(f'  GetFixed16({k!r}, def=0x10000) =', hex(e.get_fixed(o, k, 0x10000) & 0xffffffff))
    print('  key in nested [SUB] visible from S?', e.get_int(o, 'k', -1))
    e.reset(o); print('enter s (dup section, case):', e.enter(o, 's'), ' other =', e.get_int(o, 'other', -1), ' key =', e.get_int(o, 'key', -1))
    e.reset(o); print('enter FAKE (inside comment):', e.enter(o, 'FAKE'))
    e.reset(o); print('enter SUB from root:', e.enter(o, 'SUB'))
    e.reset(o); print('root key trailing (after [S2]):', e.get_int(o, 'trailing', -1) if False else 'n/a')


def unit_env(e):
    e.stubs[0x422e40] = lambda e: e.ret(0x1234, 1)          # corpse -> feature index (stub)
    e.stubs[0x4cb560] = lambda e: e.ret(e.alloc(64), 1)
    files = {}
    def mapping(path):
        rel = path.lstrip('\\')
        p = os.path.join(DATA, rel.replace('\\', '/'))
        return read(p) if os.path.exists(p) else None
    e.hook_tdf_load(mapping)


UNIT_FIELDS = [  # (off, type, label) observed stores in 0x42bf40
    (0x00, 's32', 'name'), (0x20, 's32', 'unitname'), (0x40, 's64', 'description'), (0x80, 's32', 'objectname'),
    (0x14a, 'i16', 'footprintx'), (0x14c, 'i16', 'footprintz'), (0x14e, 'u32', 'yardmap ptr'),
    (0x15e, 'i32', 'bbox minx'), (0x166, 'i32', 'bbox minz'), (0x16a, 'i32', 'bbox maxx'), (0x172, 'i32', 'bbox maxz'),
    (0x176, 'i32', 'size x'), (0x17e, 'i32', 'size z'), (0x182, 'i32', '(sx+sz)/3'),
    (0x186, 'f32', 'buildcostenergy'), (0x18a, 'f32', 'buildcostmetal'), (0x192, 'i32', 'maxvelocity fx'),
    (0x196, 'i32', 'maxvel/(maxslope+1) fx'), (0x19a, 'i32', 'brakerate fx'), (0x19e, 'i32', 'acceleration fx'),
    (0x1a2, 'i32', 'bankscale fx'), (0x1a6, 'i32', 'pitchscale fx'), (0x1aa, 'i32', 'damagemodifier fx'),
    (0x1ae, 'i32', 'moverate1 fx'), (0x1b2, 'i32', 'moverate2 fx'), (0x1b6, 'u32', 'movementclass ptr'),
    (0x1ba, 'i16', 'turnrate'), (0x1bc, 'u16', 'corpse feature'), (0x1be, 'i16', 'maxwaterdepth'),
    (0x1c0, 'i16', 'minwaterdepth'), (0x1c2, 'f32', 'energymake'), (0x1c6, 'f32', 'energyuse'), (0x1ca, 'f32', 'metalmake'),
    (0x1ce, 'f32', 'extractsmetal'), (0x1d2, 'f32', 'windgenerator'), (0x1d6, 'f32', 'tidalgenerator'),
    (0x1da, 'f32', 'cloakcost'), (0x1de, 'f32', 'cloakcostmoving'), (0x1e2, 'f32', 'energystorage'), (0x1e6, 'f32', 'metalstorage'),
    (0x1ea, 'i32', 'buildtime'), (0x1ee, 'u32', 'weapon1 ptr'), (0x1f2, 'u32', 'weapon2 ptr'), (0x1f6, 'u32', 'weapon3 ptr'),
    (0x1fa, 'i32', 'maxdamage'), (0x1fe, 'i16', 'workertime'), (0x200, 'i16', 'healtime'), (0x202, 'i16', 'sightdistance'),
    (0x204, 'i16', 'radardistance'), (0x206, 'i16', 'sonardistance'), (0x208, 'i16', 'mincloakdistance'),
    (0x20a, 'i16', 'radardistancejam'), (0x20c, 'i16', 'sonardistancejam'), (0x20e, 'u16', 'soundcategory'),
    (0x210, 'i16', 'buildangle'), (0x212, 'i16', 'builddistance'), (0x214, 'i16', 'maneuverleashlength'),
    (0x216, 'i16', 'attackrunlength'), (0x218, 'i16', 'kamikazedistance'), (0x21a, 'i16', 'sortbias'), (0x21c, 'i16', 'cruisealt'),
    (0x220, 'u32', 'explodeas ptr'), (0x224, 'u32', 'selfdestructas ptr'), (0x228, 'u8', 'maxslope'), (0x229, 'u8', 'maxwaterslope'),
    (0x22a, 'u8', 'transportsize'), (0x22b, 'u8', 'transportcapacity'), (0x22c, 'u8', 'waterline'), (0x22d, 'u8', 'makesmetal'),
    (0x22f, 'u8', 'bmcode'), (0x230, 'u8', 'defaultmissiontype'), (0x231, 'u32', 'wpri_badTargetCategory'),
    (0x241, 'x32', 'flagsA'), (0x245, 'x32', 'flagsB')]


def dump_fields(e, base, fields):
    for off, t, lab in fields:
        a = base + off
        v = {'s32': lambda: repr(e.cstr(a, 32)), 's64': lambda: repr(e.cstr(a, 64)), 'i16': lambda: e.s16(a),
             'u16': lambda: e.u16(a), 'u8': lambda: e.u8(a), 'i32': lambda: e.s32(a), 'u32': lambda: hex(e.r32(a)),
             'x32': lambda: hex(e.r32(a)), 'f32': lambda: e.f32(a)}[t]()
        if t == 'i32' and 'fx' in lab: v = f'{v} ({v / 65536:.5f})'
        print(f'  +0x{off:03x} {lab:28s} {v}')


def t_unit(name, extra=None):
    e = Emu(); unit_env(e)
    rec = e.alloc(0x249)
    path = f'units\\{name}.fbi'
    if extra is not None:
        e.hook_tdf_load(lambda p: extra)
    e.call(0x42bf40, [e.put(path), rec])
    print(f'== unit {name}')
    dump_fields(e, rec, UNIT_FIELDS)
    ym = e.r32(rec + 0x14e)
    if ym:
        fx, fz = e.s16(rec + 0x14a), e.s16(rec + 0x14c)
        cells = bytes(e.u.mem_read(ym, fx * fz))
        print('  yardmap cells:', ' '.join(f'{c:02x}' for c in cells))
    return e, rec


WEAP_FIELDS = [(0x00, 's32', 'section name'), (0x20, 's64', 'name'), (0x68, 'i32', 'weaponvelocity fx/tick'),
    (0x6c, 'i32', 'startvelocity fx/tick'), (0x70, 'i32', 'weaponacceleration fx/tick2'), (0xc0, 'f32', 'energypershot'),
    (0xc4, 'f32', 'metalpershot'), (0xc8, 'f32', 'minbarrelangle rad'), (0xcc, 'i32', 'shakemagnitude'),
    (0xd0, 'i32', 'shakeduration ticks'), (0xd4, 'i16', 'damage default'), (0xd6, 'i16', 'areaofeffect'),
    (0xd8, 'f32', 'edgeeffectiveness'), (0xdc, 'i32', 'range'), (0xe0, 'i32', 'coverage'), (0xe4, 'u16', 'reloadtime ticks'),
    (0xe6, 'u16', 'weapontimer ticks'), (0xe8, 'u16', 'turnrate/30'), (0xea, 'u16', 'burst'), (0xec, 'u16', 'burstrate ticks'),
    (0xee, 'u16', 'sprayangle'), (0xf0, 'u16', 'duration ticks'), (0xf2, 'u16', 'randomdecay ticks'),
    (0xf4, 'u16', 'soundstart id'), (0xf6, 'u16', 'soundhit id'), (0xf8, 'u16', 'soundwater id'),
    (0xfa, 'u16', 'smokedelay ticks'), (0xfc, 'u16', 'flighttime ticks'), (0xfe, 'u16', 'holdtime ticks'),
    (0x104, 'u16', 'accuracy'), (0x106, 'u16', 'tolerance'), (0x108, 'u16', 'pitchtolerance'), (0x10b, 'u8', 'firestarter'),
    (0x10c, 'u8', 'rendertype'), (0x10d, 'u8', 'color'), (0x10e, 'u8', 'color2'), (0x111, 'x32', 'flags'), (0x60, 'u32', 'aim handler')]


def t_weapon(fname, idx):
    e = Emu()
    e.stubs[0x429470] = lambda e: e.ret(0x55, 2)    # sound lookup stub
    e.stubs[0x429700] = lambda e: e.ret(0, 1)       # gaf load stub
    e.stubs[0x4cb560] = lambda e: e.ret(e.alloc(64), 1)
    e.stubs[0x4cb590] = lambda e: e.ret(0, 1)
    e.stubs[0x42a140] = lambda e: e.ret(0, 2)
    e.stubs[0x4b8d40] = lambda e: e.ret(e.alloc(16), 2)
    e.w32(GAME + 0x391e9, e.alloc(0x1000))          # map info (lava flag 0)
    o = e.tdf_from_text(read(f'{DATA}/weapons/{fname}.tdf'))
    e.reset(o)
    ok = e.call(0x4c3490, [idx], ecx=o)
    node = e.r32(o + 4)
    wid = e.get_int(o, 'ID', -1)
    print(f'== weapon {fname}[{idx}] enter={ok} ID={wid}')
    e.call(0x42e440, [node])
    base = GAME + 0x2cf3 + wid * 0x115
    dump_fields(e, base, WEAP_FIELDS)


def t_move():
    e = Emu(); unit_env(e)
    e.stubs[0x458160] = lambda e: e.ret(0, 0)
    e.stubs[0x4e4ac0] = lambda e: e.ret(0, 0)       # atexit
    e.call(0x440230)                                # static init of the 32 move classes (defaults)
    try:
        e.call(0x42d2e0)
    except Exception as ex:
        print('  (stopped after moveinfo part:', ex, ')')
    for i in range(32):
        a = 0x512358 + 0x20 * i
        p = e.r32(a)
        if not p: continue
        print(f'  CLASS{i}: {e.cstr(p)!r} fx={e.s16(a+4)} fz={e.s16(a+6)} maxwd={e.s16(a+8)} minwd={e.s16(a+10)} '
              f'maxslope={e.u8(a+12)} badslope={e.u8(a+13)} maxwslope={e.u8(a+14)} badwslope={e.u8(a+15)}')


if __name__ == '__main__':
    os.environ.setdefault('TMPDIR', '/home/pocket_geek/ta-tmp')
    cmd = sys.argv[1]
    if cmd == 'tdf': t_tdf()
    elif cmd == 'unit': t_unit(sys.argv[2])
    elif cmd == 'weapon': t_weapon(sys.argv[2], int(sys.argv[3]))
    elif cmd == 'move': t_move()
