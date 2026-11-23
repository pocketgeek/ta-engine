#include "tdf/tdf.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tak::tdf {

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
    ParseLimits limits;
    ParseUsage local{};
    ParseUsage& usage() {return limits.usage?*limits.usage:local;}
    [[noreturn]] void limit(const char* reason) const {throw ParseLimitError(origin+": "+reason);}
    void charge(size_t bytes) {
        auto& memory=usage().memory;
        if(memory>limits.memory || bytes>limits.memory-memory)limit("parsed memory limit exceeded");
        memory+=bytes;
    }
    void section() {
        if(++usage().sections>limits.sections || ++usage().nodes>limits.nodes)limit("section/node limit exceeded");
        charge(sizeof(Node)*3+256);
    }

    void addChild(Node& node,std::map<std::string,size_t>& latest,const std::string& key,Node child) {
        auto previous=node.children.find(key);
        if(previous==node.children.end())node.childOrder.push_back(key);
        else {
            node.childEntries.at(latest.at(key)).previous=int(node.previousChildren.size());
            node.previousChildren.push_back(std::move(previous->second));
        }
        latest[key]=node.childEntries.size();
        node.childEntries.push_back({key,-1});
        node.children[key]=std::move(child);
    }

    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error(origin + ":" + std::to_string(line) + ": " + msg);
    }

    int peek() { return pos < text.size() ? uint8_t(text[pos]) : -1; }

    void advance() {
        if((pos&4095)==0 && limits.stop.stop_requested())limit("parsing cancelled");
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
        std::map<std::string,size_t> latest;
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
                addChild(node,latest,key,std::move(child));
            } else if (c == ';') {
                advance();  // stray semicolon
            } else {
                parseAssignment(node);
            }
        }
    }

    std::string parseSectionName() {
        section();
        advance();  // '['
        std::string name;
        while (pos < text.size() && text[pos] != ']') {
            charge(8);
            name += text[pos];
            advance();
        }
        if (pos >= text.size()) fail("unterminated section name");
        advance();  // ']'
        return trim(name);
    }

    void parseAssignment(Node& node) {
        if(++usage().nodes>limits.nodes)limit("node limit exceeded");
        charge(256);
        std::string key;
        while (pos < text.size() && text[pos] != '=' && text[pos] != '\n') {
            charge(4);
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
            charge(4);
            value += text[pos];
            advance();
        }
        value = trim(value);
        while (!value.empty() && value.back() == ';') value.pop_back();
        node.values[lower(trim(key))] = trim(value);
    }

    Node parseTop() {
        Node root;
        std::map<std::string,size_t> latest;
        while (true) {
            skipWs();
            int c = peek();
            if (c == -1) return root;
            if (c != '[') fail("expected section");
            std::string name = parseSectionName();
            std::string key = lower(name);
            Node child;
            parseBody(child, 0);
            addChild(root,latest,key,std::move(child));
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

std::vector<std::pair<std::string_view,const Node*>> Node::orderedChildren() const {
    std::vector<std::pair<std::string_view,const Node*>> result;
    result.reserve(childEntries.size());
    for(const auto& entry:childEntries)
        result.emplace_back(entry.name,entry.previous<0 ? &children.at(entry.name) :
            &previousChildren.at(size_t(entry.previous)));
    return result;
}

Node parseText(const std::string& text, const std::string& originName, ParseLimits limits) {
    Parser p{text, originName, 0, 1, limits};
    return p.parseTop();
}

Node parse(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return parseText(ss.str(), file.string());
}

} // namespace tak::tdf
