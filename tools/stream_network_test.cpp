// Test-only RTMPS sink injection; never compiled into takclient.
#include "video/stream.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include "net/tls.h"
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <set>
#endif
namespace {
// --sink PORT SECONDS: a plain RTMP receiver built from the same static FFmpeg, so the
// upload tests need no ffmpeg program. Like a real ingest it never speaks mid-stream
// before its acknowledgement window fills (FFmpeg's own server, as `ffmpeg -listen 1`).
int sink(int port,int seconds){
    avformat_network_init();
    AVDictionary* o=nullptr;av_dict_set(&o,"listen","1",0);av_dict_set(&o,"timeout",std::to_string(seconds).c_str(),0);
    const std::string url="rtmp://127.0.0.1:"+std::to_string(port)+"/live/test-key";
    std::printf("SINK LISTENING\n");std::fflush(stdout);
    AVIOContext* pb=nullptr;
    const int opened=avio_open2(&pb,url.c_str(),AVIO_FLAG_READ,nullptr,&o);av_dict_free(&o);
    if(opened<0){std::printf("SINK OPEN FAILED %d\n",opened);return 1;}
    std::vector<unsigned char> buf(65536);
    unsigned long long bytes=0;bool flv=false;
    auto start=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)){
        const int n=avio_read(pb,buf.data(),int(buf.size()));
        if(n<=0)break;
        if(!bytes)flv=n>=3&&std::memcmp(buf.data(),"FLV",3)==0;
        bytes+=n;
    }
    avio_closep(&pb);
    std::printf("SINK BYTES %llu FLV %d\n",bytes,flv?1:0);std::fflush(stdout);
    return 0;
}
#ifdef __APPLE__
// --system-roots [FILE]: the trust anchors macOS streaming hands FFmpeg's OpenSSL TLS as its CA
// file (video/stream.cpp). Every certificate in that bundle must parse back (an anchor
// OpenSSL cannot decode is left out of it, as multiplayer TLS skips it), and they must include the roots
// YouTube's ingest certificates chain to (Google Trust Services, cross-signed by
// GlobalSign), or every real RTMPS stream fails as TLS CERTIFICATE NOT TRUSTED.
// With a FILE argument the bundle is also written there, for a manual check.
int systemRoots(const char* file){
    const std::string pem=tak::net::systemRootsPem();
    if(file){
        FILE* out=std::fopen(file,"wb");if(!out)return 1;
        const bool written=std::fwrite(pem.data(),1,pem.size(),out)==pem.size();
        if(std::fclose(out)!=0||!written)return 1;
    }
    BIO* in=BIO_new_mem_buf(pem.data(),int(pem.size()));
    if(!in)return 1;
    int count=0;std::set<std::string> names;
    while(X509* cert=PEM_read_bio_X509(in,nullptr,nullptr,nullptr)){
        ++count;char name[256];
        if(X509_NAME_get_text_by_NID(X509_get_subject_name(cert),NID_commonName,name,sizeof name)>0)names.insert(name);
        X509_free(cert);
    }
    BIO_free(in);
    const size_t blocks=[&]{size_t n=0;for(size_t at=0;(at=pem.find("-----BEGIN CERTIFICATE-----",at))!=std::string::npos;++at)++n;return n;}();
    std::printf("ROOTS %d of %zu\n",count,blocks);
    bool ok=count>=50&&size_t(count)==blocks;
    for(const char* want:{"GTS Root R1","GlobalSign Root CA"}){
        const bool has=names.count(want)>0;
        std::printf("ROOT %s %s\n",want,has?"present":"MISSING");ok=ok&&has;
    }
    return ok?0:1;
}
#endif
}
int main(int argc,char** argv){
    if(argc==4&&std::strcmp(argv[1],"--sink")==0)return sink(std::atoi(argv[2]),std::atoi(argv[3]));
#ifdef __APPLE__
    if((argc==2||argc==3)&&std::strcmp(argv[1],"--system-roots")==0)return systemRoots(argc==3?argv[2]:nullptr);
#endif
    // url ca-file seconds [encoder [local-copy-folder]]; ca-file "-" keeps the
    // production trust store (no test CA).
    if(argc<4 || argc>6)return 2;
    tak::video::Stream stream;tak::video::StreamConfig c;c.encoder=argc>4?argv[4]:"libx264";
    if(argc>5)c.localCopyDir=argv[5];
    if(!stream.startTestEndpoint(c,argv[1],std::strcmp(argv[2],"-")==0?"":argv[2]))return 3;
    std::vector<uint8_t> pixels(320*240*4,100);
    std::string previous;
    auto start=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-start<std::chrono::seconds(std::atoi(argv[3]))) {
        stream.video(pixels.data(),320,240,1280);
        auto s=stream.status();
        const std::string line=s.state+(s.local.empty()?"":" | "+s.local+" "+s.localFile);
        if(line!=previous){std::printf("%s frames=%llu\n",line.c_str(),(unsigned long long)s.frames);std::fflush(stdout);previous=line;}
        if(!s.active)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    const auto last=stream.status();
    std::printf("DONE frames=%llu bytes=%llu dropped=%llu send=%llu\n",(unsigned long long)last.frames,(unsigned long long)last.bytes,
                (unsigned long long)last.dropped,(unsigned long long)last.sendMicros);std::fflush(stdout);
    stream.stop();
    while(stream.active())std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto s=stream.status();
    if(!s.local.empty())std::printf("%s %s\n",s.local.c_str(),s.localFile.c_str());
}
