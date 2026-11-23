#pragma once

// What operator new really hands out, on this thread, while one of these is alive.
// World::clone is a defaulted copy of hundreds of members, so the size of a replay
// keyframe cannot be worked out, and a guess was five times short on a Legion game.
// alloccount.cpp replaces the global operator new and delete to count it. Not nestable.

#include <cstdint>

namespace tak::client {

class AllocCount {
public:
    AllocCount();
    ~AllocCount();
    AllocCount(const AllocCount&) = delete;
    AllocCount& operator=(const AllocCount&) = delete;
    uint64_t bytes() const { return bytes_; }

private:
    uint64_t bytes_ = 0;
};

}  // namespace tak::client
