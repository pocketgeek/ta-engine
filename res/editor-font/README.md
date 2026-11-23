# Editor font sources

Every file here is an open font under SIL OFL 1.1 (see `EDITOR-FONT-LICENSE.txt` at
the repository root for notices and the license text); none is a retail game asset.

| File | Provides | Notes |
| --- | --- | --- |
| `NotoSansMono-Regular.ttf` | Latin, Latin Extended (incl. Vietnamese), Greek, Cyrillic, box drawing, many symbols | 2.014, primary face, exact metrics (glyphs whose ink would leave the cell are fitted instead) |
| `NotoSansArmenian-VF.ttf` | Armenian | 2.008 variable font (default 400 instance), fitted into one column |
| `NotoSansGeorgian-VF.ttf` | Georgian (Mkhedruli, Mtavruli, Asomtavruli) | 2.005, fitted into one column |
| `NotoSansSymbols-VF.ttf`, `NotoSansSymbols2-Regular.ttf` | arrows, math, technical, shapes, dingbats, Braille, currency | 2.003 / 2.008, fill gaps in Noto Sans Mono |
| `NotoSansMonoCJK-subset.otf` | Hiragana, Katakana, Bopomofo, Hangul (KS X 1001), ~7,200 Han, CJK punctuation, fullwidth and halfwidth forms | subset of Noto Sans Mono CJK 2.004 (SC face, regular weight) |

`tools/build-editor-font.py` rasterizes the mapped code points into the embedded alpha
masks (`src/cartographer/fontdata.inc` narrow glyphs, `fontwide.inc` double-width glyphs,
`fontwidth.inc` the double-width ranges) and records every source file's SHA256 in
their headers. Regeneration needs Python, Pillow (with raqm, so the committed bytes
reproduce) and fontTools; ordinary builds and deployed binaries need none of them.
`--subset-cjk <NotoSansMonoCJK-VF.ttc>` rebuilds the CJK subset from the upstream
collection (the Han set is the union of the GB 2312 level 1, JIS X 0208 level 1 and
Big5 level 1 repertoires).

Layout is fixed-width: a narrow glyph is one text column (6 UI px per scale), and code
points with East Asian Width Wide/Fullwidth (CJK, Hangul, fullwidth forms, emoji-style
symbols) are two columns. Width is a function of the code point alone, so caret,
selection, hit-testing and wrapping do not depend on which glyphs exist. A code point
with no glyph draws an outlined replacement box of the right width; invalid UTF-8
draws U+FFFD.

Not covered: right-to-left scripts (Hebrew, Arabic; the editor lays text out
left-to-right), scripts needing shaping or combining-mark placement (Indic, Thai,
Khmer...), combining marks (each takes its own column), color emoji, and rarer Han
characters outside the three legacy level-1 sets. Those show the replacement box.
