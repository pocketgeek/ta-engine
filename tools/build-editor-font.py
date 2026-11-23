#!/usr/bin/env python3
"""Regenerate the embedded editor glyph tables (Pillow + fontTools, tooling only).

  build-editor-font.py                     res/editor-font/*  ->  src/cartographer/font{data,wide,width}.inc
  build-editor-font.py --subset-cjk TTC    rebuild res/editor-font/NotoSansMonoCJK-subset.otf from a local
                                           Noto Sans Mono CJK collection (the SC face, regular weight)

Cells: narrow glyphs (one text column, 6 UI px advance) are 16x32 alpha masks, run-length coded.
Wide glyphs (East Asian Width W/F: CJK, Hangul, fullwidth forms, emoji-style symbols; two columns)
are 24x24 masks at 2 bits per pixel. Which code points are wide comes from unicodedata and is
emitted as fontwidth.inc, so the C++ side never has to agree with a second table. Every source
font is SIL OFL 1.1 (EDITOR-FONT-LICENSE.txt); each file's SHA256 is recorded in the output headers.
"""
from pathlib import Path
import argparse, hashlib, sys, unicodedata
from PIL import Image, ImageDraw, ImageFont, features
from fontTools.ttLib import TTFont, TTCollection

root = Path(__file__).resolve().parents[1]
fontDir = root / 'res/editor-font'
outDir = root / 'src/cartographer'

# ---- sources ---------------------------------------------------------------
MONO = 'NotoSansMono-Regular.ttf'            # exact metrics: Latin, Greek, Cyrillic, box drawing...
CJK = 'NotoSansMonoCJK-subset.otf'           # kana, Hangul, common Han, fullwidth forms
SCRIPTS = [  # proportional fonts, fitted into one monospace cell
    ('NotoSansArmenian-VF.ttf', [(0x531, 0x58f)]),
    ('NotoSansGeorgian-VF.ttf', [(0x10a0, 0x10ff), (0x1c90, 0x1cbf), (0x2d00, 0x2d2f)]),
]
SYMBOL_FONTS = ['NotoSansSymbols-VF.ttf', 'NotoSansSymbols2-Regular.ttf']
SYMBOL_RANGES = [(0x2070, 0x209f), (0x20a0, 0x20cf), (0x2100, 0x218f), (0x2190, 0x23ff), (0x2460, 0x24ff),
                 (0x2600, 0x27bf), (0x27f0, 0x27ff), (0x2800, 0x28ff), (0x2900, 0x297f), (0x2b00, 0x2bff)]
SOURCES = [MONO, CJK] + [f for f, _ in SCRIPTS] + SYMBOL_FONTS

# ---- cell geometry (must match src/cartographer/editorfont.cpp) ---------------
NARROW = dict(w=16, h=32, ox=2, base=24, adv=12, size=20)   # glyph advance 12 px inside a 16 px cell
WIDE = dict(w=24, h=24, ox=0, base=21, adv=24, size=24)     # 24 px em box = two narrow advances

def isWide(code): return unicodedata.east_asian_width(chr(code)) in ('W', 'F')

# ---- CJK subset ---------------------------------------------------------------
def pairs(enc, leads, trails, lo, hi):
    out = set()
    for lead in leads:
        for trail in trails:
            try: ch = bytes([lead, trail]).decode(enc)
            except Exception: continue
            if len(ch) == 1 and lo <= ord(ch) <= hi: out.add(ord(ch))
    return out

def cjkPlan():
    """Code points worth embedding: the common-use sets of the legacy CJK charsets (GB 2312 level 1,
    JIS X 0208 level 1, Big5 level 1, KS X 1001 Hangul), kana, punctuation and fullwidth forms."""
    han = (0x4e00, 0x9fff)
    codes = pairs('gb2312', range(0xb0, 0xd8), range(0xa1, 0xff), *han)
    codes |= pairs('shift_jis', range(0x88, 0x98), range(0x40, 0xfd), *han)
    codes |= pairs('shift_jis', [0x98], range(0x40, 0x73), *han)
    codes |= pairs('big5', range(0xa4, 0xc7), list(range(0x40, 0x7f)) + list(range(0xa1, 0xff)), *han)
    codes |= pairs('euc_kr', range(0xb0, 0xc9), range(0xa1, 0xff), 0xac00, 0xd7a3)
    for lo, hi in [(0x3000, 0x303f), (0x3041, 0x3096), (0x3099, 0x30ff), (0x3105, 0x312f), (0x3131, 0x318e),
                   (0x31f0, 0x31ff), (0xff01, 0xff9f), (0xffe0, 0xffe6)]:
        codes |= set(range(lo, hi + 1))
    return codes

