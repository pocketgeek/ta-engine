#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tak::render {

// GameView's unit id -> model (visual) name registry, with a compact record of its
// key set (workstream L). The painter pass asks "is this unit registered?" for every
// candidate -- with 10,000 units in view a hash lookup each was half of the pass --
// so a dense flag per non-negative id answers it; negative ids (test fixtures) fall
// back to the map. The flags change only through set()/erase(), which also change
// the map, so registered(id) == (count(id) != 0) always. Reads keep the map's API.
// A registered unit's visual name can change (corpse and veteran meshes swap it in
// place), so nothing may cache a resolved visual by unit identity alone. Like the
// map, the table is never pruned (one byte per id ever registered, beside the map's
// string per id).
class UnitTypeTable {
public:
    using Map = std::unordered_map<int, std::string>;
    Map::const_iterator find(int id) const { return map_.find(id); }
    Map::const_iterator end() const { return map_.end(); }
    size_t count(int id) const { return map_.count(id); }
    const std::string& at(int id) const { return map_.at(id); }
    bool registered(int id) const {
        if (id < 0) return map_.count(id) != 0;
        return size_t(id) < flags_.size() && flags_[size_t(id)] != 0;
    }
    void set(int id, const std::string& visual) {
        map_[id] = visual;
        if (id < 0) return;
        if (size_t(id) >= flags_.size()) flags_.resize(size_t(id) + 1, 0);
        flags_[size_t(id)] = 1;
    }
    void erase(int id) {
        map_.erase(id);
        if (id >= 0 && size_t(id) < flags_.size()) flags_[size_t(id)] = 0;
    }
    void clear() { map_.clear(); flags_.clear(); }
    size_t flagBytes() const { return flags_.capacity(); }
private:
    Map map_;
    std::vector<uint8_t> flags_;
};

}  // namespace tak::render
