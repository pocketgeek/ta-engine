#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "net/netcompat.h"
#include "server/acme.h"
#include "server/acme_internal.h"
#include "net/conn.h"
#include "net/revocation.h"
#include "server/filelease.h"
#include "vendor/nlohmann/json.hpp"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/core_names.h>
#include <openssl/rand.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace tak::srv {
namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
// Keep the macOS 14 SDK baseline: no experimental libc++ jthread/stop_token.
struct Stop {
    const std::atomic<bool>* flag=nullptr;
    bool stop_requested() const {return flag && flag->load();}
};
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using Bio=std::unique_ptr<BIO,decltype(&BIO_free)>;
constexpr size_t maxBody=1024*1024;
[[noreturn]] void fail(const std::string& reason) {throw std::runtime_error("ACME: "+reason);}
void require(bool ok,const char* reason) {if(!ok)fail(reason);}
std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)return {};
    std::string data;char buf[4096];
    while(in.read(buf,sizeof buf) || in.gcount()) {
        data.append(buf,size_t(in.gcount()));require(data.size()<=maxBody,"state file too large");
    }
    return data;
}
void save(const std::filesystem::path& path,const std::string& data) {
    // One atomic bundle holds both certificate and key: crashes cannot publish
    // a certificate with yesterday's key. State is in a private leased directory.
    auto temporary=path;temporary+=".tmp";
#ifdef _WIN32
    HANDLE f=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(f!=INVALID_HANDLE_VALUE,"cannot create state file");DWORD written=0;
    bool ok=WriteFile(f,data.data(),DWORD(data.size()),&written,nullptr) && written==data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    require(ok && MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"cannot persist state");
#else
    int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    require(fd>=0,"cannot create state file");
    size_t done=0;bool ok=true;
    while(done<data.size()) {const auto n=::write(fd,data.data()+done,data.size()-done);if(n<0 && errno==EINTR)continue;if(n<=0){ok=false;break;}done+=size_t(n);}
    ok=(::fsync(fd)==0)&&ok;ok=(::close(fd)==0)&&ok;
    require(ok && ::rename(temporary.c_str(),path.c_str())==0,"cannot persist state");
    int directory=::open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY);
    if(directory>=0) {::fsync(directory);::close(directory);}