def subsetCjk(collection):
    from fontTools import subset
    from fontTools.varLib import instancer
    faces = TTCollection(collection).fonts
    face = next(f for f in faces if f['name'].getDebugName(1).endswith(' SC'))
    face = instancer.instantiateVariableFont(face, {'wght': 400})
    available = set(face.getBestCmap())
    plan = sorted(cjkPlan() & available)
    options = subset.Options(); options.layout_features = []; options.name_IDs = ['*']
    options.notdef_outline = True; options.glyph_names = False; options.hinting = False
    subsetter = subset.Subsetter(options); subsetter.populate(unicodes=plan); subsetter.subset(face)
    target = fontDir / CJK
    face.save(target)
    print(f'{CJK}: {len(plan)} code points, {target.stat().st_size} bytes')

# ---- rasterizing -------------------------------------------------------------
if not features.check('raqm'):
    print('warning: Pillow without raqm; existing glyph bitmaps will differ from the committed ones', file=sys.stderr)
_fonts = {}
def font(name, size):
    if (name, size) not in _fonts: _fonts[name, size] = ImageFont.truetype(str(fontDir / name), size)
    return _fonts[name, size]

def renderExact(name, code, cell):
    img = Image.new('L', (cell['w'], cell['h']))
    ImageDraw.Draw(img).text((cell['ox'], cell['base']), chr(code), font=font(name, cell['size']), fill=255, anchor='ls')
    return img

def inkBox(name, code, size):
    """Ink extent (l, t, r, b) relative to the pen origin and baseline. Pillow's getbbox spans the advance
    horizontally, which would shrink and shift glyphs drawn on a wider advance than their ink."""
    pad = 2 * size
    img = Image.new('L', (4 * size, 4 * size))
    ImageDraw.Draw(img).text((pad, 3 * size), chr(code), font=font(name, size), fill=255, anchor='ls')
    box = img.getbbox()
    return (box[0] - pad, box[1] - 3 * size, box[2] - pad, box[3] - 3 * size) if box else (0, 0, 0, 0)

def fitsExact(name, code, cell):
    """Whether the glyph's ink stays inside the cell at the font's own metrics. Noto Sans Mono draws some
    symbols (double arrows, fractions, circled operators, ...) on a two-column advance, and a few stacked
    Vietnamese capitals overshoot the ascent; drawn exactly they would be clipped."""
    l, t, r, b = inkBox(name, code, cell['size'])
    return l + cell['ox'] >= 0 and r + cell['ox'] <= cell['w'] and cell['base'] + t >= 0 and cell['base'] + b <= cell['h']

def renderFit(name, code, cell, maxw):
    """Proportional/foreign glyph: centre its ink on the cell's advance box, shrinking it until it fits."""
    size = cell['size']
    while True:
        l, t, r, b = inkBox(name, code, size)
        top, bottom = cell['base'], cell['h'] - cell['base']
        if (r - l <= maxw and -t <= top and b <= bottom) or size <= 8: break
        size -= 1
    img = Image.new('L', (cell['w'], cell['h']))
    x = cell['ox'] + round((cell['adv'] - (r - l)) / 2) - l
    ImageDraw.Draw(img).text((x, cell['base']), chr(code), font=font(name, size), fill=255, anchor='ls')
    return img

# ---- code point -> source ------------------------------------------------------
def cmapOf(name):
    return set(TTFont(fontDir / name).getBestCmap())

