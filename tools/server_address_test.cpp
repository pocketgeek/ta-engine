#include "client/serveraddress.h"
#include <cstdio>

int main() {
    int failed=0;
    auto check=[&](const char* input,const char* host,unsigned port,bool fallback) {
        const auto a=tak::parseServerAddress(input);
        if(!a || a->host!=host || a->port!=port || a->plainFallback!=fallback) {++failed;std::fprintf(stderr,"FAIL: %s\n",input);return;}
        const auto roundtrip=tak::parseServerAddress(tak::formatServerAddress(a->host,a->port));
        if(!roundtrip || roundtrip->host!=a->host || roundtrip->port!=a->port)++failed;
    };
    // No scheme: TLS first, then unencrypted (loopback included).
    check("tak.pgnet.us","tls://tak.pgnet.us",7677,true);
    check(" tak.pgnet.us \n","tls://tak.pgnet.us",7677,true);
    check("tak.pgnet.us:1234","tls://tak.pgnet.us",1234,true);
    check("192.0.2.1","tls://192.0.2.1",7677,true);
    check("[::1]","tls://::1",7677,true);
    check("::1","tls://::1",7677,true);
    check("127.0.0.1:7677","tls://127.0.0.1",7677,true);
    check("127.1.2.3","tls://127.1.2.3",7677,true);
    check("localhost","tls://localhost",7677,true);
    // A scheme is that transport only.
    check("tls://tak.pgnet.us:7677","tls://tak.pgnet.us",7677,false);
    check("tls://localhost","tls://localhost",7677,false);
    check("tls://127.0.0.1","tls://127.0.0.1",7677,false);
    check("tls://[::1]:1234","tls://::1",1234,false);
    check("tcp://[::1]:1234","::1",1234,false);
    check("tcp://localhost","localhost",7677,false);
    for(const char* input:{""," ","host:","host:0","host:65536","host:-1","host:123abc","[::1", "[::1]junk","tls://","https://host"})
        if(tak::parseServerAddress(input)) {++failed;std::fprintf(stderr,"accepted invalid: %s\n",input);}
    return failed?1:0;
}
