#pragma once
// Debug-only JSONL roster of a referee game, for the retail AI comparison
// (tools/re/compare_ai.py; the retail side is tools/re/retail_ai_poll.py, and
// both write the same schema). Off unless TA_ROSTER_DUMP names an output file;
// TA_ROSTER_EVERY sets the cadence in ticks (default 30 = once a second). The
// first room that ticks writes to that path, any later room to "<path>.room<id>".
// A release (NDEBUG) taserver compiles none of it and reads neither variable.
//
// It only reads the World after the tick and writes a file: no sim state, no
// command, no hash input changes, so --mpai hashes are the same with it on.
#include <cstdint>
#include <vector>
namespace ta::sim {class World;}
namespace ta::ai {class Controller;}
namespace ta::srv {
#ifndef NDEBUG
void rosterDump(uint32_t roomId,const sim::World& world,const std::vector<ai::Controller>& controllers);
#endif
}
