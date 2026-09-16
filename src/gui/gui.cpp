#include "gui/gui.h"

#include "tdf/tdf.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace ta::gui {

namespace {

bool ieq(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

const Gadget* Gui::find(const std::string& name) const {
    for (const auto& g : gadgets)
        if (ieq(g.name, name)) return &g;
    return nullptr;
}

Gui parse(const std::vector<uint8_t>& bytes, const std::string& origin) {
    tdf::Node root = tdf::parseText(std::string(bytes.begin(), bytes.end()), origin);

    Gui out;
    // Gadgets are numbered, and the numbering is the z/declaration order -- so walk
    // the indices rather than root.childOrder, which would be fine here but would
    // put GADGET10 next to GADGET1 if the file ever declared them out of sequence.
    for (int i = 0;; ++i) {
        const tdf::Node* gn = root.child("gadget" + std::to_string(i));
        if (!gn) break;
        const tdf::Node* cn = gn->child("common");
        if (!cn) continue;   // a gadget with no COMMON block has no geometry to use

        Gadget g;
        g.type = int(cn->numberOr("id", 0));
        g.assoc = int(cn->numberOr("assoc", 0));
        g.name = cn->valueOr("name", "");
        g.x = int(cn->numberOr("xpos", 0));
        g.y = int(cn->numberOr("ypos", 0));
        g.w = int(cn->numberOr("width", 0));
        g.h = int(cn->numberOr("height", 0));
        g.attribs = int(cn->numberOr("attribs", 0));
        g.commonAttribs = int(cn->numberOr("commonattribs", 0));
        g.colorF = int(cn->numberOr("colorf", 0));
        g.colorB = int(cn->numberOr("colorb", 0));
        g.textureNumber = int(cn->numberOr("texturenumber", 0));
        g.fontNumber = int(cn->numberOr("fontnumber", 0));
        g.active = cn->numberOr("active", 1) != 0;

        // Type-specific keys sit beside COMMON, not inside it.
        g.quickKey = int(gn->numberOr("quickkey", 0));
        g.grayedOut = gn->numberOr("grayedout", 0) != 0;
        g.stages = int(gn->numberOr("stages", 0));
        g.status = int(gn->numberOr("status", 0));
        g.text = gn->valueOr("text", "");
        g.help = gn->valueOr("help", "");
        g.filename = gn->valueOr("filename", "");
        g.range = int(gn->numberOr("range", 0));
        g.thick = int(gn->numberOr("thick", 0));
        g.knobPos = int(gn->numberOr("knobpos", 0));
        g.knobSize = int(gn->numberOr("knobsize", 0));
        g.maxChars = int(gn->numberOr("maxchars", 0));
        g.itemHeight = int(gn->numberOr("itemheight", 0));

        if (i == 0) {
            out.panel = gn->valueOr("panel", "");
            out.totalGadgets = int(gn->numberOr("totalgadgets", 0));
        }
        out.gadgets.push_back(std::move(g));
    }

    // A file with no GADGET0 is not a TA .gui -- most likely a Kingdoms-era binary
    // token stream, which would otherwise parse to an empty Gui and be reported as
    // a screen with no controls rather than as the wrong format.
    if (out.gadgets.empty())
        throw std::runtime_error(origin + ": no [GADGET0] -- not a TA .gui");
    return out;
}

}  // namespace ta::gui
