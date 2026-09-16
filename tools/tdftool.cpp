// tdftool — parse and query TDF-family files (.tdf/.fbi/.ota/.gui).
//
//   tdftool dump <file>                  re-emit parsed structure
//   tdftool get <file> <section> <key>   print one value
//   tdftool check <file>...              parse each, report failures

#include "tdf/tdf.h"

#include <iostream>

using ta::tdf::Node;

static void dump(const Node& n, int depth) {
    std::string ind(size_t(depth) * 2, ' ');
    for (const auto& [k, v] : n.values) std::cout << ind << k << " = " << v << "\n";
    for (const auto& name : n.childOrder) {
        std::cout << ind << "[" << name << "]\n";
        dump(n.children.at(name), depth + 1);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: tdftool dump <file>\n"
                     "       tdftool get <file> <section> <key>\n"
                     "       tdftool check <file>...\n";
        return 2;
    }
    std::string cmd = argv[1];

    if (cmd == "check") {
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            try {
                ta::tdf::parse(argv[i]);
            } catch (const std::exception& e) {
                std::cerr << e.what() << "\n";
                ++failures;
            }
        }
        std::cout << (argc - 2) << " files, " << failures << " failures\n";
        return failures ? 1 : 0;
    }

    try {
        auto root = ta::tdf::parse(argv[2]);
        if (cmd == "dump") {
            dump(root, 0);
        } else if (cmd == "get" && argc >= 5) {
            const auto* sec = root.child(argv[3]);
            if (!sec) { std::cerr << "no section " << argv[3] << "\n"; return 1; }
            const auto* v = sec->value(argv[4]);
            if (!v) { std::cerr << "no key " << argv[4] << "\n"; return 1; }
            std::cout << *v << "\n";
        } else {
            std::cerr << "unknown command\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
