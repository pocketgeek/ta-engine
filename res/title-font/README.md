# Title lettering

Cinzel Bold 2.000 (SIL OFL 1.1; notice and licence text in `TITLE-FONT-LICENSE.txt` at
the repository root), unmodified, from https://github.com/NDISCOVER/Cinzel
(`fonts/ttf/Cinzel-Bold.ttf`). Classical Roman capitals, close to the retail logo's
lettering.

`cmake/EmbedFiles.cmake` writes it into `takclient` as a byte array (no runtime data
file), and `src/client/uifont.cpp` rasterizes it with stb_truetype (`src/vendor/stb`).
