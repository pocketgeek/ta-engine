#pragma once
#include <ctime>
#include <string>

struct x509_st;
struct ssl_st;
namespace tak::net {
// Online revocation for a verified certificate, from the CRL its issuer names in
// the certificate's CRL Distribution Points. Let's Encrypt no longer runs OCSP, so
// CRLs are the only online source. Fetching is soft-fail: an unreachable or
// unusable CRL is Unavailable (the caller connects and logs it), and only a valid
// issuer-signed CRL that lists the serial is a definite Revoked.
enum class Revocation {Good,Revoked,Unavailable,NotChecked};
struct RevocationStatus {Revocation state=Revocation::NotChecked;std::string detail;};
// The first http:// CRL distribution point, or empty when the certificate has none
// (short-lived certificates omit it; such a certificate is NotChecked).
std::string crlUrl(x509_st* leaf);
// Checks a DER CRL fetched from `url` at time `now`: issuer name, signature, key
// usage, freshness and distribution-point scope must all hold before its entries
// are trusted. No network access.
RevocationStatus crlStatus(const std::string& der,x509_st* leaf,x509_st* issuer,const std::string& url,std::time_t now);
// Fetches (or reuses a cached) CRL for `leaf` within `timeoutMs` and checks it.
RevocationStatus checkRevocation(x509_st* leaf,x509_st* issuer,int timeoutMs=4000);
// Checks the peer chain of a completed client handshake (leaf + its issuer).
RevocationStatus checkPeerRevocation(ssl_st* ssl,int timeoutMs=4000);
}
