#pragma once
#include <memory>
#include <string>

struct ssl_ctx_st;
struct ssl_st;
namespace tak::net {
bool attachTlsSocket(ssl_st* ssl,int fd);
// OpenSSL is statically linked. Certificates/keys stay outside the executable.
struct TlsContext {
    ssl_ctx_st* handle=nullptr;
    ~TlsContext();
    static std::shared_ptr<TlsContext> server(const std::string& certificate,const std::string& key);
    static std::shared_ptr<TlsContext> serverPem(const std::string& certificateChain,const std::string& privateKey);
    static std::shared_ptr<TlsContext> client();
};
#ifdef __APPLE__
// The macOS system trust anchors (the ones client() imports) as one PEM bundle.
// OpenSSL never reads the keychain, so streaming hands these to FFmpeg's OpenSSL
// TLS as its CA file. Empty when the system returns no anchors.
std::string systemRootsPem();
#endif
}
