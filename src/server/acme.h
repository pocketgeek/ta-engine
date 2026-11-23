#pragma once
#include "net/tls.h"
#include <filesystem>
#include <memory>
#include <string>

namespace tak::srv {
struct AcmeOptions {
    std::string domain,email;
    std::filesystem::path state;
    std::string directory="https://acme-v02.api.letsencrypt.org/directory";
    bool agreeTerms=false;
    uint16_t challengePort=80; // production CLI always uses 80; test harness may override
};
// Owns account state, the temporary HTTP-01 listener and a renewal worker.
// Initial issuance and renewal retry in the background, respecting persisted backoff.
// context() is null until initial issuance succeeds: callers must refuse connections.
// Context snapshots are immutable; existing connections survive replacement.
class AcmeCertificates {
public:
    explicit AcmeCertificates(AcmeOptions options);
    ~AcmeCertificates();
    std::shared_ptr<tak::net::TlsContext> context() const;
    AcmeCertificates(const AcmeCertificates&)=delete;
    AcmeCertificates& operator=(const AcmeCertificates&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
