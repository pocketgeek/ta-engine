#pragma once

// One secret in the OS credential store: Windows Credential Manager, the macOS Keychain,
// or the freedesktop Secret Service on Linux (KWallet, GNOME Keyring, KeePassXC...). Used
// ONLY for the opt-in Remember Key of the YouTube stream key (client/streamkey.h).
//
// There is deliberately no file fallback. A platform without a store, a Linux session
// without libsecret or without a Secret Service on the session bus, reports a reason from
// probe() and the feature stays off -- the key is never written anywhere else.
//
// Every call may block: a Secret Service can be slow to start, and KWallet or the
// Keychain may put up an unlock prompt and wait for the user. Call these from a worker
// thread only (StreamKeyMemory does), never from the render loop. Error strings are
// fixed upper-case UI text and never contain the secret or a backend's own message.
// One platform file implements system(): credstore_{win,mac,linux,none}.cpp.

#include <memory>
#include <string>

namespace tak::credstore {

// Windows: TargetName "service/account" (CRED_TYPE_GENERIC). macOS: kSecClassGenericPassword
// with kSecAttrService/kSecAttrAccount. Secret Service: attributes service/account under
// the schema "org.takengine.Credential".
struct Slot { std::string service, account; };
inline Slot streamKeySlot() { return {"TAK Engine", "YouTube stream key"}; }
// Round-trip tests and debug screenshots use this; never the real slot.
inline Slot testSlot() { return {"TAK Engine Test", "credstore round-trip"}; }

enum class Result { Ok, Missing, Failed };

class Store {
public:
    virtual ~Store() = default;
    // The store's name for the panel ("KEYCHAIN"...).
    virtual std::string name() const = 0;
    // "" when usable, else the short reason the checkbox is greyed out. Must not prompt.
    virtual std::string probe() = 0;
    // Missing is not an error: nothing is stored. On Failed, `error` is set.
    virtual Result load(std::string& secret, std::string& error) = 0;
    virtual bool store(const std::string& secret, std::string& error) = 0;
    // Deleting a slot that holds nothing succeeds.
    virtual bool erase(std::string& error) = 0;
    // From another thread, at shutdown: abandon a call blocked on a prompt, if possible.
    virtual void cancel() {}
};

// The platform's store for `slot`; probe() says whether it can be used.
std::unique_ptr<Store> system(const Slot& slot);

inline constexpr const char* kNoStore = "NO SYSTEM PASSWORD STORE";

// Overwrite a buffer that held a secret before it is freed (the stores and the panel
// copy keys through temporaries). Volatile writes, so the stores are not elided.
inline void wipe(void* data, size_t n) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    while (n--) *p++ = 0;
}
inline void wipe(std::string& s) {
    s.resize(s.capacity());   // never reallocates; the spare capacity is wiped too
    wipe(s.data(), s.size());
    s.clear();
}

}  // namespace tak::credstore
