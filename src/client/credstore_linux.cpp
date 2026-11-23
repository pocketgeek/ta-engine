// Linux credential store: the freedesktop Secret Service (KWallet's ksecretd/kwalletd,
// GNOME Keyring, KeePassXC...) through libsecret, loaded with dlopen at first use.
//
// Release builds are statically linked and must run where libsecret is absent, so nothing
// here links against it or includes its headers: the few entry points are declared by
// hand below. They are libsecret's simple password API and GLib's GDBus/GError/GObject
// calls, all ABI-stable since libsecret 0.18 / GLib 2.40 (2014). dlsym on the libsecret
// handle also finds the GLib/GIO/GObject symbols, since those are its dependencies.
// The handle is never closed: GLib does not support being unloaded.
//
// probe() asks the session bus whether org.freedesktop.secrets is owned or activatable.
// That never opens or unlocks a wallet, so it cannot prompt. load/store/erase may prompt
// (KWallet asking to open the wallet); StreamKeyMemory runs them on its worker thread,
// and cancel() abandons a blocked one at shutdown through a GCancellable.

#include "client/credstore.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace tak::credstore {
namespace {

using gboolean = int;
struct GError { uint32_t domain; int code; char* message; };
struct GCancellable;
struct GDBusConnection;
struct GVariant;
struct GVariantType;
struct SecretSchemaAttribute { const char* name; int type; };   // 0 = string
struct SecretSchema {
    const char* name;
    int flags;                                                  // 0 = match the name
    SecretSchemaAttribute attributes[32];
    int reserved;
    void* reserved1; void* reserved2; void* reserved3; void* reserved4;
    void* reserved5; void* reserved6; void* reserved7;
};
constexpr int kBusSession = 2;   // G_BUS_TYPE_SESSION
constexpr int kBusTimeoutMs = 3000;

struct Lib {
    bool ok = false;
    gboolean (*store)(const SecretSchema*, const char*, const char*, const char*, GCancellable*, GError**, ...) = nullptr;
    char* (*lookup)(const SecretSchema*, GCancellable*, GError**, ...) = nullptr;
    gboolean (*clear)(const SecretSchema*, GCancellable*, GError**, ...) = nullptr;
    void (*freePassword)(char*) = nullptr;   // wipes before freeing
    void (*errorFree)(GError*) = nullptr;
    GCancellable* (*cancellableNew)() = nullptr;
    void (*cancellableCancel)(GCancellable*) = nullptr;
    void (*objectUnref)(void*) = nullptr;
    GDBusConnection* (*busGet)(int, GCancellable*, GError**) = nullptr;
    GVariant* (*call)(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*,
                      const GVariantType*, int, int, GCancellable*, GError**) = nullptr;
    GVariant* (*variantNew)(const char*, ...) = nullptr;
    void (*variantGet)(GVariant*, const char*, ...) = nullptr;
    void (*variantUnref)(GVariant*) = nullptr;
    void (*strfreev)(char**) = nullptr;
};

const Lib& lib() {
    static Lib l;
    static std::once_flag once;
    std::call_once(once, [] {
        void* h = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!h) return;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(h, name));
            return fn != nullptr;
        };
        l.ok = sym(l.store, "secret_password_store_sync") && sym(l.lookup, "secret_password_lookup_sync") &&
            sym(l.clear, "secret_password_clear_sync") && sym(l.freePassword, "secret_password_free") &&
            sym(l.errorFree, "g_error_free") && sym(l.cancellableNew, "g_cancellable_new") &&
            sym(l.cancellableCancel, "g_cancellable_cancel") && sym(l.objectUnref, "g_object_unref") &&
            sym(l.busGet, "g_bus_get_sync") && sym(l.call, "g_dbus_connection_call_sync") &&
            sym(l.variantNew, "g_variant_new") && sym(l.variantGet, "g_variant_get") &&
            sym(l.variantUnref, "g_variant_unref") && sym(l.strfreev, "g_strfreev");
    });
    return l;
}

