#include "gui/tagui.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace ta::tagui {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// GetInt (atol). The GUI editor writes -51 / 52685 (0xCDCD) as uninitialised
// filler; those read as 0 here, as they never mean anything.
int geti(const tdf::Node& n, const char* key, int def = 0) {
    const std::string* v = n.value(key);
    if (!v) return def;
    long x = std::strtol(v->c_str(), nullptr, 10);
    if (x == -51 || x == 52685) return def;
    return int(x);
}
std::string gets(const tdf::Node& n, const char* key, size_t max) {
    std::string v = n.valueOr(key, "");
    if (v.size() >= max) v.resize(max - 1);
    return v;
}
std::string base(const std::string& name) {
    std::string b = name;
    if (auto s = b.find_last_of("/\\"); s != std::string::npos) b = b.substr(s + 1);
    if (auto d = b.rfind('.'); d != std::string::npos) b = b.substr(0, d);
    return b;
}

}  // namespace

std::vector<std::string> Gadget::stageLabels() const {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '|') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

const Gadget* Screen::find(const std::string& n) const {
    const std::string u = upper(n);
    for (const auto& g : gadgets)
        if (upper(g.name) == u) return &g;
    return nullptr;
}
Gadget* Screen::find(const std::string& n) {
    return const_cast<Gadget*>(static_cast<const Screen*>(this)->find(n));
}

std::string Screen::enterDefault() const {
    if (!gadgets.empty() && !gadgets[0].crDefault.empty()) return gadgets[0].crDefault;
    for (const auto& g : gadgets) {
        if (g.type != Type::Button) continue;
        const std::string u = upper(g.name);
        if (u.rfind("OK", 0) == 0 || u.rfind("NEXT", 0) == 0) return g.name;
    }
    return {};
}

Screen parse(const std::string& text, const std::string& baseName) {
    const tdf::Node root = tdf::parseText(text, baseName);
    Screen s;
    s.name = upper(base(baseName));
    for (int i = 0;; ++i) {
        const tdf::Node* gn = root.child("GADGET" + std::to_string(i));
        if (!gn) break;
        Gadget g;
        g.index = i;
        const tdf::Node* c = gn->child("COMMON");
        if (c) {
            g.type = Type(uint8_t(geti(*c, "id", 9)));
            g.assoc = geti(*c, "assoc");
            g.name = gets(*c, "name", 17);
            g.x = int16_t(geti(*c, "xpos"));
            g.y = int16_t(geti(*c, "ypos"));
            g.w = int16_t(geti(*c, "width"));
            g.h = int16_t(geti(*c, "height"));
            g.attribs = uint32_t(geti(*c, "attribs"));
            g.colorf = uint32_t(geti(*c, "colorf"));
            g.colorb = uint32_t(geti(*c, "colorb"));
            g.textureNumber = geti(*c, "texturenumber");
            g.fontNumber = geti(*c, "fontnumber");
            g.active = geti(*c, "active", 1) != 0;
            g.commonAttribs = geti(*c, "commonattribs");
            g.help = gets(*c, "help", 128);
        }
        switch (g.type) {
        case Type::Container:
            g.totalGadgets = geti(*gn, "totalgadgets");
            g.panel = gets(*gn, "panel", 32);
            g.crDefault = gets(*gn, "crdefault", 17);
            g.escDefault = gets(*gn, "escdefault", 17);
            g.defaultFocus = gets(*gn, "defaultfocus", 17);
            break;
        case Type::Button: {
            g.status = geti(*gn, "status");
            g.text = gets(*gn, "text", 128);
            g.grayedOut = (geti(*gn, "grayedout") & 1) != 0;
            g.stages = geti(*gn, "stages");
            const std::string q = gn->valueOr("quickkey", "");
            if (q.size() == 1 && std::isalpha(static_cast<unsigned char>(q[0])))
                g.quickKey = std::toupper(static_cast<unsigned char>(q[0]));
            else if (!q.empty()) g.quickKey = std::atoi(q.c_str());
            if (g.quickKey == -51 || g.quickKey == 52685) g.quickKey = 0;
            break;
        }
        case Type::ListBox: g.itemHeight = geti(*gn, "itemheight"); break;
        case Type::TextField:
            g.maxChars = std::min(geti(*gn, "maxchars"), 128);
            g.text = gets(*gn, "text", 128);
            break;
        case Type::ScrollBar:
            g.range = geti(*gn, "range");
            g.thick = geti(*gn, "thick");
            g.knobPos = geti(*gn, "knobpos");
            g.knobSize = geti(*gn, "knobsize");
            break;
        case Type::Label:
            g.text = gets(*gn, "text", 128);
            g.link = gets(*gn, "link", 17);
            break;
        case Type::Surface: g.hotOrNot = (geti(*gn, "hotornot") & 1) != 0; break;
        case Type::Font:
        case Type::Picture: g.filename = gets(*gn, "filename", 33); break;
        default: break;
        }
        s.gadgets.push_back(std::move(g));
    }
    if (s.gadgets.empty()) throw std::runtime_error(baseName + ": no GADGET0");
    s.gadgets[0].name = s.name;   // 0x4aac98: the container takes the file's name
    return s;
}

static std::string guiPath(const std::string& name) {
    return "guis/" + lowerStr(base(name)) + ".gui";
}

bool exists(const hpi::Vfs& vfs, const std::string& name) { return vfs.has(guiPath(name)); }

Screen load(const hpi::Vfs& vfs, const std::string& name) {
    const std::string path = guiPath(name);
    auto bytes = vfs.read(path);
    return parse(std::string(bytes.begin(), bytes.end()), base(name));
}

}  // namespace ta::tagui
