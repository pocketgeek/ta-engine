// cobtool — inspect COB unit scripts.
//
//   cobtool info <file.cob>            script + piece lists
//   cobtool disasm <file.cob> <name>   disassemble one script

#include "cob/cob.h"

#include <iostream>

static int usage() {
    std::cerr << "usage: cobtool info <file.cob>\n"
                 "       cobtool disasm <file.cob> <script>\n";
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    if (!(cmd == "info" || (cmd == "disasm" && argc >= 4))) return usage();
    try {
        auto f = tak::cob::load(argv[2]);
        if (cmd == "info") {
            std::cout << f.scripts.size() << " scripts, " << f.pieces.size()
                      << " pieces, " << f.code.size() << " code words, "
                      << f.numStatics << " statics\n";
            std::cout << "scripts:";
            for (const auto& s : f.scripts) std::cout << " " << s.name;
            std::cout << "\npieces:";
            for (const auto& p : f.pieces) std::cout << " " << p;
            std::cout << "\n";
            if (!f.names.empty()) {   // PLAY_SOUND / mission MAP_COMMAND string table
                std::cout << "names (" << f.names.size() << "):\n";
                for (size_t i = 0; i < f.names.size(); ++i)
                    std::cout << "  [" << i << "] " << f.names[i] << "\n";
            }
        } else if (cmd == "disasm" && argc >= 4) {
            int idx = f.scriptIndex(argv[3]);
            if (idx < 0) { std::cerr << "no script " << argv[3] << "\n"; return 1; }
            std::cout << tak::cob::disassemble(f, idx);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
