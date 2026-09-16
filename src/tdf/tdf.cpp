#include "tdf/tdf.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ta::tdf {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

struct Parser {
    const std::string& text;
    const std::string& origin;
    size_t pos = 0;
    int line = 1;

    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error(origin + ":" + std::to_string(line) + ": " + msg);
    }

    int peek() { return pos < text.size() ? uint8_t(text[pos]) : -1; }

    void advance() {
        if (text[pos] == '\n') ++line;
        ++pos;
    }

    void skipWs() {
        while (pos < text.size()) {
            char c = text[pos];
            if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
                while (pos < text.size() && text[pos] != '\n') advance();
            } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
                advance(); advance();
                while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/'))
                    advance();
                if (pos + 1 >= text.size()) fail("unterminated comment");
                advance(); advance();
            } else if (std::isspace(uint8_t(c))) {
                advance();
            } else {
                return;
            }
        }
    }

    // Parse the body between { and } into `node`.
    void parseBody(Node& node, int depth) {
        if (depth > 32) fail("sections nested too deep");
        skipWs();
        if (peek() != '{') fail("expected '{'");
        advance();
        while (true) {
            skipWs();
            int c = peek();
            if (c == -1) fail("unexpected end of file in section");
            if (c == '}') { advance(); return; }
            if (c == '[') {
                std::string name = parseSectionName();
                std::string key = lower(name);
                skipWs();
                Node child;
                parseBody(child, depth + 1);
                if (!node.children.count(key)) node.childOrder.push_back(key);
                node.children[key] = std::move(child);
            } else if (c == ';') {
                advance();  // stray semicolon
            } else {
                parseAssignment(node);
            }
        }
    }

    std::string parseSectionName() {
        advance();  // '['
        std::string name;
        while (pos < text.size() && text[pos] != ']') {
            name += text[pos];
            advance();
        }
        if (pos >= text.size()) fail("unterminated section name");
        advance();  // ']'
        return trim(name);
    }

    void parseAssignment(Node& node) {
        std::string key;
        while (pos < text.size() && text[pos] != '=' && text[pos] != '\n') {
            key += text[pos];
            advance();
        }
        if (peek() != '=') fail("expected '=' in assignment");
        advance();
        // Values run to end of line: game data contains values holding ';'
        // (French sentences) and even '}' (the SYMBOL_7D key name table), so
        // ';' and '}' only terminate at line granularity. A '}' after other
        // content on the line closes the section and is pushed back.
        std::string value;
        while (pos < text.size() && text[pos] != '\n') {
            if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '/') break;
            if (text[pos] == '}' && !trim(value).empty()) break;
            value += text[pos];
            advance();
        }
        value = trim(value);
        while (!value.empty() && value.back() == ';') value.pop_back();
        node.values[lower(trim(key))] = trim(value);
    }

    Node parseTop() {
        Node root;
        while (true) {
            skipWs();
            int c = peek();
            if (c == -1) return root;
            if (c != '[') fail("expected section");
            std::string name = parseSectionName();
            std::string key = lower(name);
            Node child;
            parseBody(child, 0);
            if (!root.children.count(key)) root.childOrder.push_back(key);
            root.children[key] = std::move(child);
        }
    }
};

} // namespace

const std::string* Node::value(const std::string& key) const {
    auto it = values.find(lower(key));
    return it == values.end() ? nullptr : &it->second;
}

std::string Node::valueOr(const std::string& key, const std::string& def) const {
    const auto* v = value(key);
    return v ? *v : def;
}

double Node::numberOr(const std::string& key, double def) const {
    const auto* v = value(key);
    if (!v) return def;
    char* end = nullptr;
    double x = std::strtod(v->c_str(), &end);
    return end == v->c_str() ? def : x;
}

const Node* Node::child(const std::string& name) const {
    auto it = children.find(lower(name));
    return it == children.end() ? nullptr : &it->second;
}

Node parseText(const std::string& text, const std::string& originName) {
    Parser p{text, originName};
    return p.parseTop();
}

Node parse(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return parseText(ss.str(), file.string());
}

} // namespace ta::tdf
