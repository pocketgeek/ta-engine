#include "gui/gui.h"

#include <cctype>
#include <stdexcept>

namespace tak::gui {

namespace {

// Token reader over the raw byte stream. Two token kinds: signed decimal ints, and
// length-prefixed strings ("<int L> <one ws> <L bytes>", extended to the next
// whitespace to repair one mis-authored shipped file that under-counts a length).
struct Reader {
    const uint8_t* d;
    size_t n, i = 0;
    const std::string& origin;

    Reader(const std::vector<uint8_t>& bytes, const std::string& o)
        : d(bytes.data()), n(bytes.size()), origin(o) {}

    static bool ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    void skipws() { while (i < n && ws(d[i])) ++i; }
    bool eof() { skipws(); return i >= n; }

    int rint() {
        skipws();
        size_t s = i;
        while (i < n && !ws(d[i])) ++i;
        if (i == s) throw std::runtime_error("gui: int expected @" + origin);
        return std::stoi(std::string(reinterpret_cast<const char*>(d + s), i - s));
    }
    std::string rstr() {
        int L = rint();
        if (L <= 0) return {};
        if (i < n && ws(d[i])) ++i;
        size_t j = i + size_t(L);
        if (j > n) j = n;
        while (j < n && !ws(d[j])) ++j;   // extend past an under-counted length
        std::string v(reinterpret_cast<const char*>(d + i), j - i);
        i = j;
        return v;
    }
    // True if the next tokens are the BASE-GADGET signature `2 <8 ints> 3`.
    bool commonAhead() {
        size_t save = i;
        bool ok = false;
        try {
            if (rint() == 2) {
                for (int k = 0; k < 8; ++k) rint();
                ok = (rint() == 3);
            }
        } catch (...) { ok = false; }
        i = save;
        return ok;
    }
};

// Consume the type-specific STYLE-PREFIX between "<type> <ver>" and the BASE-GADGET.
void readPrefix(Reader& r, int gtype) {
    if (gtype == 1 || gtype == 2) return;         // Dialog root / Panel: no prefix
    if (gtype == 21) {                            // EditBox: has an embedded string
        r.rint(); r.rint();
        r.rstr();
        for (int k = 0; k < 10; ++k) r.rint();
        for (int k = 0; k < 5; ++k) r.rint();     // colour 1
        for (int k = 0; k < 5; ++k) r.rint();     // colour 2
        r.rint();                                 // marker 1
        return;
    }
    // Generic: numeric prefix -- consume ints until the COMMON block begins.
    int guard = 0;
    while (!r.commonAhead()) {
        r.rint();
        if (++guard > 64) throw std::runtime_error("gui: prefix runaway @" + r.origin);
    }
}

Gadget parseGadget(Reader& r) {
    Gadget g;
    g.type = r.rint();
    g.ver = r.rint();
    readPrefix(r, g.type);
    if (r.rint() != 2) throw std::runtime_error("gui: COMMON tag @" + r.origin);
    g.x = r.rint(); g.y = r.rint(); g.w = r.rint(); g.h = r.rint();
    for (int k = 0; k < 4; ++k) r.rint();          // flags
    if (r.rint() != 3) throw std::runtime_error("gui: colour tag @" + r.origin);
    g.ca = uint8_t(r.rint()); g.cr = uint8_t(r.rint());
    g.cg = uint8_t(r.rint()); g.cb = uint8_t(r.rint());
    if (r.rint() != 1) throw std::runtime_error("gui: cursor tag @" + r.origin);
    r.rstr(); r.rstr();                            // cursor gaf/seq
    if (r.rint() != 1) throw std::runtime_error("gui: font tag @" + r.origin);
    r.rstr();                                      // font
    g.name = r.rstr();
    r.rint();                                      // p
    r.rint();                                      // q (== image count)
    int ni = r.rint();
    for (int k = 0; k < ni; ++k) {
        ImgRef im;
        r.rint();                                  // tag (1)
        im.gaf = r.rstr();
        im.seq = r.rstr();
        im.frame = r.rint();
        im.flags = r.rint();
        g.imgs.push_back(std::move(im));
    }
    int nt = r.rint();
    for (int k = 0; k < nt; ++k) {
        r.rint();                                  // tag (2)
        r.rint();                                  // align
        g.texts.push_back(r.rstr());
    }
    int ns = r.rint();
    for (int k = 0; k < ns; ++k) {
        r.rint();                                  // tag (1)
        g.states.push_back(r.rstr());
    }
    r.rint();                                      // cmd tag (2)
    r.rint();                                      // cmd align (0)
    g.cmd = r.rstr();
    r.rint();                                      // trailing id
    return g;
}

}  // namespace

const Gadget* Gui::find(const std::string& name) const {
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t k = 0; k < a.size(); ++k)
            if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]))
                return false;
        return true;
    };
    for (const auto& g : gadgets)
        if (ieq(g.name, name)) return &g;
    return nullptr;
}

Gui parse(const std::vector<uint8_t>& bytes, const std::string& origin) {
    Reader r(bytes, origin);
    Gui gui;
    while (!r.eof()) gui.gadgets.push_back(parseGadget(r));
    return gui;
}

}  // namespace tak::gui
