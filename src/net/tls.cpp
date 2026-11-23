#include "net/tls.h"
#include "net/netcompat.h"
#include <cstdint>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#endif

namespace tak::net {
// Socket BIO with MSG_NOSIGNAL: an abruptly closed TLS peer must never raise
// SIGPIPE and terminate the server (OpenSSL's stock Unix socket BIO uses write).
bool attachTlsSocket(SSL* ssl,int fd) {
    static BIO_METHOD* method=[] {
        BIO_METHOD* m=BIO_meth_new(BIO_TYPE_SOURCE_SINK|BIO_get_new_index(),"TAK socket");
        if(!m)return m;
        BIO_meth_set_create(m,[](BIO* b){BIO_set_init(b,1);return 1;});
        BIO_meth_set_destroy(m,[](BIO*){return 1;});
        BIO_meth_set_read(m,[](BIO* b,char* data,int size) {
            BIO_clear_retry_flags(b);
            const int result=int(::recv(int(reinterpret_cast<intptr_t>(BIO_get_data(b))),data,size,0));
            if(result<0 && (sockWouldBlock(sockErr()) || sockInterrupted(sockErr())))BIO_set_retry_read(b);
            return result;
        });
        BIO_meth_set_write(m,[](BIO* b,const char* data,int size) {
            BIO_clear_retry_flags(b);
            const int result=int(::send(int(reinterpret_cast<intptr_t>(BIO_get_data(b))),data,size,MSG_NOSIGNAL));
            if(result<0 && (sockWouldBlock(sockErr()) || sockInterrupted(sockErr())))BIO_set_retry_write(b);
            return result;
        });
        BIO_meth_set_ctrl(m,[](BIO* b,int command,long,void* pointer)->long {
            if(command==BIO_CTRL_FLUSH)return 1;
            if(command==BIO_C_GET_FD) {const int n=int(reinterpret_cast<intptr_t>(BIO_get_data(b)));if(pointer)*static_cast<int*>(pointer)=n;return n;}
            return 0;
        });
        return m;
    }();
    if(!method)return false;
    BIO* bio=BIO_new(method);if(!bio)return false;
    BIO_set_data(bio,reinterpret_cast<void*>(intptr_t(fd)));
    SSL_set_bio(ssl,bio,bio);return true;
}
TlsContext::~TlsContext() {SSL_CTX_free(handle);}
namespace {
std::shared_ptr<TlsContext> context(bool server) {
    auto c=std::make_shared<TlsContext>();
    c->handle=SSL_CTX_new(server?TLS_server_method():TLS_client_method());
    if(!c->handle || !SSL_CTX_set_min_proto_version(c->handle,TLS1_2_VERSION))
        throw std::runtime_error("cannot initialize TLS");
    SSL_CTX_set_options(c->handle,SSL_OP_NO_COMPRESSION|SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_mode(c->handle,SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    // Session tickets add no value to long-lived game sessions.
    SSL_CTX_set_num_tickets(c->handle,0);
    return c;
}
#if defined(_WIN32) || defined(__APPLE__)
void addRoot(SSL_CTX* ctx,const unsigned char* bytes,long length) {
    auto* cert=d2i_X509(nullptr,&bytes,length);
    if(cert) {X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx),cert);X509_free(cert);}
    ERR_clear_error(); // duplicate anchors are harmless
}
#endif
#ifdef __APPLE__
// Each system trust anchor's DER bytes.
template<class F> void forEachSystemRoot(F&& visit) {
    CFArrayRef roots=nullptr;
    if(SecTrustCopyAnchorCertificates(&roots)!=errSecSuccess || !roots)return;
    for(CFIndex i=0;i<CFArrayGetCount(roots);++i) {
        auto cert=static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(roots,i)));
        CFDataRef bytes=SecCertificateCopyData(cert);
        if(bytes) {visit(CFDataGetBytePtr(bytes),long(CFDataGetLength(bytes)));CFRelease(bytes);}
    }
    CFRelease(roots);
}
#endif
}
#ifdef __APPLE__
std::string systemRootsPem() {
    std::unique_ptr<BIO,decltype(&BIO_free)> out(BIO_new(BIO_s_mem()),BIO_free);
    if(!out)return {};
    forEachSystemRoot([&](const unsigned char* bytes,long length) {
        if(auto* cert=d2i_X509(nullptr,&bytes,length)) {PEM_write_bio_X509(out.get(),cert);X509_free(cert);}
    });
    ERR_clear_error();
    char* data=nullptr;
    const long size=BIO_get_mem_data(out.get(),&data);
    return size>0?std::string(data,size_t(size)):std::string();
}
#endif
std::shared_ptr<TlsContext> TlsContext::server(const std::string& certificate,const std::string& key) {
    auto c=context(true);
    if(SSL_CTX_use_certificate_chain_file(c->handle,certificate.c_str())!=1 ||
       SSL_CTX_use_PrivateKey_file(c->handle,key.c_str(),SSL_FILETYPE_PEM)!=1 ||
       SSL_CTX_check_private_key(c->handle)!=1)throw std::runtime_error("cannot load TLS certificate/private key");
    return c;
}
std::shared_ptr<TlsContext> TlsContext::serverPem(const std::string& chain,const std::string& privateKey) {
    if(chain.size()>1024*1024 || privateKey.size()>1024*1024)
        throw std::runtime_error("TLS certificate bundle too large");
    auto c=context(true);
    std::unique_ptr<BIO,decltype(&BIO_free)> certificates(BIO_new_mem_buf(chain.data(),int(chain.size())),BIO_free);
    std::unique_ptr<BIO,decltype(&BIO_free)> keys(BIO_new_mem_buf(privateKey.data(),int(privateKey.size())),BIO_free);
    if(!certificates || !keys)throw std::runtime_error("cannot allocate TLS bundle reader");
    std::unique_ptr<X509,decltype(&X509_free)> leaf(PEM_read_bio_X509_AUX(certificates.get(),nullptr,nullptr,nullptr),X509_free);
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(PEM_read_bio_PrivateKey(keys.get(),nullptr,nullptr,nullptr),EVP_PKEY_free);
    if(!leaf || !key || SSL_CTX_use_certificate(c->handle,leaf.get())!=1 ||
       SSL_CTX_use_PrivateKey(c->handle,key.get())!=1 || SSL_CTX_check_private_key(c->handle)!=1)
        throw std::runtime_error("invalid TLS certificate/private key bundle");
    while(auto* extra=PEM_read_bio_X509(certificates.get(),nullptr,nullptr,nullptr)) {
        if(SSL_CTX_add_extra_chain_cert(c->handle,extra)!=1) {
            X509_free(extra);throw std::runtime_error("invalid TLS certificate chain");
        }
    }
    ERR_clear_error();return c;
}
std::shared_ptr<TlsContext> TlsContext::client() {
    auto c=context(false);
    SSL_CTX_set_verify(c->handle,SSL_VERIFY_PEER,nullptr);
    SSL_CTX_set_default_verify_paths(c->handle); // includes explicit SSL_CERT_FILE/DIR
#ifdef _WIN32
    HCERTSTORE roots=CertOpenSystemStoreW(0,L"ROOT");
    if(roots) {
        PCCERT_CONTEXT cert=nullptr;
        while((cert=CertEnumCertificatesInStore(roots,cert)))addRoot(c->handle,cert->pbCertEncoded,long(cert->cbCertEncoded));
        CertCloseStore(roots,0);
    }
#elif defined(__APPLE__)
    forEachSystemRoot([&](const unsigned char* bytes,long length){addRoot(c->handle,bytes,length);});
#else
    // Our static OpenSSL prefix is not the operating system's trust-store path.
    for(const char* path:{"/etc/ssl/certs/ca-certificates.crt","/etc/pki/tls/certs/ca-bundle.crt","/etc/ssl/cert.pem"})
        SSL_CTX_load_verify_locations(c->handle,path,nullptr);
    SSL_CTX_load_verify_locations(c->handle,nullptr,"/etc/ssl/certs");
#endif
    ERR_clear_error();return c;
}
}
