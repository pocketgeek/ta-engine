#pragma once
// Debug-only JSONL roster of a referee game, for the retail AI comparison
// (tools/re/compare_ai.py; the retail side is tools/re/retail_ai_poll.py, and
// both write the same schema). Off unless TAK_ROSTER_DUMP names an output file;
// TAK_ROSTER_EVERY sets the cadence in ticks (default 30 = once a second). The
// first room that ticks writes to that path, any later room to "<path>.room<id>".
// A release (NDEBUG) takserver compiles none of it and reads neither variable.
//
// It only reads the World after the tick and writes a file: no sim state, no
// command, no hash input changes, so --mpai hashes are the same with it on.
#include <cstdint>
#include <vector>
namespace tak::sim {class World;}
namespace tak::ai {class Controller;}
namespace tak::srv {
#ifndef NDEBUG
void rosterDump(uint32_t roomId,const sim::World& world,const std::vector<ai::Controller>& controllers);
#endif
}
