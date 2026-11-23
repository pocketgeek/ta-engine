#pragma once

// REMEMBER KEY for the YouTube streaming panel (client/streaming.cpp): an opt-in copy of
// the stream key in the OS credential store (client/credstore.h), loaded masked into the
// key field at the next launch. Only the on/off preference is a setting
// (Settings::streamRememberKey); the key never goes into settings or any other file.
//
// While the box is ticked the stored key follows the key field. It is written when the
// box is ticked, when a key is pasted, when streaming starts, and when the panel closes
// (closing the application with it open counts). A field that is empty at any of those
// moments -- Ctrl+A clears it at once -- deletes the stored key instead of leaving a
// stale one. Unticking deletes it; if the store refuses, the box ticks itself again
// (remember() turns true) so it never claims a key is gone that is still stored. Typed
// characters are not written one by one.
//
// Every store call runs on one worker thread, in order, so a slow Secret Service or an
// unlock prompt never stalls the render loop. The panel calls poll() each frame to pick
// up results. Destruction waits briefly for queued writes and deletes, so an untick just
// before quitting still takes effect, then abandons a call stuck on a prompt.

#include "client/credstore.h"

#include <cstdint>
#include <memory>
#include <string>

namespace tak {

class StreamKeyMemory {
public:
    enum class State { Unknown, Checking, Available, Unavailable };
    StreamKeyMemory(std::shared_ptr<credstore::Store> store, bool remember);
    ~StreamKeyMemory();
    StreamKeyMemory(const StreamKeyMemory&) = delete;
    StreamKeyMemory& operator=(const StreamKeyMemory&) = delete;

    // Probe the store, and load the remembered key once if the box is ticked. Called at
    // startup when ticked and whenever the panel opens; re-probes only while unavailable.
    void check();
    // Apply finished work. A loaded key fills `key` only if the field is still empty. May
    // turn remember() back on (a refused untick); the caller re-saves the preference.
    void poll(std::string& key);
    // Tick stores `key` (an empty field deletes any stale entry); untick deletes. Returns
    // false, changing nothing, while the store is not available.
    bool setRemember(bool on, const std::string& key);
    // While ticked: make the stored key match `key`, if it changed since the last write.
    void sync(const std::string& key);

    bool remember() const { return remember_; }
    State state() const { return state_; }
    const std::string& reason() const { return reason_; }   // why Unavailable
    std::string storeName() const;
    bool busy() const;                // queued or running store work, or unpolled results
    std::string takeError();          // the latest failure as UI text ("" = none)
    bool waitIdle(int ms);            // tests and screenshots: wait for the worker (poll() applies)

private:
    struct Shared;
    struct Op;
    void enqueue(Op op);
    std::shared_ptr<Shared> sh_;
    bool remember_ = false, loaded_ = false, syncedValid_ = false;
    uint64_t syncedHash_ = 0, generation_ = 0;
    State state_ = State::Unknown;
    std::string reason_, error_, name_;
};

// The store the panel uses: the system store, or in a debug build TAK_CREDSTORE=none
// (no store, for the greyed-out state), =memory (process memory only) or =test (the
// system store under credstore::testSlot()).
std::shared_ptr<credstore::Store> streamKeyStore();

}  // namespace tak