def plan():
    assigned = {}  # code -> (font, exact): exact = drawn at the font's own metrics, else fitted/centred
    cjk, cjkCodes = fontDir / CJK, set()
    if cjk.exists():
        cjkCodes = {c for c in cmapOf(CJK) if c >= 32}
        for c in cjkCodes:   # fullwidth forms etc. are designed for the CJK em box; prefer them over mono's lookalikes
            if isWide(c): assigned[c] = (CJK, True)
    for c in cmapOf(MONO):
        if c >= 32 and c not in assigned: assigned[c] = (MONO, not isWide(c))
    for c in cjkCodes:       # what is left of the CJK set (halfwidth katakana) is narrow
        if c not in assigned: assigned[c] = (CJK, False)
    for name, ranges in SCRIPTS:
        have = cmapOf(name)
        for lo, hi in ranges:
            for c in range(lo, hi + 1):
                if c in have and c not in assigned: assigned[c] = (name, False)
    for name in SYMBOL_FONTS:
        have = cmapOf(name)
        for lo, hi in SYMBOL_RANGES:
            for c in range(lo, hi + 1):
                if c in have and c not in assigned: assigned[c] = (name, False)
    return assigned

def sha(name): return hashlib.sha256((fontDir / name).read_bytes()).hexdigest()

def header(what):
    lines = [f'// Generated by tools/build-editor-font.py ({what}). Font data: SIL OFL 1.1; see EDITOR-FONT-LICENSE.txt.',
             f'// Unicode data (East Asian Width): {unicodedata.unidata_version}. Source SHA256 (res/editor-font/):']
    lines += [f'//   {n}  {sha(n)}' for n in SOURCES if (fontDir / n).exists()]
    return lines

def rle(pixels, data):
    offset = len(data)
    for value in pixels:
        if len(data) > offset and data[-1] == value and data[-2] < 255: data[-2] += 1
        else: data.extend((1, value))
    return offset

def pack2(pixels):
    out = bytearray()
    q = [min(3, (v + 42) // 85) for v in pixels]   # 0, 85, 170, 255
    for i in range(0, len(q), 4):
        out.append(q[i] << 6 | q[i + 1] << 4 | q[i + 2] << 2 | q[i + 3])
    return out

def wrapNumbers(values, per=32): return [','.join(str(v) for v in values[i:i + per]) + ',' for i in range(0, len(values), per)]

def generate():
    assigned = plan()
    narrow, wide = [], []
    data, wideData = [], bytearray()
    for code in sorted(assigned):
        name, exact = assigned[code]
        if isWide(code):
            img = renderExact(name, code, WIDE) if exact else renderFit(name, code, WIDE, 24)
            wide.append(code); wideData += pack2(list(img.tobytes()))
        else:
            img = renderExact(name, code, NARROW) if exact and fitsExact(name, code, NARROW) else renderFit(name, code, NARROW, 13)
            narrow.append((code, rle(list(img.tobytes()), data)))
    # Wide ranges (including code points the fonts lack, which draw a two-column replacement box).
    ranges, start = [], None
    for c in range(0x1100, 0x110000):
        w = isWide(c)
        if w and start is None: start = c
        if not w and start is not None: ranges.append((start, c - 1)); start = None
    if start is not None: ranges.append((start, 0x10ffff))

    lines = header('narrow glyphs') + ['struct FontGlyph {uint32_t code,offset;};', 'static constexpr FontGlyph fontGlyphs[]={']
    lines += [f'{{{c},{o}}},' for c, o in narrow] + ['};', 'static constexpr uint8_t fontAlpha[]={'] + wrapNumbers(data) + ['};']
    (outDir / 'fontdata.inc').write_text('\n'.join(lines) + '\n')
    lines = header('wide glyphs: 24x24, 2 bits per pixel, 144 bytes each, indexed like fontWideCodes') + [
        'static constexpr uint32_t fontWideCodes[]={'] + wrapNumbers(wide) + ['};', 'static constexpr uint8_t fontWideAlpha[]={'] + wrapNumbers(list(wideData)) + ['};']
    (outDir / 'fontwide.inc').write_text('\n'.join(lines) + '\n')
    lines = header('double-width ranges') + ['static constexpr uint32_t fontWideRanges[][2]={'] + [f'{{{a},{b}}},' for a, b in ranges] + ['};']
    (outDir / 'fontwidth.inc').write_text('\n'.join(lines) + '\n')
    print(f'{len(narrow)} narrow glyphs, {len(data)} RLE bytes; {len(wide)} wide glyphs, {len(wideData)} bytes; {len(ranges)} wide ranges')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--subset-cjk', metavar='TTC', help='rebuild the committed CJK subset from a Noto Sans Mono CJK .ttc')
    args = parser.parse_args()
    subsetCjk(args.subset_cjk) if args.subset_cjk else generate()
