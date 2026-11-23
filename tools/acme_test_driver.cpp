#include "server/acme.h"
#include <chrono>
#include <cstdio>
#include <thread>
int main(int argc,char** argv) {
    if(argc!=6) {std::fprintf(stderr,"usage: acme_test_driver state domain directory http-port hold-seconds\n");return 2;}
    try {
        tak::srv::AcmeOptions options;options.state=argv[1];options.domain=argv[2];options.directory=argv[3];options.challengePort=uint16_t(std::stoi(argv[4]));options.agreeTerms=true;
        tak::srv::AcmeCertificates certificates(options);
        const auto startup=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        while(!certificates.context() && std::chrono::steady_clock::now()<startup)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(!certificates.context())return 1;
        std::printf("certificate ready\n");std::fflush(stdout);
        auto previous=certificates.context();
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(std::stoi(argv[5]));
        while(std::chrono::steady_clock::now()<end) {
            const auto current=certificates.context();
            if(current!=previous) {std::puts("certificate renewed");std::fflush(stdout);previous=current;}
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return 0;
    }catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
}
