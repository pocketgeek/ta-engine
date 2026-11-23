#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "net/netcompat.h"
#include "net/conn.h"
#include "server/acme_internal.h"
#include "server/acme.h"
#include <ctime>
#include <thread>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <fstream>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
using namespace tak::net;
using tak::srv::acme_detail::Challenge;
namespace {
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
uint16_t unusedPort() {
    int fd=int(socket(AF_INET,SOCK_STREAM,0));check(fd>=0,"socket");
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof addr)==0,"bind");
    socklen_t n=sizeof addr;check(getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&n)==0,"getsockname");sockClose(fd);return ntohs(addr.sin_port);
}
int connectTo(uint16_t port,bool independentSource=false) {
    // ::1 is a separate local source from the flood's 127.0.0.1. Unlike
    // 127.0.0.2, it needs no administrator-created loopback alias on macOS.
    if(independentSource) {
        int fd=int(socket(AF_INET6,SOCK_STREAM,0));
        if(fd>=0) {
            sockaddr_in6 addr{};addr.sin6_family=AF_INET6;
            addr.sin6_addr=in6addr_loopback;addr.sin6_port=htons(port);
            if(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof addr)==0)return fd;
            sockClose(fd);
        }
    }
    int fd=int(socket(AF_INET,SOCK_STREAM,0));check(fd>=0,"socket");
    if(independentSource) {
        // Retain the IPv4-only fixture on hosts without IPv6 loopback.
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(0x7f000002);
        if(bind(fd,reinterpret_cast<sockaddr*>(&local),sizeof local)!=0) {
            sockClose(fd);throw std::runtime_error("need IPv6 loopback or 127.0.0.2 for independent source test");
        }
    }
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons(port);
    if(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof addr)) {sockClose(fd);return -1;}return fd;
}
std::string request(uint16_t port,const std::string& text) {
    int fd=connectTo(port,true);check(fd>=0,"connect responder");
    check(send(fd,text.data(),int(text.size()),MSG_NOSIGNAL)==int(text.size()),"send request");
    sockSetNonBlock(fd);std::string response;char bytes[2048];
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(std::chrono::steady_clock::now()<deadline) {
        pollfd p{};p.fd=fd;p.events=POLLIN;TAK_POLL(&p,1,100);
        int n=int(recv(fd,bytes,sizeof bytes,0));if(n>0)response.append(bytes,size_t(n));else if(n==0)break;
        else if(!sockWouldBlock(sockErr()) && !sockInterrupted(sockErr()))break;
    }
    sockClose(fd);return response;
}
}
int main() {
    try {
        netStartup();using tak::srv::acme_detail::validDomain;
        check(validDomain("tak.pgnet.us"),"valid hostname");
        for(const char* value:{"","localhost","*.example.org","127.0.0.1","bad_name.test","x..test","-a.test","a-.test","tls://a.test","a.test:80","a.test\r\n"})check(!validDomain(value),"invalid hostname accepted");
        const auto port=unusedPort();const std::string token(43,'a'),auth=token+".thumbprint";
        check(connectTo(port)<0,"port should start closed");
        {
            Challenge listener(port,token,auth);
            auto response=request(port,"GET /.well-known/acme-challenge/"+token+" HTTP/1.1\r\nHost: example.test\r\n\r\n");
            check(response.starts_with("HTTP/1.1 200 OK") && response.ends_with(auth),"challenge response");
            for(const char* path:{"/","/../current.pem","/.well-known/acme-challenge/wrong"})
                check(request(port,std::string("GET ")+path+" HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404"),"unknown path");
            check(request(port,"POST /.well-known/acme-challenge/"+token+" HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404"),"unexpected method");
            std::vector<int> slow;for(int i=0;i<40;++i)slow.push_back(connectTo(port));
            check(request(port,"GET /.well-known/acme-challenge/"+token+" HTTP/1.1\r\n\r\n").ends_with(auth),"one source blocked another source's challenge");
            for(int fd:slow)if(fd>=0)sockClose(fd);
            bool rejected=false;try {Challenge duplicate(port,token,auth);}catch(const std::exception&) {rejected=true;}
            check(rejected,"occupied challenge port accepted");
        }
        check(connectTo(port)<0,"port remained open after validation");
        const auto failedPort=unusedPort();
        try {Challenge listener(failedPort,token,auth);throw std::runtime_error("simulated CA failure");}catch(const std::exception&){}
        check(connectTo(failedPort)<0,"port remained open after failure");
        bool rejected=false;try {Challenge listener(port,"../invalid",auth);}catch(const std::exception&){rejected=true;}
        check(rejected && connectTo(port)<0,"invalid token opened listener");
        // ACME Renewal Information (RFC 9773). certID uses the RFC's own example
        // values: AKI 69:88:5B:...:C8:D4 and serial 0x87654321 (DER 00 87 65 43 21).
        {
            using namespace tak::srv::acme_detail;
            EVP_PKEY* key=EVP_RSA_gen(2048);X509* cert=X509_new();check(key && cert,"ARI fixture");
            X509_set_version(cert,2);ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert),0x87654321u);
            X509_gmtime_adj(X509_getm_notBefore(cert),-60);X509_gmtime_adj(X509_getm_notAfter(cert),90*86400);X509_set_pubkey(cert,key);
            const unsigned char keyId[]={0x69,0x88,0x5B,0x6B,0x87,0x46,0x40,0x41,0xE1,0xB3,0x7B,0x84,0x7B,0xA0,0xAE,0x2C,0xDE,0x01,0xC8,0xD4};
            AUTHORITY_KEYID* aki=AUTHORITY_KEYID_new();aki->keyid=ASN1_OCTET_STRING_new();ASN1_OCTET_STRING_set(aki->keyid,keyId,int(sizeof keyId));
            check(X509_add1_ext_i2d(cert,NID_authority_key_identifier,aki,0,X509V3_ADD_DEFAULT)==1,"ARI AKI");AUTHORITY_KEYID_free(aki);
            check(X509_sign(cert,key,EVP_sha256())>0,"ARI signature");
            BIO* bio=BIO_new(BIO_s_mem());PEM_write_bio_X509(bio,cert);char* bytes=nullptr;const long length=BIO_get_mem_data(bio,&bytes);
            const std::string pem(bytes,size_t(length));BIO_free(bio);X509_free(cert);EVP_PKEY_free(key);
            const auto id=ariCertId(pem);
            check(id=="aYhba4dGQEHhs3uEe6CuLN4ByNQ.AIdlQyE","RFC 9773 certID");
            check(parseRfc3339("2025-01-02T04:00:00Z")==std::time_t(1735790400) &&
                  parseRfc3339("2025-01-02T06:00:00.123456+02:00")==std::time_t(1735790400) &&
                  parseRfc3339("2024-02-29t23:59:59z")==std::time_t(1709251199),"RFC 3339 timestamps");
            for(const char* bad:{"","2025-13-01T00:00:00Z","2025-01-02 04:00:00Z","2025-01-02T04:00:00","2025-01-02T04:00:00.Z","2025-01-02T04:00:00Zx"})
                check(!parseRfc3339(bad),"invalid RFC 3339 accepted");
            auto window=[](const char* start,const char* end) {return std::string("{\"suggestedWindow\":{\"start\":\"")+start+"\",\"end\":\""+end+"\"}}";};
            const std::time_t now=1735000000,notAfter=1740000000,start=1735790400,end=start+2*86400;
            const auto chosen=ariRenewalTime(window("2025-01-02T04:00:00Z","2025-01-04T04:00:00Z"),id,notAfter,now);
            check(chosen && *chosen>=start && *chosen<=end,"renewal outside suggested window");
            check(chosen==ariRenewalTime(window("2025-01-02T04:00:00Z","2025-01-04T04:00:00Z"),id,notAfter,now),"window choice not stable per certificate");
            check(ariRenewalTime(window("2024-01-02T04:00:00Z","2024-01-04T04:00:00Z"),id,notAfter,now)==now,"past window must renew now");
            check(!ariRenewalTime(window("2025-01-04T04:00:00Z","2025-01-02T04:00:00Z"),id,notAfter,now),"inverted window accepted");
            const auto clipped=ariRenewalTime(window("2025-01-02T04:00:00Z","2026-01-05T04:00:00Z"),id,notAfter,now);
            check(clipped && *clipped>=start && *clipped<=notAfter,"window past expiry not clipped to notAfter");
            check(!ariRenewalTime(window("2026-01-04T04:00:00Z","2026-01-05T04:00:00Z"),id,notAfter,now),"window after expiry accepted");
            for(const char* bad:{"","{}","{\"suggestedWindow\":{\"start\":1}}","not json"})check(!ariRenewalTime(bad,id,notAfter,now),"malformed renewalInfo accepted");
        }
        tak::srv::AcmeOptions options;options.domain="tak.pgnet.us";options.state="unused-acme-test-state";
        rejected=false;try {tak::srv::AcmeCertificates certificates(options);}catch(const std::exception&) {rejected=true;}
        check(rejected,"terms agreement not enforced");
        // Saved-state startup must work offline, including Unicode state paths
        // and owner-only Windows ACLs. No certificate authority is contacted.
        const auto root=std::filesystem::temp_directory_path()/
            (std::filesystem::path(u8"tak-acme-é-")+=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        try {
            EVP_PKEY* key=EVP_RSA_gen(2048);check(key,"fixture key");
            X509* cert=X509_new();check(cert,"fixture certificate");
            X509_set_version(cert,2);ASN1_INTEGER_set(X509_get_serialNumber(cert),1);
            X509_gmtime_adj(X509_getm_notBefore(cert),-60);X509_gmtime_adj(X509_getm_notAfter(cert),7*86400);
            X509_set_pubkey(cert,key);auto* subject=X509_get_subject_name(cert);
            X509_NAME_add_entry_by_txt(subject,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>("acme.example.test"),-1,-1,0);
            X509_set_issuer_name(cert,subject);
            auto* san=X509V3_EXT_conf_nid(nullptr,nullptr,NID_subject_alt_name,"DNS:acme.example.test");
            check(san && X509_add_ext(cert,san,-1)==1,"fixture SAN");X509_EXTENSION_free(san);
            check(X509_sign(cert,key,EVP_sha256())>0,"fixture signature");
            BIO* bio=BIO_new(BIO_s_mem());check(bio,"fixture BIO");
            check(PEM_write_bio_X509(bio,cert)==1 && PEM_write_bio_PrivateKey(bio,key,nullptr,nullptr,0,nullptr,nullptr)==1,"fixture PEM");
            char* bytes=nullptr;const long length=BIO_get_mem_data(bio,&bytes);
            {std::ofstream out(root/"current.pem",std::ios::binary);out.write(bytes,length);}
            BIO_free(bio);X509_free(cert);EVP_PKEY_free(key);
            options.domain="acme.example.test";options.state=root;options.agreeTerms=true;
            {
                tak::srv::AcmeCertificates saved(options);check(bool(saved.context()),"offline saved certificate");
                rejected=false;try {tak::srv::AcmeCertificates duplicate(options);}catch(const std::exception&) {rejected=true;}
                check(rejected,"shared state lease not enforced");
                check(!std::filesystem::exists(root/"account.pem"),"offline startup contacted CA");
            }
            std::filesystem::remove(root/"current.pem");
            // Local closed port: a transient CA failure records backoff but does
            // not terminate the service. No public CA or DNS is contacted.
            options.directory="https://localhost:"+std::to_string(unusedPort())+"/dir";
            std::filesystem::remove(root/"identity.json");
            {
                tak::srv::AcmeCertificates failed(options);
                auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(!std::filesystem::exists(root/"retry.json") && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                check(std::filesystem::exists(root/"retry.json") && !failed.context(),"transient failure did not persist retry");
            }
            {std::ofstream retry(root/"retry.json");retry<<"{\"next\":"<<(std::time(nullptr)+7200)<<",\"failures\":2}";}
            auto began=std::chrono::steady_clock::now();
            {tak::srv::AcmeCertificates waiting(options);check(!waiting.context(),"backoff served plaintext context");}
            check(std::chrono::steady_clock::now()-began<std::chrono::seconds(1),"persisted backoff blocked startup/shutdown");
            std::filesystem::remove_all(root);
        }catch(...) {std::filesystem::remove_all(root);throw;}
        std::puts("ACME responder, saved state and configuration checks passed");return 0;
    }catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