class SecretService final : public Store {
public:
    explicit SecretService(const Slot& slot) : service_(slot.service), account_(slot.account),
        label_(slot.service + ": " + slot.account) {
        schema_.name = "org.takengine.Credential";
        schema_.attributes[0] = {"service", 0};
        schema_.attributes[1] = {"account", 0};
    }
    ~SecretService() override { if (auto* c = cancel_.load()) lib().objectUnref(c); }
    std::string name() const override { return "SECRET SERVICE"; }
    std::string probe() override {
        const Lib& l = lib();
        if (!l.ok) return kNoStore;   // libsecret is not installed
        if (!cancel_.load()) cancel_ = l.cancellableNew();
        GError* err = nullptr;
        GDBusConnection* bus = l.busGet(kBusSession, cancel_, &err);
        if (!bus) { if (err) l.errorFree(err); return kNoStore; }   // no session bus at all
        bool found = false;
        auto ask = [&](const char* method, GVariant* args) -> GVariant* {
            GError* e = nullptr;
            GVariant* r = l.call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                 method, args, nullptr, 0, kBusTimeoutMs, cancel_, &e);
            if (e) l.errorFree(e);
            return r;
        };
        if (GVariant* r = ask("NameHasOwner", l.variantNew("(s)", "org.freedesktop.secrets"))) {
            gboolean owned = 0;
            l.variantGet(r, "(b)", &owned);
            l.variantUnref(r);
            found = owned != 0;
        }
        if (!found) if (GVariant* r = ask("ListActivatableNames", nullptr)) {
            char** names = nullptr;
            l.variantGet(r, "(^as)", &names);
            for (char** n = names; n && *n; ++n) found |= !std::strcmp(*n, "org.freedesktop.secrets");
            l.strfreev(names);
            l.variantUnref(r);
        }
        l.objectUnref(bus);
        return found ? std::string() : kNoStore;
    }
    Result load(std::string& secret, std::string& error) override {
        const Lib& l = lib();
        if (!l.ok) { error = kNoStore; return Result::Failed; }
        GError* err = nullptr;
        char* got = l.lookup(&schema_, cancel_, &err, "service", service_.c_str(), "account", account_.c_str(),
                             nullptr);
        if (err) { l.errorFree(err); if (got) l.freePassword(got); error = "PASSWORD STORE DID NOT OPEN"; return Result::Failed; }
        if (!got) return Result::Missing;
        secret.assign(got);
        l.freePassword(got);
        return Result::Ok;
    }
    bool store(const std::string& secret, std::string& error) override {
        const Lib& l = lib();
        if (!l.ok) { error = kNoStore; return false; }
        GError* err = nullptr;
        const bool ok = l.store(&schema_, nullptr, label_.c_str(), secret.c_str(), cancel_, &err,
                                "service", service_.c_str(), "account", account_.c_str(), nullptr) && !err;
        if (err) l.errorFree(err);
        if (!ok) error = "PASSWORD STORE DID NOT SAVE THE KEY";
        return ok;
    }
    bool erase(std::string& error) override {
        const Lib& l = lib();
        if (!l.ok) { error = kNoStore; return false; }
        GError* err = nullptr;
        // FALSE without an error only means there was nothing to delete.
        l.clear(&schema_, cancel_, &err, "service", service_.c_str(), "account", account_.c_str(), nullptr);
        if (!err) return true;
        l.errorFree(err);
        error = "PASSWORD STORE DID NOT DELETE THE KEY";
        return false;
    }
    void cancel() override { if (auto* c = cancel_.load()) lib().cancellableCancel(c); }
private:
    std::string service_, account_, label_;
    SecretSchema schema_{};
    std::atomic<GCancellable*> cancel_{nullptr};
};

}  // namespace

std::unique_ptr<Store> system(const Slot& slot) { return std::make_unique<SecretService>(slot); }

}  // namespace tak::credstore
