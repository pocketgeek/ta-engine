#include "net/conn.h"
#include "net/netcompat.h"
#include "net/crypto.h"
#include "net/revocation.h"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>

using namespace tak::net;
static void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
static void certificate(const std::filesystem::path& certPath,const std::filesystem::path& keyPath,bool expired) {
    EVP_PKEY* key=EVP_RSA_gen(2048);check(key,"key generation");
    X509* cert=X509_new();check(cert,"certificate allocation");
    X509_set_version(cert,2);ASN1_INTEGER_set(X509_get_serialNumber(cert),expired?2:1);
    X509_gmtime_adj(X509_getm_notBefore(cert),expired?-259200:-60);
    X509_gmtime_adj(X509_getm_notAfter(cert),expired?-86400:86400);
    X509_set_pubkey(cert,key);
    X509_NAME* name=X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>("localhost"),-1,-1,0);
    X509_set_issuer_name(cert,name);
    X509_EXTENSION* san=X509V3_EXT_conf_nid(nullptr,nullptr,NID_subject_alt_name,const_cast<char*>("DNS:localhost"));
    check(san,"SAN generation");X509_add_ext(cert,san,-1);X509_EXTENSION_free(san);
    check(X509_sign(cert,key,EVP_sha256())>0,"certificate signing");
    FILE* out=std::fopen(certPath.string().c_str(),"wb");check(out,"certificate file");
    check(PEM_write_X509(out,cert)==1,"certificate write");std::fclose(out);
    out=std::fopen(keyPath.string().c_str(),"wb");check(out,"key file");
    check(PEM_write_PrivateKey(out,key,nullptr,nullptr,0,nullptr,nullptr)==1,"key write");std::fclose(out);
    X509_free(cert);EVP_PKEY_free(key);
}
static void trust(const std::filesystem::path& cert) {
#ifdef _WIN32
    _putenv_s("SSL_CERT_FILE",cert.string().c_str());
#else
    setenv("SSL_CERT_FILE",cert.string().c_str(),1);
#endif
}
struct Echo {
    int listener=-1;uint16_t port=0;std::atomic<bool> stop=false;
    std::thread worker;
    std::mutex activeMutex;
    std::shared_ptr<TlsContext> active;
    std::shared_ptr<TlsContext> activeContext() {std::lock_guard lock(activeMutex);return active;}
    void setActive(std::shared_ptr<TlsContext> context) {std::lock_guard lock(activeMutex);active=std::move(context);}
    explicit Echo(std::shared_ptr<TlsContext> context):active(std::move(context)) {
        std::string error;listener=listenOn(0,error,true);check(listener>=0,"listen");
        sockaddr_in address{};socklen_t length=sizeof address;
        check(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0,"port");port=ntohs(address.sin_port);
        worker=std::thread([this] {
            std::vector<Conn> peers;
            while(!stop.load()) {
                const int fd=int(accept(listener,nullptr,nullptr));
                if(fd>=0) {setupSocket(fd);peers.emplace_back(fd,activeContext());}
                for(auto& peer:peers) {
                    if(!peer.ok())continue;
                    peer.recv();Frame f;
                    while(peer.ok() && peer.poll(f))peer.send(f.kind,f.payload);
                    peer.flushWrite();
                    if(peer.peerClosed() || !peer.ok())peer.closeNow();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~Echo() {stop=true;if(worker.joinable())worker.join();if(listener>=0)sockClose(listener);}
};
// Revocation fixtures: a CA, CRL-bearing leaves, and a loopback HTTP CRL host.
using X509Ptr=std::unique_ptr<X509,decltype(&X509_free)>;
using KeyPtr=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
static void extend(X509* cert,X509* issuer,int nid,const char* value) {
    X509V3_CTX ctx;X509V3_set_ctx(&ctx,issuer,cert,nullptr,nullptr,0);
    X509_EXTENSION* ext=X509V3_EXT_conf_nid(nullptr,&ctx,nid,value);check(ext,"extension");
    check(X509_add_ext(cert,ext,-1)==1,"add extension");X509_EXTENSION_free(ext);
}
static X509Ptr issue(EVP_PKEY* key,X509* issuer,EVP_PKEY* signer,long serial,const char* name,const std::string& crl) {
    X509Ptr cert(X509_new(),X509_free);check(bool(cert),"certificate allocation");
    X509_set_version(cert.get(),2);ASN1_INTEGER_set(X509_get_serialNumber(cert.get()),serial);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()),-60);X509_gmtime_adj(X509_getm_notAfter(cert.get()),86400);
    X509_set_pubkey(cert.get(),key);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(cert.get()),"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>(name),-1,-1,0);
    X509* parent=issuer?issuer:cert.get();
    X509_set_issuer_name(cert.get(),X509_get_subject_name(parent));
    if(!issuer) {
        extend(cert.get(),parent,NID_basic_constraints,"critical,CA:TRUE");
        extend(cert.get(),parent,NID_key_usage,"critical,keyCertSign,cRLSign");
    } else {
        extend(cert.get(),parent,NID_subject_alt_name,"DNS:localhost");
        if(!crl.empty())extend(cert.get(),parent,NID_crl_distribution_points,("URI:"+crl).c_str());
    }
    check(X509_sign(cert.get(),signer,EVP_sha256())>0,"certificate signing");return cert;
}
static std::string makeCrl(X509* ca,EVP_PKEY* signer,long revoked,long nextUpdate,const std::string& scope) {
    std::unique_ptr<X509_CRL,decltype(&X509_CRL_free)> crl(X509_CRL_new(),X509_CRL_free);check(bool(crl),"CRL allocation");
    X509_CRL_set_version(crl.get(),1);X509_CRL_set_issuer_name(crl.get(),X509_get_subject_name(ca));
    std::unique_ptr<ASN1_TIME,decltype(&ASN1_TIME_free)> last(X509_gmtime_adj(nullptr,-3600),ASN1_TIME_free),next(X509_gmtime_adj(nullptr,nextUpdate),ASN1_TIME_free);
    X509_CRL_set1_lastUpdate(crl.get(),last.get());X509_CRL_set1_nextUpdate(crl.get(),next.get());
    if(revoked) {
        X509_REVOKED* entry=X509_REVOKED_new();ASN1_INTEGER* serial=ASN1_INTEGER_new();ASN1_INTEGER_set(serial,revoked);
        X509_REVOKED_set_serialNumber(entry,serial);X509_REVOKED_set_revocationDate(entry,last.get());
        ASN1_INTEGER_free(serial);X509_CRL_add0_revoked(crl.get(),entry);
    }
    if(!scope.empty()) {
        ISSUING_DIST_POINT* idp=ISSUING_DIST_POINT_new();check(idp,"CRL scope allocation");
        idp->distpoint=DIST_POINT_NAME_new();idp->distpoint->type=0;idp->distpoint->name.fullname=GENERAL_NAMES_new();
        ASN1_IA5STRING* uri=ASN1_IA5STRING_new();ASN1_STRING_set(uri,scope.data(),int(scope.size()));
        GENERAL_NAME* name=GENERAL_NAME_new();GENERAL_NAME_set0_value(name,GEN_URI,uri);
        sk_GENERAL_NAME_push(idp->distpoint->name.fullname,name);idp->onlyuser=1;
        const int added=X509_CRL_add1_ext_i2d(crl.get(),NID_issuing_distribution_point,idp,1,X509V3_ADD_DEFAULT);
        ISSUING_DIST_POINT_free(idp);check(added==1,"CRL scope");
    }
    X509_CRL_sort(crl.get());check(X509_CRL_sign(crl.get(),signer,EVP_sha256())>0,"CRL signing");
    const int n=i2d_X509_CRL(crl.get(),nullptr);std::string der(size_t(n),'\0');
    auto* p=reinterpret_cast<unsigned char*>(der.data());i2d_X509_CRL(crl.get(),&p);return der;
}
static void writePem(const std::filesystem::path& path,std::initializer_list<X509*> certs,EVP_PKEY* key) {
    FILE* out=std::fopen(path.string().c_str(),"wb");check(out,"PEM file");
    for(X509* cert:certs)check(PEM_write_X509(out,cert)==1,"PEM certificate");
    if(key)check(PEM_write_PrivateKey(out,key,nullptr,nullptr,0,nullptr,nullptr)==1,"PEM key");
    std::fclose(out);
}
struct CrlHost {
    int listener=-1;uint16_t port=0;std::atomic<bool> stop=false;std::thread worker;
    std::mutex lock;std::map<std::string,std::string> files;std::map<std::string,int> hits;
    CrlHost() {
        std::string error;listener=listenOn(0,error,true);check(listener>=0,"CRL listen");
        sockaddr_in address{};socklen_t length=sizeof address;
        check(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0,"CRL port");port=ntohs(address.sin_port);
        worker=std::thread([this] {
            while(!stop.load()) {
                pollfd p{};p.fd=listener;p.events=POLLIN;if(TAK_POLL(&p,1,20)<=0)continue;
                const int fd=int(accept(listener,nullptr,nullptr));if(fd<0)continue;
                std::string request;char buf[2048];
                for(int i=0;i<200 && request.find("\r\n\r\n")==request.npos;++i) {
                    pollfd q{};q.fd=fd;q.events=POLLIN;if(TAK_POLL(&q,1,10)<=0)continue;
                    const int n=int(recv(fd,buf,sizeof buf,0));if(n<=0)break;request.append(buf,size_t(n));
                }
                const auto path=request.substr(4,request.find(' ',4)-4);std::string response;
                {
                    std::lock_guard guard(lock);++hits[path];auto f=files.find(path);
                    response=f==files.end()?std::string("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n"):
                        "HTTP/1.0 200 OK\r\nContent-Type: application/pkix-crl\r\nContent-Length: "+std::to_string(f->second.size())+"\r\n\r\n"+f->second;
                }
                for(size_t sent=0;sent<response.size();) {
                    const int n=int(::send(fd,response.data()+sent,int(response.size()-sent),MSG_NOSIGNAL));
                    if(n>0)sent+=size_t(n);
                    else if(n<0 && (sockWouldBlock(sockErr()) || sockInterrupted(sockErr())))std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    else break;
                }
                sockClose(fd);
            }
        });
    }
    std::string url(const std::string& path) const {return "http://127.0.0.1:"+std::to_string(port)+path;}
    int count(const std::string& path) {std::lock_guard guard(lock);return hits[path];}
    ~CrlHost() {stop=true;if(worker.joinable())worker.join();if(listener>=0)sockClose(listener);}
};
static uint16_t closedPort() {
    std::string error;const int fd=listenOn(0,error,true);check(fd>=0,"probe listen");
    sockaddr_in address{};socklen_t length=sizeof address;getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length);
    sockClose(fd);return ntohs(address.sin_port);
}
static void revocationChecks(const std::filesystem::path& root) {
    KeyPtr caKey(EVP_RSA_gen(2048),EVP_PKEY_free),leafKey(EVP_RSA_gen(2048),EVP_PKEY_free),otherKey(EVP_RSA_gen(2048),EVP_PKEY_free);
    check(caKey && leafKey && otherKey,"revocation keys");
    auto ca=issue(caKey.get(),nullptr,caKey.get(),1,"TAK Test CA","");
    CrlHost host;
    const auto goodUrl=host.url("/good.crl"),revokedUrl=host.url("/revoked.crl");
    const auto deadUrl="http://127.0.0.1:"+std::to_string(closedPort())+"/dead.crl";
    auto good=issue(leafKey.get(),ca.get(),caKey.get(),10,"localhost",goodUrl);
    auto revoked=issue(leafKey.get(),ca.get(),caKey.get(),11,"localhost",revokedUrl);
    auto dead=issue(leafKey.get(),ca.get(),caKey.get(),12,"localhost",deadUrl);
    auto plain=issue(leafKey.get(),ca.get(),caKey.get(),13,"localhost","");
    // Offline CRL evaluation: only a fresh, issuer-signed, in-scope CRL is trusted.
    const auto now=std::time(nullptr);
    check(crlUrl(good.get())==goodUrl && crlUrl(plain.get()).empty(),"CRL distribution point");
    check(crlStatus(makeCrl(ca.get(),caKey.get(),99,86400,goodUrl),good.get(),ca.get(),goodUrl,now).state==Revocation::Good,"unlisted serial not good");
    check(crlStatus(makeCrl(ca.get(),caKey.get(),10,86400,""),good.get(),ca.get(),goodUrl,now).state==Revocation::Revoked,"listed serial not revoked");
    check(crlStatus(makeCrl(ca.get(),otherKey.get(),10,86400,""),good.get(),ca.get(),goodUrl,now).state==Revocation::Unavailable,"forged CRL trusted");
    check(crlStatus(makeCrl(ca.get(),caKey.get(),10,-600,""),good.get(),ca.get(),goodUrl,now).state==Revocation::Unavailable,"stale CRL trusted");
    check(crlStatus(makeCrl(ca.get(),caKey.get(),10,86400,host.url("/other.crl")),good.get(),ca.get(),goodUrl,now).state==Revocation::Unavailable,"out-of-scope CRL trusted");
    check(crlStatus("not a crl",good.get(),ca.get(),goodUrl,now).state==Revocation::Unavailable,"malformed CRL trusted");
    check(crlStatus(makeCrl(good.get(),leafKey.get(),10,86400,""),good.get(),good.get(),goodUrl,now).state==Revocation::Unavailable,"non-CA CRL trusted");
    {
        std::lock_guard guard(host.lock);
        host.files["/good.crl"]=makeCrl(ca.get(),caKey.get(),99,86400,goodUrl);
        host.files["/revoked.crl"]=makeCrl(ca.get(),caKey.get(),11,86400,revokedUrl);
    }
    writePem(root/"ca.pem",{ca.get()},nullptr);trust(root/"ca.pem");
    writePem(root/"leaf-key.pem",{},leafKey.get());
    auto serve=[&](X509* leaf,const char* name) {
        writePem(root/name,{leaf,ca.get()},nullptr);
        return std::make_unique<Echo>(TlsContext::server((root/name).string(),(root/"leaf-key.pem").string()));
    };
    {
        auto server=serve(good.get(),"good.pem");
        Conn first;check(first.connect("tls://localhost",server->port),"unrevoked certificate rejected");
        Conn second;check(second.connect("tls://localhost",server->port),"cached CRL rejected certificate");
        check(host.count("/good.crl")==1,"CRL not fetched once and cached");
    }
    {
        auto server=serve(revoked.get(),"revoked.pem");Conn conn;
        check(!conn.connect("tls://localhost",server->port) && conn.error().find("revoked")!=std::string::npos,"revoked certificate accepted");
    }
    {
        auto server=serve(dead.get(),"dead.pem");Conn conn;
        check(conn.connect("tls://localhost",server->port),"unreachable CRL host must soft-fail");
    }
    {
        auto server=serve(plain.get(),"plain.pem");Conn conn;
        check(conn.connect("tls://localhost",server->port),"certificate without CRL distribution point rejected");
    }
}
int main(int argc,char** argv) {
    if(argc==3 && std::string(argv[1])=="--fixtures") {
        const std::filesystem::path root=argv[2];std::filesystem::create_directories(root);
        certificate(root/"cert.pem",root/"key.pem",false);return 0;
    }
    const auto root=std::filesystem::temp_directory_path()/("tak-tls-"+tak::crypto::toHex(tak::crypto::randomVec(8)));
    std::filesystem::create_directories(root);
    int result=0;
    try {
        const auto cert=root/"cert.pem",key=root/"key.pem";
        certificate(cert,key,false);trust(cert);
        {
            Echo server(TlsContext::server(cert.string(),key.string()));
            Conn good;check(good.connect("tls://localhost",server.port),"trusted hostname rejected");
            auto read=[](const auto& path) {std::ifstream in(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
            const auto bundle=read(cert)+read(key);
            server.setActive(TlsContext::serverPem(bundle,bundle));
            // An established connection must survive a renewal context swap;
            // later connections below also exercise the new in-memory context.
            const std::vector<uint8_t> payload(65536,0x5a);
            for(int i=0;i<64;++i)good.send(Msg::Chat,payload);
            int received=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(received<64 && std::chrono::steady_clock::now()<deadline) {
                check(good.flushWrite() && good.recv(),"TLS transfer failed");Frame frame;
                while(good.poll(frame)) {check(frame.kind==Msg::Chat && frame.payload==payload,"TLS payload corrupted");++received;}
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(received==64,"TLS multi-frame transfer stalled");
            good.closeNow(); // no close-notify: must not kill the process on a subsequent write
            Conn wrong;check(!wrong.connect("tls://127.0.0.1",server.port),"wrong hostname accepted");
            trust(root/"missing.pem");Conn untrusted;
            check(!untrusted.connect("tls://localhost",server.port),"untrusted certificate accepted");
            trust(cert);
            Conn plain;check(plain.connect("127.0.0.1",server.port),"plain TCP connect");plain.send(Msg::Hello);plain.flushWrite();
            for(int i=0;i<1000 && plain.ok() && !plain.peerClosed();++i) {plain.recv();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            check(!plain.ok() || plain.peerClosed(),"plaintext accepted by TLS listener");
            Conn healthy;check(healthy.connect("tls://localhost",server.port),"bad peers damaged TLS listener");
        }
        certificate(cert,key,true);trust(cert);
        {Echo server(TlsContext::server(cert.string(),key.string()));Conn expired;
         check(!expired.connect("tls://localhost",server.port),"expired certificate accepted");}
        revocationChecks(root);
        std::puts("PASS: verified TLS, framing/backpressure, wrong hostname, untrusted/expired certificate, plaintext rejection, abrupt close and CRL revocation (good, revoked, cached, soft-fail)");
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());result=1;}
    std::filesystem::remove_all(root);return result;
}
