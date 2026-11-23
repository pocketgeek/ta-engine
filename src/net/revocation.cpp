#include "net/revocation.h"
#include "net/netcompat.h"
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>

namespace tak::net {
namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t maxCrl=10*1024*1024;
RevocationStatus status(Revocation state,std::string detail) {return {state,std::move(detail)};}
struct HttpUrl {std::string host,path;uint16_t port=80;};
bool parseUrl(const std::string& text,HttpUrl& out) {
    if(!text.starts_with("http://") || text.size()>1024 || text.find_first_of("\r\n\t #@?")!=text.npos)return false;
    const auto slash=text.find('/',7);if(slash==text.npos)return false;
    out.host=text.substr(7,slash-7);out.path=text.substr(slash);
    const auto colon=out.host.rfind(':');
    if(colon!=out.host.npos && out.host.find(']')==out.host.npos) {
        unsigned port=0;const auto p=std::string_view(out.host).substr(colon+1);
        const auto r=std::from_chars(p.data(),p.data()+p.size(),port);
        if(r.ec!=std::errc{} || r.ptr!=p.data()+p.size() || !port || port>65535)return false;
        out.port=uint16_t(port);out.host.resize(colon);
    }
    if(out.host.size()>2 && out.host.front()=='[' && out.host.back()==']')out.host=out.host.substr(1,out.host.size()-2);
    return !out.host.empty();
}
bool waitFor(int fd,short events,Clock::time_point deadline) {
    for(;;) {
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(left<=0)return false;
        pollfd p{};p.fd=fd;p.events=events;const int n=TAK_POLL(&p,1,int(std::min<long long>(left,100)));
        if(n>0)return true;
        if(n<0 && !sockInterrupted(sockErr()))return false;
    }
}
// CRLs are signed objects served over plain HTTP. An HTTP/1.0 request keeps the
// response framing to Content-Length or close-delimited; redirects are not followed.
bool fetch(const std::string& text,int timeoutMs,std::string& body,std::string& error) {
    HttpUrl u;if(!parseUrl(text,u)) {error="unsupported CRL URL";return false;}
    netStartup();const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
    addrinfo hints{},*addresses=nullptr;hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_UNSPEC;
    if(getaddrinfo(u.host.c_str(),std::to_string(u.port).c_str(),&hints,&addresses)!=0 || !addresses) {error="cannot resolve CRL host";return false;}
    std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owned(addresses,freeaddrinfo);
    int fd=-1;
    for(auto* a=addresses;a && fd<0;a=a->ai_next) {
        const int s=int(::socket(a->ai_family,a->ai_socktype,a->ai_protocol));if(s<0)continue;
        sockSetNonBlock(s);
        const int r=::connect(s,a->ai_addr,socklen_t(a->ai_addrlen));
        int e=0;socklen_t len=sizeof e;
        if(!r || ((sockWouldBlock(sockErr()) || sockInProgress(sockErr())) && waitFor(s,POLLOUT,deadline) &&
                  getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&e),&len)==0 && !e))fd=s;
        else sockClose(s);
    }
    if(fd<0) {error="cannot connect to CRL host";return false;}
    struct Close {int fd;~Close(){sockClose(fd);}} closer{fd};
#ifdef SO_NOSIGPIPE
    int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
