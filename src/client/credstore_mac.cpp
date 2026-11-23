// macOS credential store: the user's default (login) Keychain, one kSecClassGenericPassword
// item with kSecAttrService/kSecAttrAccount, visible in Keychain Access. The value is the
// key's UTF-8 bytes. A locked keychain, or a session that cannot show the unlock dialog
// (ssh), fails the call (errSecInteractionNotAllowed) instead of storing anywhere else.
// Security.framework and CoreFoundation provide the calls.

#include "client/credstore.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace tak::credstore {
namespace {

// Owns one CF reference.
template <class T> struct CF {
    T ref = nullptr;
    explicit CF(T r) : ref(r) {}
    ~CF() { if (ref) CFRelease(ref); }
    CF(const CF&) = delete;
    CF& operator=(const CF&) = delete;
};

CFStringRef cfString(const std::string& s) {
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()),
                                   kCFStringEncodingUTF8, false);
}

class Keychain final : public Store {
public:
    explicit Keychain(const Slot& slot) : service_(cfString(slot.service)), account_(cfString(slot.account)) {}
    std::string name() const override { return "KEYCHAIN"; }
    std::string probe() override { return service_.ref && account_.ref ? std::string() : kNoStore; }
    Result load(std::string& secret, std::string& error) override {
        CF<CFMutableDictionaryRef> q(query());
        CFDictionarySetValue(q.ref, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(q.ref, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q.ref, &out);
        CF<CFTypeRef> data(out);
        if (st == errSecItemNotFound) return Result::Missing;
        if (st != errSecSuccess || !out || CFGetTypeID(out) != CFDataGetTypeID()) {
            error = failure(st, "KEYCHAIN DID NOT RETURN THE KEY");
            return Result::Failed;
        }
        const auto d = static_cast<CFDataRef>(out);
        const auto* bytes = CFDataGetBytePtr(d);
        secret.assign(reinterpret_cast<const char*>(bytes), size_t(CFDataGetLength(d)));
        // The returned CFData is ours alone (retain count 1, released just below).
        wipe(const_cast<UInt8*>(bytes), size_t(CFDataGetLength(d)));
        return Result::Ok;
    }
    bool store(const std::string& secret, std::string& error) override {
        CF<CFDataRef> value(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()),
                                         CFIndex(secret.size())));
        CF<CFMutableDictionaryRef> q(query());
        CF<CFMutableDictionaryRef> change(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                    &kCFTypeDictionaryValueCallBacks));
        CFDictionarySetValue(change.ref, kSecValueData, value.ref);
        OSStatus st = SecItemUpdate(q.ref, change.ref);
        if (st == errSecItemNotFound) {
            CFDictionarySetValue(q.ref, kSecValueData, value.ref);
            CF<CFStringRef> label(cfString("TAK Engine: YouTube stream key"));
            CFDictionarySetValue(q.ref, kSecAttrLabel, label.ref);
            st = SecItemAdd(q.ref, nullptr);
        }
        // CFDataCreate copied the key; wipe that copy before it is released.
        if (value.ref) wipe(const_cast<UInt8*>(CFDataGetBytePtr(value.ref)), size_t(CFDataGetLength(value.ref)));
        if (st == errSecSuccess) return true;
        error = failure(st, "KEYCHAIN DID NOT SAVE THE KEY");
        return false;
    }
    bool erase(std::string& error) override {
        CF<CFMutableDictionaryRef> q(query());
        const OSStatus st = SecItemDelete(q.ref);
        if (st == errSecSuccess || st == errSecItemNotFound) return true;
        error = failure(st, "KEYCHAIN DID NOT DELETE THE KEY");
        return false;
    }
private:
    CFMutableDictionaryRef query() const {
        CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(q, kSecAttrService, service_.ref);
        CFDictionarySetValue(q, kSecAttrAccount, account_.ref);
        return q;
    }
    static std::string failure(OSStatus st, const char* otherwise) {
        if (st == errSecInteractionNotAllowed || st == errSecAuthFailed || st == errSecUserCanceled)
            return "KEYCHAIN IS LOCKED OR ACCESS WAS DENIED";
        return otherwise;
    }
    CF<CFStringRef> service_, account_;
};

}  // namespace

std::unique_ptr<Store> system(const Slot& slot) { return std::make_unique<Keychain>(slot); }

}  // namespace tak::credstore
