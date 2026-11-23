#pragma once

#include "cartographer/document.h"
#include <deque>

namespace cart {
// Serialize a document for history, then retain only changed byte runs. A full
// current snapshot is kept once, not once per command or mouse-motion event.
struct HistoryState {
    std::vector<uint8_t> terrain;
    std::string metadata;
    std::vector<uint8_t> scenario;
    std::set<std::string> useOnly;
    std::string name;
    int seaLevel=0;
    bool stockTerrain=false;
    std::string editorMetadata;
    AiFiles aiFiles;   // small; kept whole per entry like the .ota text
    bool operator==(const HistoryState&) const = default;
};
HistoryState historyState(const tak::tnt::Map&,const tak::tnt::Scenario&,
                          const tak::crt::Scenario&,const std::vector<PlacedUnit>&,
                          const std::set<std::string>&,const AiFiles&,const std::string&);

class History {
public:
    explicit History(size_t budget=64*1024*1024):budget_(budget) {}
    void reset(HistoryState state,bool saved);
    bool commit(HistoryState state);
    const HistoryState* undo();
    const HistoryState* redo();
    void markSaved() {saved_=revision_;}
    bool dirty() const {return revision_!=saved_;}
    bool canUndo() const {return position_>0;}
    bool canRedo() const {return position_<entries_.size();}
    size_t retainedBytes() const {return bytes_;}
    uint64_t revision() const {return revision_;}
private:
    struct Run {size_t offset;std::vector<uint8_t> before,after;};
    struct Patch {
        size_t beforeSize=0,afterSize=0;
        std::vector<Run> runs;
        size_t cost=0;
        static Patch make(const std::vector<uint8_t>&,const std::vector<uint8_t>&);
        void apply(std::vector<uint8_t>&,bool forward) const;
    };
    struct Entry {
        Patch terrain,scenario;
        HistoryState before,after; // metadata only; large byte arrays live in patches
        uint64_t beforeRevision=0,afterRevision=0;
        size_t cost=0;
    };
    HistoryState current_;
    std::deque<Entry> entries_;
    size_t position_=0,bytes_=0,budget_;
    uint64_t serial_=0,revision_=0,saved_=0;
};
} // namespace cart