#endif
    const std::string request="GET "+u.path+" HTTP/1.0\r\nHost: "+u.host+"\r\nUser-Agent: TAK-CRL/1\r\nAccept: application/pkix-crl\r\nConnection: close\r\n\r\n";
    for(size_t sent=0;sent<request.size();) {
        const int n=int(::send(fd,request.data()+sent,int(request.size()-sent),MSG_NOSIGNAL));
        if(n>0) {sent+=size_t(n);continue;}
        if(n<0 && (sockWouldBlock(sockErr()) || sockInterrupted(sockErr())) && waitFor(fd,POLLOUT,deadline))continue;
        error="CRL request failed";return false;
    }
    std::string received;char buf[16384];
    for(;;) {
        const int n=int(::recv(fd,buf,sizeof buf,0));
        if(n>0) {received.append(buf,size_t(n));if(received.size()>maxCrl+16384) {error="CRL too large";return false;}continue;}
        if(!n)break;
        if((sockWouldBlock(sockErr()) || sockInterrupted(sockErr())) && waitFor(fd,POLLIN,deadline))continue;
        error=Clock::now()>=deadline?"CRL fetch timed out":"CRL receive failed";return false;
    }
    const auto split=received.find("\r\n\r\n");
    if(split==received.npos || split>16384 || !(received.starts_with("HTTP/1.1 200") || received.starts_with("HTTP/1.0 200"))) {error="CRL server did not return HTTP 200";return false;}
    std::string headers=received.substr(0,split);
    for(char& c:headers)c=char(std::tolower(static_cast<unsigned char>(c)));
    if(headers.find("\r\ntransfer-encoding:")!=headers.npos) {error="unsupported CRL HTTP framing";return false;}
    body=received.substr(split+4);
    if(const auto at=headers.find("\r\ncontent-length:");at!=headers.npos) {
        auto value=std::string_view(headers).substr(at+17);value=value.substr(0,value.find("\r\n"));
        while(!value.empty() && (value.front()==' ' || value.front()=='\t'))value.remove_prefix(1);
        size_t n=0;const auto r=std::from_chars(value.data(),value.data()+value.size(),n);
        if(r.ec!=std::errc{} || n!=body.size()) {error="truncated CRL";return false;}
    }
    if(body.empty() || body.size()>maxCrl) {error="invalid CRL size";return false;}
    return true;
}
bool hasUri(GENERAL_NAMES* names,const std::string& url) {
    for(int i=0;names && i<sk_GENERAL_NAME_num(names);++i) {
        const auto* name=sk_GENERAL_NAME_value(names,i);
        if(name->type!=GEN_URI)continue;
        const auto* uri=name->d.uniformResourceIdentifier;
        if(std::string_view(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)),size_t(ASN1_STRING_length(uri)))==url)return true;
    }
    return false;
}
// A fetched CRL is reused until the earlier of its nextUpdate and six hours; a
// failed fetch is not retried for ten minutes, so reconnect loops do not stall.
struct Cached {std::string der,error;std::time_t until=0;};
std::mutex cacheMutex;
std::map<std::string,Cached> cache;
}
std::string crlUrl(X509* leaf) {
    std::unique_ptr<STACK_OF(DIST_POINT),void(*)(STACK_OF(DIST_POINT)*)> points(
        static_cast<STACK_OF(DIST_POINT)*>(X509_get_ext_d2i(leaf,NID_crl_distribution_points,nullptr,nullptr)),
        [](STACK_OF(DIST_POINT)* p){sk_DIST_POINT_pop_free(p,DIST_POINT_free);});
    for(int i=0;points && i<sk_DIST_POINT_num(points.get());++i) {
        const auto* point=sk_DIST_POINT_value(points.get(),i);
        // Indirect and reason-partitioned CRLs are outside this checker's scope.
        if(!point->distpoint || point->distpoint->type!=0 || point->CRLissuer || point->reasons)continue;
        auto* names=point->distpoint->name.fullname;
        for(int j=0;j<sk_GENERAL_NAME_num(names);++j) {
            const auto* name=sk_GENERAL_NAME_value(names,j);
            if(name->type!=GEN_URI)continue;
            const auto* uri=name->d.uniformResourceIdentifier;
            std::string text(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)),size_t(ASN1_STRING_length(uri)));
            if(text.starts_with("http://"))return text;
        }
    }
    return {};
}
RevocationStatus crlStatus(const std::string& der,X509* leaf,X509* issuer,const std::string& url,std::time_t now) {
    const auto* bytes=reinterpret_cast<const unsigned char*>(der.data());
    std::unique_ptr<X509_CRL,decltype(&X509_CRL_free)> crl(d2i_X509_CRL(nullptr,&bytes,long(der.size())),X509_CRL_free);
    if(!crl || bytes!=reinterpret_cast<const unsigned char*>(der.data())+der.size())return status(Revocation::Unavailable,"malformed CRL");
    if(X509_NAME_cmp(X509_CRL_get_issuer(crl.get()),X509_get_subject_name(issuer))!=0 ||
       X509_NAME_cmp(X509_get_issuer_name(leaf),X509_get_subject_name(issuer))!=0)
        return status(Revocation::Unavailable,"CRL issuer mismatch");
    if(!(X509_get_key_usage(issuer)&KU_CRL_SIGN))return status(Revocation::Unavailable,"issuer may not sign CRLs");
    if(X509_CRL_verify(crl.get(),X509_get0_pubkey(issuer))!=1)return status(Revocation::Unavailable,"CRL signature invalid");
    // Five minutes of clock skew either way; a CRL without nextUpdate cannot be fresh.
    std::time_t early=now+300,late=now-300;
    const auto* next=X509_CRL_get0_nextUpdate(crl.get());
    if(X509_cmp_time(X509_CRL_get0_lastUpdate(crl.get()),&early)!=-1 || !next || X509_cmp_time(next,&late)!=1)
        return status(Revocation::Unavailable,"CRL is not current");
    if(X509_CRL_get_ext_by_NID(crl.get(),NID_delta_crl,-1)>=0)return status(Revocation::Unavailable,"delta CRL");
    // The CRL must cover the distribution point we fetched and end-entity certificates.
    int critical=0;
    std::unique_ptr<ISSUING_DIST_POINT,decltype(&ISSUING_DIST_POINT_free)> scope(
        static_cast<ISSUING_DIST_POINT*>(X509_CRL_get_ext_d2i(crl.get(),NID_issuing_distribution_point,&critical,nullptr)),ISSUING_DIST_POINT_free);
    if(scope) {
        if(scope->onlyCA || scope->onlyattr || scope->onlysomereasons || scope->indirectCRL)return status(Revocation::Unavailable,"CRL scope excludes this certificate");
        if(scope->distpoint && (scope->distpoint->type!=0 || !hasUri(scope->distpoint->name.fullname,url)))
            return status(Revocation::Unavailable,"CRL distribution point mismatch");
    } else if(critical==-2)return status(Revocation::Unavailable,"duplicate CRL scope");
    X509_REVOKED* entry=nullptr;
    if(X509_CRL_get0_by_serial(crl.get(),&entry,X509_get0_serialNumber(leaf))==1)return status(Revocation::Revoked,"certificate is revoked");
    return status(Revocation::Good,{});
}
RevocationStatus checkRevocation(X509* leaf,X509* issuer,int timeoutMs) {
    if(!leaf || !issuer)return status(Revocation::NotChecked,"no issuer");
    const auto url=crlUrl(leaf);
    if(url.empty())return status(Revocation::NotChecked,"no CRL distribution point");
    const auto now=std::time(nullptr);
    std::string der,error;
    {
        std::lock_guard lock(cacheMutex);
        auto i=cache.find(url);
        if(i!=cache.end() && i->second.until>now) {der=i->second.der;error=i->second.error;}
    }
    if(der.empty() && error.empty()) {
        Cached entry;
        if(fetch(url,timeoutMs,entry.der,entry.error)) {
            const auto checked=crlStatus(entry.der,leaf,issuer,url,now);
            if(checked.state==Revocation::Unavailable) {entry.der.clear();entry.error=checked.detail;}
        }
        entry.until=now+(entry.der.empty()?600:6*3600);
        if(!entry.der.empty()) {
            const auto* bytes=reinterpret_cast<const unsigned char*>(entry.der.data());
            std::unique_ptr<X509_CRL,decltype(&X509_CRL_free)> crl(d2i_X509_CRL(nullptr,&bytes,long(entry.der.size())),X509_CRL_free);
            int days=0,seconds=0;
            if(crl && X509_CRL_get0_nextUpdate(crl.get()) && ASN1_TIME_diff(&days,&seconds,nullptr,X509_CRL_get0_nextUpdate(crl.get()))==1) {
                const std::time_t next=now+std::time_t(days)*86400+seconds;
                entry.until=std::min(entry.until,std::max(next,now+60));
            }
        }
        der=entry.der;error=entry.error;
        std::lock_guard lock(cacheMutex);
        if(cache.size()>=32)cache.clear();
        cache[url]=std::move(entry);
    }
    if(der.empty())return status(Revocation::Unavailable,url+": "+error);
    auto checked=crlStatus(der,leaf,issuer,url,now);
    if(!checked.detail.empty())checked.detail=url+": "+checked.detail;
    return checked;
}
RevocationStatus checkPeerRevocation(SSL* ssl,int timeoutMs) {
    auto* chain=SSL_get0_verified_chain(ssl);
    if(!chain || sk_X509_num(chain)<2)return status(Revocation::NotChecked,"self-issued peer");
    return checkRevocation(sk_X509_value(chain,0),sk_X509_value(chain,1),timeoutMs);
}
}
