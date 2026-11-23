// Windows credential store: Credential Manager (Control Panel > Credential Manager >
// Windows Credentials > Generic). One CRED_TYPE_GENERIC entry, TargetName
// "service/account", persisted per user on this machine (CRED_PERSIST_LOCAL_MACHINE: not
// roamed with a domain profile). The blob is the key's UTF-8 bytes. Windows protects it
// with the user's logon credentials. Under Proton/Wine the same calls reach Wine's own
// emulation, which keeps the entry in the prefix's user.reg, encrypted with a key stored
// beside it (checked under Proton 10) -- much weaker than native.
// advapi32 provides the Cred* functions.

#include "client/credstore.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

namespace tak::credstore {
namespace {

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

class CredentialManager final : public Store {
public:
    explicit CredentialManager(const Slot& slot) : target_(wide(slot.service + "/" + slot.account)),
        user_(wide(slot.account)) {}
    std::string name() const override { return "CREDENTIAL MANAGER"; }
    // A logon session without stored credentials (a service, a key-authenticated ssh
    // shell) cannot use the store; reading the slot tells, and never prompts.
    std::string probe() override {
        PCREDENTIALW cred = nullptr;
        if (CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
            SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
            CredFree(cred);
            return {};
        }
        return GetLastError() == ERROR_NO_SUCH_LOGON_SESSION ? kNoStore : std::string();
    }
    Result load(std::string& secret, std::string& error) override {
        PCREDENTIALW cred = nullptr;
        if (!CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
            if (GetLastError() == ERROR_NOT_FOUND) return Result::Missing;
            error = "CREDENTIAL MANAGER DID NOT RETURN THE KEY";
            return Result::Failed;
        }
        secret.assign(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
        CredFree(cred);
        return Result::Ok;
    }
    bool store(const std::string& secret, std::string& error) override {
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = target_.data();
        cred.UserName = user_.data();
        cred.CredentialBlobSize = DWORD(secret.size());
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (CredWriteW(&cred, 0)) return true;
        error = "CREDENTIAL MANAGER DID NOT SAVE THE KEY";
        return false;
    }
    bool erase(std::string& error) override {
        if (CredDeleteW(target_.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) return true;
        error = "CREDENTIAL MANAGER DID NOT DELETE THE KEY";
        return false;
    }
private:
    std::wstring target_, user_;
};

}  // namespace

std::unique_ptr<Store> system(const Slot& slot) { return std::make_unique<CredentialManager>(slot); }

}  // namespace tak::credstore
