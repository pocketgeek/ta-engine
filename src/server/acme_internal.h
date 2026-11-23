#pragma once
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
namespace tak::srv::acme_detail {
// Internal HTTP-01 responder. The listener exists only for this object's lifetime.
class Challenge {
public:
    Challenge(uint16_t port,std::string token,std::string authorization);
    ~Challenge();
    Challenge(const Challenge&)=delete;
    Challenge& operator=(const Challenge&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool validDomain(const std::string& domain);
// ACME Renewal Information (RFC 9773). certID of the first certificate in a PEM
// bundle, or empty without an Authority Key Identifier.
std::string ariCertId(const std::string& pem);
std::optional<std::time_t> parseRfc3339(const std::string& text);
// Renewal time chosen from a renewalInfo body, or nullopt when it is unusable.
std::optional<std::time_t> ariRenewalTime(const std::string& body,const std::string& certId,std::time_t notAfter,std::time_t now);
}