#endif
}
std::string bioText(BIO* bio) {char* p=nullptr;const long n=BIO_get_mem_data(bio,&p);return std::string(p,size_t(n));}
std::string b64(const unsigned char* bytes,size_t n) {
    std::string out(4*((n+2)/3),'\0');
    if(n)EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),bytes,int(n));
    while(!out.empty() && out.back()=='=')out.pop_back();
    for(char& c:out) {if(c=='+')c='-';else if(c=='/')c='_';}return out;
}
std::string b64(const std::string& text) {return b64(reinterpret_cast<const unsigned char*>(text.data()),text.size());}
Key newKey() {return Key(EVP_RSA_gen(2048),EVP_PKEY_free);}
std::string keyPem(EVP_PKEY* key) {Bio b(BIO_new(BIO_s_mem()),BIO_free);require(b && PEM_write_bio_PrivateKey(b.get(),key,nullptr,nullptr,0,nullptr,nullptr)==1,"key serialization failed");return bioText(b.get());}
Key parseKey(const std::string& pem) {Bio b(BIO_new_mem_buf(pem.data(),int(pem.size())),BIO_free);return Key(PEM_read_bio_PrivateKey(b.get(),nullptr,nullptr,nullptr),EVP_PKEY_free);}
std::string bn(EVP_PKEY* key,const char* name) {
    BIGNUM* n=nullptr;require(EVP_PKEY_get_bn_param(key,name,&n)==1,"RSA parameter failed");
    std::vector<unsigned char> bytes(size_t(BN_num_bytes(n)));BN_bn2bin(n,bytes.data());BN_free(n);return b64(bytes.data(),bytes.size());
}
Json jwk(EVP_PKEY* key) {return Json{{"e",bn(key,OSSL_PKEY_PARAM_RSA_E)},{"kty","RSA"},{"n",bn(key,OSSL_PKEY_PARAM_RSA_N)}};}
std::string sign(EVP_PKEY* key,const std::string& data) {
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    require(ctx && EVP_DigestSignInit(ctx.get(),nullptr,EVP_sha256(),nullptr,key)==1,"sign initialization failed");
    size_t n=0;require(EVP_DigestSign(ctx.get(),nullptr,&n,reinterpret_cast<const unsigned char*>(data.data()),data.size())==1,"signature sizing failed");
    std::vector<unsigned char> bytes(n);require(EVP_DigestSign(ctx.get(),bytes.data(),&n,reinterpret_cast<const unsigned char*>(data.data()),data.size())==1,"signature failed");return b64(bytes.data(),n);
}
std::string csr(EVP_PKEY* key,const std::string& domain) {
    std::unique_ptr<X509_REQ,decltype(&X509_REQ_free)> req(X509_REQ_new(),X509_REQ_free);
    require(req && X509_REQ_set_version(req.get(),0)==1 && X509_REQ_set_pubkey(req.get(),key)==1,"CSR initialization failed");
    require(domain.size()>64 || X509_NAME_add_entry_by_txt(X509_REQ_get_subject_name(req.get()),"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>(domain.data()),int(domain.size()),-1,0)==1,"CSR subject failed");
    auto* ext=X509V3_EXT_conf_nid(nullptr,nullptr,NID_subject_alt_name,("DNS:"+domain).c_str());
    require(ext!=nullptr,"CSR SAN failed");auto* extensions=sk_X509_EXTENSION_new_null();
    require(extensions!=nullptr,"CSR extensions failed");sk_X509_EXTENSION_push(extensions,ext);
    const int added=X509_REQ_add_extensions(req.get(),extensions);sk_X509_EXTENSION_pop_free(extensions,X509_EXTENSION_free);
    require(added==1 && X509_REQ_sign(req.get(),key,EVP_sha256())>0,"CSR signing failed");
    const int n=i2d_X509_REQ(req.get(),nullptr);require(n>0,"CSR encoding failed");
    std::vector<unsigned char> der(size_t(n),0);auto* p=der.data();i2d_X509_REQ(req.get(),&p);return b64(der.data(),der.size());
}
struct Url {std::string origin,host,path;uint16_t port=443;};
Url url(const std::string& text) {
    require(text.starts_with("https://") && text.find_first_of("\r\n\t #@") == text.npos,"invalid HTTPS endpoint");
    const auto slash=text.find('/',8);Url u;u.origin=text.substr(0,slash);u.path=slash==text.npos?"/":text.substr(slash);u.host=u.origin.substr(8);
    const auto colon=u.host.find(':');
    if(colon!=u.host.npos) {unsigned port=0;auto p=std::string_view(u.host).substr(colon+1);auto r=std::from_chars(p.data(),p.data()+p.size(),port);require(r.ec==std::errc{} && r.ptr==p.data()+p.size() && port>0 && port<=65535,"invalid HTTPS port");u.port=uint16_t(port);u.host.resize(colon);}
    require(!u.host.empty(),"missing HTTPS hostname");return u;
}
struct Socket {int fd=-1;~Socket(){if(fd>=0)tak::net::sockClose(fd);}};
void waitSocket(int fd,short events,Clock::time_point deadline,Stop stop) {
    for(;;) {
        require(!stop.stop_requested(),"cancelled");require(Clock::now()<deadline,"network timeout");
        pollfd p{};p.fd=fd;p.events=events;const int n=TAK_POLL(&p,1,100);
        if(n>0)return;
        if(n<0 && !tak::net::sockInterrupted(tak::net::sockErr()))fail("socket poll failed");
    }
}
struct Response {int status=0;std::map<std::string,std::string> headers;std::string body;};
Response https(const std::string& endpoint,const std::string& method,const std::string& body,Stop stop) {
    using namespace tak::net;const auto u=url(endpoint);netStartup();Socket socket;
    addrinfo hints{},*addresses=nullptr;hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_UNSPEC;
    require(getaddrinfo(u.host.c_str(),std::to_string(u.port).c_str(),&hints,&addresses)==0,"cannot resolve CA");
    std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owned(addresses,freeaddrinfo);
    const auto deadline=Clock::now()+std::chrono::seconds(30);
    for(auto* a=addresses;a;a=a->ai_next) {
        int fd=int(::socket(a->ai_family,a->ai_socktype,a->ai_protocol));if(fd<0)continue;
        sockSetNonBlock(fd);
        int result=::connect(fd,a->ai_addr,socklen_t(a->ai_addrlen));
        if(result && !sockWouldBlock(sockErr()) && !sockInProgress(sockErr())) {sockClose(fd);continue;}
        socket.fd=fd;
        try {waitSocket(fd,POLLOUT,std::min(deadline,Clock::now()+std::chrono::seconds(5)),stop);}
        catch(const std::exception&) {sockClose(fd);socket.fd=-1;if(stop.stop_requested())throw;continue;}
        int error=0;socklen_t len=sizeof error;
        if(getsockopt(fd,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len)==0 && !error)break;
        sockClose(fd);socket.fd=-1;
    }
    require(socket.fd>=0,"cannot connect to CA");
#ifdef SO_NOSIGPIPE
    int one=1;setsockopt(socket.fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
#endif
    auto context=TlsContext::client();std::unique_ptr<SSL,decltype(&SSL_free)> ssl(SSL_new(context->handle),SSL_free);
    require(ssl && attachTlsSocket(ssl.get(),socket.fd) && SSL_set1_host(ssl.get(),u.host.c_str())==1 && SSL_set_tlsext_host_name(ssl.get(),u.host.c_str())==1,"CA TLS setup failed");
    auto retry=[&](int n) {
        int e=SSL_get_error(ssl.get(),n);
        if(e!=SSL_ERROR_WANT_READ && e!=SSL_ERROR_WANT_WRITE) {
            const auto verified=SSL_get_verify_result(ssl.get());
            if(verified!=X509_V_OK)fail(std::string("CA certificate verification failed: ")+X509_verify_cert_error_string(verified));
            fail("CA TLS request failed");
        }
        waitSocket(socket.fd,e==SSL_ERROR_WANT_READ?POLLIN:POLLOUT,deadline,stop);
    };
    for(;;) {int n=SSL_connect(ssl.get());if(n==1)break;retry(n);}
    require(tak::net::checkPeerRevocation(ssl.get()).state!=tak::net::Revocation::Revoked,"CA certificate revoked");
    const std::string request=method+" "+u.path+" HTTP/1.1\r\nHost: "+u.origin.substr(8)+"\r\nUser-Agent: TAK-ACME/1\r\nConnection: close\r\nAccept: application/json, application/pem-certificate-chain\r\n"+(method=="POST"?"Content-Type: application/jose+json\r\nContent-Length: "+std::to_string(body.size())+"\r\n":"")+"\r\n"+body;
    for(size_t sent=0;sent<request.size();) {int n=SSL_write(ssl.get(),request.data()+sent,int(request.size()-sent));if(n>0)sent+=size_t(n);else retry(n);}
    std::string received;char buf[8192];
    for(;;) {
        int n=SSL_read(ssl.get(),buf,sizeof buf);
        if(n>0) {received.append(buf,size_t(n));require(received.size()<=maxBody+32768,"CA response too large");}
        else {const int e=SSL_get_error(ssl.get(),n);if(e==SSL_ERROR_ZERO_RETURN)break;retry(n);}
    }
    const auto split=received.find("\r\n\r\n");require(split!=received.npos && split<=32768,"invalid CA HTTP headers");
    Response response;require(received.starts_with("HTTP/1.1 ") || received.starts_with("HTTP/1.0 "),"invalid CA HTTP response");
    auto code=std::string_view(received).substr(9,3);auto parsed=std::from_chars(code.data(),code.data()+code.size(),response.status);require(parsed.ec==std::errc{} && parsed.ptr==code.data()+3,"invalid HTTP status");
    size_t pos=received.find("\r\n")+2;
    while(pos<split) {
        const auto end=received.find("\r\n",pos);const auto colon=received.find(':',pos);require(colon<end,"invalid HTTP header");
        std::string name=received.substr(pos,colon-pos);for(char& c:name)c=char(std::tolower(static_cast<unsigned char>(c)));
        auto start=received.find_first_not_of(" \t",colon+1);std::string value=start<end?received.substr(start,end-start):"";
        if(name=="content-length" || name=="transfer-encoding")require(!response.headers.count(name),"duplicate HTTP framing header");
        response.headers[name]=value;pos=end+2;
    }
    response.body=received.substr(split+4);
    if(method=="HEAD") {response.body.clear();return response;}
    if(response.headers.count("transfer-encoding")) {
        require(response.headers["transfer-encoding"]=="chunked" && !response.headers.count("content-length"),"unsupported HTTP framing");
        std::string decoded;size_t offset=0;
        for(;;) {
            auto end=response.body.find("\r\n",offset);require(end!=std::string::npos,"invalid HTTP chunk");
            auto size=std::string_view(response.body).substr(offset,end-offset);size=size.substr(0,size.find(';'));
            size_t n=0;auto r=std::from_chars(size.data(),size.data()+size.size(),n,16);require(r.ec==std::errc{} && r.ptr==size.data()+size.size() && n<=maxBody,"invalid chunk length");
            offset=end+2;if(n==0)break;
            require(offset+n+2<=response.body.size() && response.body.substr(offset+n,2)=="\r\n","truncated HTTP chunk");decoded.append(response.body,offset,n);offset+=n+2;
        }
        response.body=std::move(decoded);
    } else if(response.headers.count("content-length")) {
        const auto& text=response.headers["content-length"];size_t n=0;auto r=std::from_chars(text.data(),text.data()+text.size(),n);
        require(r.ec==std::errc{} && r.ptr==text.data()+text.size() && n==response.body.size(),"truncated HTTP body");
    }
    require(response.body.size()<=maxBody,"CA response too large");return response;
}
Json json(const std::string& text) {
    return Json::parse(text,[](int depth,Json::parse_event_t,Json&) {require(depth<32,"JSON nesting limit");return true;});
}
bool dnsName(const std::string& name) {
    if(name.empty() || name.size()>253 || name.find('.')==name.npos)return false;
    size_t label=0;
    for(size_t i=0;i<name.size();++i) {
        const char c=name[i];
        if(c=='.') {if(!label || name[i-1]=='-')return false;label=0;continue;}
        if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') || (!label && c=='-') || ++label>63)return false;
    }
    return label && name.back()!='-' && name.find_first_not_of("0123456789.")!=name.npos;
}
class ChallengeResponder {
    Socket listener_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
public:
    ChallengeResponder(uint16_t port,std::string token,std::string authorization) {
        require(token.size()>=22 && token.size()<=256 && token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")==token.npos,"invalid challenge token");
        std::string error;listener_.fd=tak::net::listenOn(port,error);
        require(listener_.fd>=0,("cannot listen for HTTP-01 on port "+std::to_string(port)+": "+error).c_str());
        std::fprintf(stderr,"ACME: HTTP-01 listening on port %u for validation only\n",port);
        const int fd=listener_.fd;
        worker_=std::thread([this,fd,path="/.well-known/acme-challenge/"+token,authorization=std::move(authorization)] {
            const Stop stop{&stopping_};
            struct Peer {int fd;std::string source,input,output;size_t sent=0;Clock::time_point deadline;};
            std::vector<Peer> peers;
            while(!stop.stop_requested()) {
                for(unsigned count=0;count<32;++count) {
                    int client=int(::accept(fd,nullptr,nullptr));if(client<0)break;
                    const auto source=tak::net::peerAddress(client);
                    if(peers.size()>=32 || std::count_if(peers.begin(),peers.end(),[&](const Peer& peer){return peer.source==source;})>=4) {tak::net::sockClose(client);continue;}
                    tak::net::sockSetNonBlock(client);
#ifdef SO_NOSIGPIPE
                    int one=1;setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
#endif
                    peers.push_back({client,source,{},{},0,Clock::now()+std::chrono::seconds(3)});
                }
                for(auto it=peers.begin();it!=peers.end();) {
                    bool close=Clock::now()>=it->deadline;char bytes[2048];
                    if(!close && it->output.empty()) {
                        int n=int(::recv(it->fd,bytes,sizeof bytes,0));
                        if(n>0) {
                            it->input.append(bytes,size_t(n));
                            if(it->input.size()>8192)close=true;
                            else if(it->input.find("\r\n\r\n")!=it->input.npos) {
                                const auto line=it->input.substr(0,it->input.find("\r\n"));
                                const bool found=line=="GET "+path+" HTTP/1.1" || line=="GET "+path+" HTTP/1.0";
                                const auto body=found?authorization:std::string("Not found\n");
                                it->output="HTTP/1.1 "+std::string(found?"200 OK":"404 Not Found")+"\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
                            }
                        } else if(n==0 || (!tak::net::sockWouldBlock(tak::net::sockErr()) && !tak::net::sockInterrupted(tak::net::sockErr())))close=true;
                    }
                    if(!close && !it->output.empty()) {
                        int n=int(::send(it->fd,it->output.data()+it->sent,int(it->output.size()-it->sent),MSG_NOSIGNAL));
                        if(n>0)it->sent+=size_t(n);
                        else if(!tak::net::sockWouldBlock(tak::net::sockErr()) && !tak::net::sockInterrupted(tak::net::sockErr()))close=true;
                        if(it->sent==it->output.size())close=true;
                    }
                    if(close) {tak::net::sockClose(it->fd);it=peers.erase(it);}else ++it;
                }
                pollfd p{};p.fd=fd;p.events=POLLIN;TAK_POLL(&p,1,10);
            }
            for(const auto& peer:peers)tak::net::sockClose(peer.fd);
        });
    }
    ~ChallengeResponder() {stopping_=true;if(worker_.joinable())worker_.join();std::fprintf(stderr,"ACME: HTTP-01 validation finished; closing listener\n");}
};
struct RetryError:std::runtime_error {long seconds;RetryError(std::string text,long delay):runtime_error(std::move(text)),seconds(delay){}};
long retryAfter(const Response& r,long fallback) {
    auto i=r.headers.find("retry-after");if(i==r.headers.end())return fallback;
    long seconds=0;auto parsed=std::from_chars(i->second.data(),i->second.data()+i->second.size(),seconds);
    if(parsed.ec==std::errc{} && parsed.ptr==i->second.data()+i->second.size())return std::clamp(seconds,1L,7L*86400);
    // HTTP dates are GMT. Never interpret them in the server's local timezone.
    std::tm tm{};std::istringstream date(i->second);date.imbue(std::locale::classic());date>>std::get_time(&tm,"%a, %d %b %Y %H:%M:%S GMT");
    if(!date.fail()) {
#ifdef _WIN32
        const auto epoch=_mkgmtime(&tm);
#else
        const auto epoch=timegm(&tm);
#endif
        return std::clamp(long(epoch-std::time(nullptr)),1L,7L*86400);
    }
    return fallback;
}
class Client {
    AcmeOptions options_;
    Key account_{nullptr,EVP_PKEY_free};
    Json directory_;
    std::string origin_,nonce_,kid_,problem_;
    Stop stop_;
    Clock::time_point deadline_=Clock::now()+std::chrono::minutes(5);
    Response request(const std::string& endpoint,const std::string& method,const std::string& body) {
        require(url(endpoint).origin==origin_,"CA endpoint changed origin");
        require(Clock::now()<deadline_,"issuance deadline exceeded");
        auto response=https(endpoint,method,body,stop_);
        auto n=response.headers.find("replay-nonce");if(n!=response.headers.end())nonce_=n->second;
        return response;
    }
    void success(const Response& response) {
        if(response.status>=200 && response.status<300)return;
        try {problem_=json(response.body).value("type",std::string{});}catch(const std::exception&) {problem_.clear();}
        throw RetryError("ACME: CA rejected request (HTTP "+std::to_string(response.status)+")",retryAfter(response,60));
    }
    Response post(const std::string& endpoint,const std::string& payload,bool account=false) {
        for(unsigned attempt=0;attempt<16;++attempt) {
            if(nonce_.empty()) {auto response=request(directory_.at("newNonce"),"HEAD","");success(response);require(!nonce_.empty(),"CA did not supply a nonce");}
            Json protectedHeader={{"alg","RS256"},{"nonce",nonce_},{"url",endpoint}};nonce_.clear();
            if(account)protectedHeader["jwk"]=jwk(account_.get());else protectedHeader["kid"]=kid_;
            const auto header=b64(protectedHeader.dump()),encoded=b64(payload);
            auto response=request(endpoint,"POST",Json{{"protected",header},{"payload",encoded},{"signature",sign(account_.get(),header+"."+encoded)}}.dump());
            if(response.status==400 && json(response.body).value("type",std::string{})=="urn:ietf:params:acme:error:badNonce")continue;
            success(response);return response;
        }
        fail("CA repeatedly rejected fresh nonces");
    }
    void pause(const Response& response) {
        const long seconds=retryAfter(response,2);
        if(Clock::now()+std::chrono::seconds(seconds)>=deadline_)throw RetryError("ACME: CA requested a later retry",seconds);
        const auto until=Clock::now()+std::chrono::seconds(seconds);
        while(Clock::now()<until) {require(!stop_.stop_requested(),"cancelled");std::this_thread::sleep_for(std::chrono::milliseconds(100));}
    }
public:
    Client(AcmeOptions options,Stop stop):options_(std::move(options)),stop_(stop) {
        origin_=url(options_.directory).origin;
        const auto existing=readFile(options_.state/"account.pem");
        account_=existing.empty()?newKey():parseKey(existing);
        require(account_!=nullptr,"cannot load/create account key");
        if(existing.empty())save(options_.state/"account.pem",keyPem(account_.get()));
        auto response=request(options_.directory,"GET","");success(response);directory_=json(response.body);
        Json registration={{"termsOfServiceAgreed",options_.agreeTerms}};
        if(!options_.email.empty())registration["contact"]=Json::array({"mailto:"+options_.email});
        response=post(directory_.at("newAccount"),registration.dump(),true);
        kid_=response.headers["location"];require(!kid_.empty(),"CA omitted account URL");
        require(json(response.body).value("status",std::string{})=="valid","ACME account not valid");
    }
    // `replaces` is the RFC 9773 certID of the certificate this order renews, sent
    // only to a CA that advertises renewalInfo. A CA that has already seen it
    // replaced still issues an ordinary order.
    std::string issue(const std::string& replaces) {
        Json newOrder={{"identifiers",Json::array({Json{{"type","dns"},{"value",options_.domain}}})}};
        if(!replaces.empty() && directory_.contains("renewalInfo"))newOrder["replaces"]=replaces;
        Response response;
        try {response=post(directory_.at("newOrder"),newOrder.dump());}
        catch(const RetryError&) {
            if(!newOrder.contains("replaces") || problem_!="urn:ietf:params:acme:error:alreadyReplaced")throw;
            std::fprintf(stderr,"ACME: CA reports the certificate already replaced; ordering without replaces\n");
            newOrder.erase("replaces");response=post(directory_.at("newOrder"),newOrder.dump());
        }
        const auto orderUrl=response.headers["location"];require(!orderUrl.empty(),"CA omitted order URL");
        auto order=json(response.body);
        const auto authorizations=order.at("authorizations");require(authorizations.is_array() && authorizations.size()==1,"unexpected authorization count");
        for(const auto& authUrl:authorizations) {
            response=post(authUrl,"");auto auth=json(response.body);
            require(auth.at("identifier").at("type")=="dns" && auth.at("identifier").at("value")==options_.domain && !auth.value("wildcard",false),"CA authorization identifier mismatch");
            if(auth.value("status",std::string{})=="valid")continue;
            require(auth.value("status",std::string{})=="pending","authorization not pending");
            Json selected;
            for(const auto& challenge:auth.at("challenges"))if(challenge.value("type",std::string{})=="http-01") {selected=challenge;break;}
            require(!selected.is_null(),"CA does not offer HTTP-01");
            const std::string token=selected.at("token"),canonical=jwk(account_.get()).dump();
            unsigned char hash[32];unsigned n=0;require(EVP_Digest(canonical.data(),canonical.size(),hash,&n,EVP_sha256(),nullptr)==1 && n==32,"thumbprint failed");
            acme_detail::Challenge challenge(options_.challengePort,token,token+"."+b64(hash,32));
            post(selected.at("url"),"{}");
            for(;;) {
                response=post(authUrl,"");auth=json(response.body);const auto status=auth.value("status",std::string{});
                if(status=="valid")break;
                require(status=="pending","HTTP-01 validation failed; check DNS, IPv4/IPv6 and inbound port 80");pause(response);
            }
        } // challenge listener closes before CSR signing / certificate download
        for(;;) {
            response=post(orderUrl,"");order=json(response.body);const auto status=order.value("status",std::string{});
            if(status=="ready")break;
            require(status=="pending","order not ready");pause(response);
        }
        auto key=newKey();require(key!=nullptr,"certificate key creation failed");
        response=post(order.at("finalize"),Json{{"csr",csr(key.get(),options_.domain)}}.dump());order=json(response.body);
        for(;;) {
            const auto status=order.value("status",std::string{});if(status=="valid")break;
            require(status=="processing" || status=="ready","certificate order failed");pause(response);response=post(orderUrl,"");order=json(response.body);
        }
        response=post(order.at("certificate"),"");
        require(response.body.starts_with("-----BEGIN CERTIFICATE-----"),"CA did not return a certificate chain");
        return response.body+"\n"+keyPem(key.get());
    }
};
// Check the issued identity, key and validity before publication. The chain is
// obtained over authenticated CA HTTPS; clients independently verify its trust.
std::time_t renewalTime(const std::string& bundle,const std::string& domain) {
    Bio bio(BIO_new_mem_buf(bundle.data(),int(bundle.size())),BIO_free);
    std::unique_ptr<X509,decltype(&X509_free)> cert(PEM_read_bio_X509(bio.get(),nullptr,nullptr,nullptr),X509_free);
    auto key=parseKey(bundle);
    require(cert && key && X509_check_private_key(cert.get(),key.get())==1 && X509_check_host(cert.get(),domain.c_str(),domain.size(),X509_CHECK_FLAG_NO_WILDCARDS,nullptr)==1,"certificate identity/key mismatch");
    require(X509_cmp_current_time(X509_get0_notBefore(cert.get()))<0 && X509_cmp_current_time(X509_get0_notAfter(cert.get()))>0,"certificate not currently valid");
    int days=0,seconds=0;require(ASN1_TIME_diff(&days,&seconds,X509_get0_notBefore(cert.get()),X509_get0_notAfter(cert.get()))==1,"invalid certificate lifetime");
    const int64_t lifetime=int64_t(days)*86400+seconds;
    require(ASN1_TIME_diff(&days,&seconds,nullptr,X509_get0_notAfter(cert.get()))==1,"invalid certificate expiry");
    return std::time(nullptr)+int64_t(days)*86400+seconds-lifetime/(lifetime<10*86400?2:3);
}
using X509Ptr=std::unique_ptr<X509,decltype(&X509_free)>;
// The leaf and (when present) its issuer from a saved chain/key bundle.
std::pair<X509Ptr,X509Ptr> bundleChain(const std::string& bundle) {
    Bio bio(BIO_new_mem_buf(bundle.data(),int(bundle.size())),BIO_free);
    X509Ptr leaf(PEM_read_bio_X509(bio.get(),nullptr,nullptr,nullptr),X509_free);
    X509Ptr issuer(leaf?PEM_read_bio_X509(bio.get(),nullptr,nullptr,nullptr):nullptr,X509_free);
    ERR_clear_error();return {std::move(leaf),std::move(issuer)};
}
std::time_t expiry(X509* cert) {
    int days=0,seconds=0;require(ASN1_TIME_diff(&days,&seconds,nullptr,X509_get0_notAfter(cert))==1,"invalid certificate expiry");
    return std::time(nullptr)+int64_t(days)*86400+seconds;
}
std::string certId(X509* cert) {
    // RFC 9773: base64url(AKI keyIdentifier) "." base64url(DER INTEGER content of the serial).
    const auto* aki=X509_get0_authority_key_id(cert);
    if(!aki || ASN1_STRING_length(aki)<=0)return {};
    const int n=i2d_ASN1_INTEGER(X509_get0_serialNumber(cert),nullptr);if(n<3)return {};
    std::vector<unsigned char> der(static_cast<size_t>(n));auto* p=der.data();i2d_ASN1_INTEGER(X509_get0_serialNumber(cert),&p);
    const size_t header=der[1]<0x80?2:2+size_t(der[1]&0x7f);if(header>=der.size())return {};
    return b64(ASN1_STRING_get0_data(aki),size_t(ASN1_STRING_length(aki)))+"."+b64(der.data()+header,der.size()-header);
}
int64_t civilDays(int64_t y,unsigned m,unsigned d) {
    y-=m<=2;const int64_t era=(y>=0?y:y-399)/400;const unsigned yoe=unsigned(y-era*400);
    const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1,doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+int64_t(doe)-719468;
}
std::optional<std::time_t> rfc3339(const std::string& text) {
    // YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM); fractions are truncated.
    auto digits=[&](size_t at,size_t count,unsigned& out) {
        if(at+count>text.size())return false;
        out=0;
        for(size_t i=at;i<at+count;++i) {if(text[i]<'0' || text[i]>'9')return false;out=out*10+unsigned(text[i]-'0');}
        return true;
    };
    unsigned y,mo,d,h,mi,sec;
    if(text.size()<20 || !digits(0,4,y) || text[4]!='-' || !digits(5,2,mo) || text[7]!='-' || !digits(8,2,d) ||
       (text[10]!='T' && text[10]!='t') || !digits(11,2,h) || text[13]!=':' || !digits(14,2,mi) || text[16]!=':' || !digits(17,2,sec) ||
       mo<1 || mo>12 || d<1 || d>31 || h>23 || mi>59 || sec>60)return std::nullopt;
    size_t at=19;
    if(text[at]=='.') {++at;const size_t first=at;while(at<text.size() && text[at]>='0' && text[at]<='9')++at;if(at==first)return std::nullopt;}
    int64_t offset=0;
    if(at<text.size() && (text[at]=='Z' || text[at]=='z'))++at;
    else if(at<text.size() && (text[at]=='+' || text[at]=='-')) {
        unsigned oh,om;if(!digits(at+1,2,oh) || text[at+3]!=':' || !digits(at+4,2,om) || oh>23 || om>59)return std::nullopt;
        offset=(text[at]=='+'?1:-1)*int64_t(oh*3600+om*60);at+=6;
    } else return std::nullopt;
    if(at!=text.size())return std::nullopt;
    return std::time_t(civilDays(y,mo,d)*86400+h*3600+mi*60+std::min(sec,59u)-offset);
}
// RFC 9773 section 4.2: renew at a uniformly chosen point of the suggested window,
// or at once when that point has passed. The point is derived from the certID, so
// it is stable across restarts and still spreads one CA's certificates evenly.
std::optional<std::time_t> ariTime(const std::string& body,const std::string& id,std::time_t notAfter,std::time_t now) {
    try {
        const auto info=json(body);const auto& window=info.at("suggestedWindow");
        const auto start=rfc3339(window.at("start").get<std::string>()),end=rfc3339(window.at("end").get<std::string>());
        if(!start || !end || *end<=*start || *start>=notAfter)return std::nullopt;
        unsigned char hash[32];unsigned n=0;
        require(EVP_Digest(id.data(),id.size(),hash,&n,EVP_sha256(),nullptr)==1 && n==32,"window digest failed");
        uint64_t seed=0;for(int i=0;i<8;++i)seed=seed<<8|hash[i];
        // A window running past expiry is cut at notAfter: never renew after it.
        const auto span=uint64_t(std::min(*end,notAfter)-*start);
        const auto chosen=*start+std::time_t(seed%(span+1));
        return std::max(chosen,now);
    }catch(const std::exception&) {return std::nullopt;}
}
struct RenewalInfo {bool advertised=false;Response response;};
RenewalInfo renewalInfo(const AcmeOptions& options,const std::string& id,Stop stop) {
    auto response=https(options.directory,"GET","",stop);
    require(response.status==200,"CA directory unavailable");
    const auto directory=json(response.body);
    RenewalInfo info;if(!directory.contains("renewalInfo") || !directory["renewalInfo"].is_string())return info;
    std::string base=directory["renewalInfo"];
    require(url(base).origin==url(options.directory).origin,"CA endpoint changed origin");
    if(base.ends_with('/'))base.pop_back();
    info.advertised=true;info.response=https(base+"/"+id,"GET","",stop);return info;
}
}
struct acme_detail::Challenge::Impl:ChallengeResponder {
    using ChallengeResponder::ChallengeResponder;
};
acme_detail::Challenge::Challenge(uint16_t port,std::string token,std::string authorization)
    :impl_(std::make_unique<Impl>(port,std::move(token),std::move(authorization))){}
acme_detail::Challenge::~Challenge()=default;
bool acme_detail::validDomain(const std::string& domain) {return dnsName(domain);}
std::string acme_detail::ariCertId(const std::string& pem) {auto chain=bundleChain(pem);return chain.first?certId(chain.first.get()):std::string{};}
std::optional<std::time_t> acme_detail::parseRfc3339(const std::string& text) {return rfc3339(text);}
std::optional<std::time_t> acme_detail::ariRenewalTime(const std::string& body,const std::string& id,std::time_t notAfter,std::time_t now) {return ariTime(body,id,notAfter,now);}
struct AcmeCertificates::Impl {
    AcmeOptions options;
    std::unique_ptr<FileLease> lease;
    // The live certificate: replaced by renewals, read by every new connection.
    mutable std::mutex currentMutex;
    std::shared_ptr<tak::net::TlsContext> current;
    void setCurrent(std::shared_ptr<tak::net::TlsContext> context) {std::lock_guard lock(currentMutex);current=std::move(context);}
    std::shared_ptr<tak::net::TlsContext> currentContext() const {std::lock_guard lock(currentMutex);return current;}
    std::time_t renewAt=0,nextAttempt=0;
    unsigned failures=0;
    // Renewal information (RFC 9773) for the current certificate. renewAt holds the
    // fixed one-third-of-lifetime rule until the CA suggests a window.
    std::string bundle,replaces,certificateId;
    std::time_t fixedRenewAt=0,ariPollAt=0,ariChoice=0;
    std::atomic<bool> stopping{false};
    std::thread worker;
    ~Impl() {stopping=true;if(worker.joinable())worker.join();}
    explicit Impl(AcmeOptions value):options(std::move(value)) {
        require(options.agreeTerms,"--acme-agree-tos is required");
        require(dnsName(options.domain),"use one lowercase public DNS hostname (no wildcard, IP or scheme)");
        require(options.email.size()<255 && options.email.find_first_of("\r\n") == options.email.npos,"invalid account email");
        require(!options.state.empty(),"ACME state directory required");url(options.directory);
        std::filesystem::create_directories(options.state);
#ifdef _WIN32
        HANDLE token=nullptr;require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token),"cannot inspect state owner");
        DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
        std::vector<unsigned char> storage(size);
        const bool found=GetTokenInformation(token,TokenUser,storage.data(),size,&size);CloseHandle(token);
        require(found,"cannot inspect state owner");
        EXPLICIT_ACCESSW access{};access.grfAccessPermissions=FILE_ALL_ACCESS;access.grfAccessMode=SET_ACCESS;
        access.grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm=TRUSTEE_IS_SID;access.Trustee.TrusteeType=TRUSTEE_IS_USER;
        access.Trustee.ptstrName=reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid);
        PACL acl=nullptr;require(SetEntriesInAclW(1,&access,nullptr,&acl)==ERROR_SUCCESS,"cannot protect state directory");
        auto path=options.state.wstring();const auto result=SetNamedSecurityInfoW(path.data(),SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);LocalFree(acl);
        require(result==ERROR_SUCCESS,"cannot protect state directory");
#else
        require(::chmod(options.state.c_str(),0700)==0,"cannot protect state directory");
#endif
        lease=std::make_unique<FileLease>(options.state/"acme");
        const auto identity=Json{{"directory",options.directory},{"domain",options.domain}}.dump();
        auto saved=readFile(options.state/"identity.json");
        require(saved.empty() || saved==identity,"state directory belongs to another hostname or CA");
        if(saved.empty())save(options.state/"identity.json",identity);
        auto retry=readFile(options.state/"retry.json");if(!retry.empty()) {auto r=json(retry);nextAttempt=r.value("next",std::time_t{});failures=std::min(r.value("failures",0u),4u);}
        const auto stored=readFile(options.state/"current.pem");
        if(!stored.empty()) {
            try {renewAt=renewalTime(stored,options.domain);setCurrent(tak::net::TlsContext::serverPem(stored,stored));adopt(stored,renewAt);}
            catch(const std::exception& e) {std::fprintf(stderr,"ACME: stored certificate unavailable: %s\n",e.what());}
        }
        // Initial issuance uses the same persistent, interruptible retry loop as
        // renewal. A CA backoff must not become a systemd restart storm.
        worker=std::thread([this] {
            const Stop stop{&stopping};
            while(!stop.stop_requested()) {
                if(!bundle.empty() && std::time(nullptr)>=ariPollAt)pollRenewal(stop);
                const auto now=std::time(nullptr);
                if(!stop.stop_requested() && now>=renewAt && now>=nextAttempt) {
                    try {issue(stop);}catch(const std::exception& e) {if(!stop.stop_requested()) {try {recordFailure(e);}catch(const std::exception& stateError) {std::fprintf(stderr,"ACME: cannot save retry state: %s\n",stateError.what());}}}
                }
                for(int i=0;i<100 && !stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    void issue(Stop stop) {
        std::fprintf(stderr,"ACME: obtaining certificate for %s\n",options.domain.c_str());
        Client client(options,stop);auto issued=client.issue(replaces);const auto renewal=renewalTime(issued,options.domain);
        require(renewal>std::time(nullptr),"issued certificate already due for renewal");
        auto context=tak::net::TlsContext::serverPem(issued,issued);
        save(options.state/"current.pem",issued);
        setCurrent(std::move(context));renewAt=renewal;nextAttempt=0;failures=0;adopt(issued,renewal);
        save(options.state/"retry.json",Json{{"next",0},{"failures",0}}.dump());
        std::fprintf(stderr,"ACME: certificate installed; existing game connections retained\n");
    }
    void adopt(const std::string& pem,std::time_t fixed) {
        bundle=pem;fixedRenewAt=fixed;replaces.clear();ariPollAt=0;ariChoice=0;
        auto chain=bundleChain(pem);certificateId=chain.first?certId(chain.first.get()):std::string{};
    }
    // Polls the CA's renewalInfo for the current certificate and checks the
    // certificate's own CRL. A CA without renewalInfo keeps the fixed rule; a failed
    // poll keeps the last schedule and retries within an hour. Retry-After is
    // honoured between one minute and one day (RFC 9773 section 4.3).
    void pollRenewal(Stop stop) {
        const auto now=std::time(nullptr);ariPollAt=now+6*3600;
        auto chain=bundleChain(bundle);if(!chain.first)return;
        if(!certificateId.empty()) {
            try {
                auto info=renewalInfo(options,certificateId,stop);
                if(!info.advertised) {renewAt=fixedRenewAt;replaces.clear();ariPollAt=now+86400;}
                else {
                    const auto chosen=info.response.status==200?ariTime(info.response.body,certificateId,expiry(chain.first.get()),now):std::nullopt;
                    ariPollAt=now+std::clamp(retryAfter(info.response,chosen?6*3600:3600),60L,86400L);
                    if(chosen) {
                        if(*chosen!=ariChoice) {
                            char when[32];std::tm tm{};const std::time_t value=*chosen;
#ifdef _WIN32
                            gmtime_s(&tm,&value);
#else
                            gmtime_r(&value,&tm);
#endif
                            std::strftime(when,sizeof when,"%Y-%m-%d %H:%M:%S UTC",&tm);
                            std::string why;try {why=json(info.response.body).value("explanationURL",std::string{});}catch(const std::exception&) {}
                            std::fprintf(stderr,"ACME: renewal information schedules renewal at %s%s%s\n",when,why.empty()?"":"; CA explanation: ",why.c_str());
                        }
                        // Only a certificate the CA described is named in `replaces`;
                        // a CA that does not know it (404) would reject the order.
                        ariChoice=*chosen;renewAt=*chosen;replaces=certificateId;
                    } else std::fprintf(stderr,"ACME: renewal information unusable (HTTP %d); keeping current schedule\n",info.response.status);
                }
            }catch(const std::exception& e) {
                if(stop.stop_requested())return;
                ariPollAt=now+3600;std::fprintf(stderr,"ACME: renewal information unavailable: %s\n",e.what());
            }
        }
        // The CA's renewal window covers revocation for ARI CAs; the CRL also covers others.
        if(!stop.stop_requested() && chain.second &&
           tak::net::checkRevocation(chain.first.get(),chain.second.get()).state==tak::net::Revocation::Revoked && renewAt>now) {
            std::fprintf(stderr,"ACME: current certificate is revoked; renewing now\n");renewAt=now;
        }
    }
    void recordFailure(const std::exception& error) {
        constexpr long delays[]={60,600,6000,86400};long delay=delays[std::min(failures++,3u)];
        if(auto* retry=dynamic_cast<const RetryError*>(&error))delay=std::max(delay,retry->seconds);
        unsigned char jitter=0;RAND_bytes(&jitter,1);delay+=delay*long(jitter)/2550;
        nextAttempt=std::time(nullptr)+delay;
        std::fprintf(stderr,"ACME: %s; retaining any existing certificate, retry in %ld seconds\n",error.what(),delay);
        save(options.state/"retry.json",Json{{"next",nextAttempt},{"failures",failures}}.dump());
    }
};
AcmeCertificates::AcmeCertificates(AcmeOptions options):impl_(std::make_unique<Impl>(std::move(options))){}
AcmeCertificates::~AcmeCertificates()=default;
std::shared_ptr<tak::net::TlsContext> AcmeCertificates::context() const {return impl_->currentContext();}
}
