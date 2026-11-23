#include "server/validationworker.h"
#include <cstdio>
#include "util/stoptoken.h"
#include <chrono>
int main() try {
    if(tak::StopToken{}.stop_requested())throw std::runtime_error("empty token cancelled");
    tak::StopToken retained;
    {
        tak::StopSource source;
        retained=source.get_token();
        if(retained.stop_requested() || !source.request_stop() || source.request_stop())
            throw std::runtime_error("cancellation transition failure");
    }
    if(!retained.stop_requested())throw std::runtime_error("token lost source state");
    tak::srv::ValidationWorker worker;
    std::promise<void> entered,release;
    auto gate=release.get_future().share();
    auto first=worker.submit([&]{entered.set_value();gate.wait();return 1;});
    entered.get_future().wait();
    tak::StopSource cancelled;
    auto second=worker.submit([token=cancelled.get_token()]{return token.stop_requested();});
    auto third=worker.submit([]{return 3;});
    bool rejected=false;
    try{worker.submit([]{return 4;});}catch(const std::runtime_error&){rejected=true;}
    cancelled.request_stop();release.set_value();
    if(!rejected || first.get()!=1 || !second.get() || third.get()!=3)throw std::runtime_error("queue bound/cancellation failure");
    auto recovered=worker.submit([]{return 5;});
    if(recovered.get()!=5)throw std::runtime_error("worker did not recover");
    std::puts("validation queue bounds, cancellation and recovery passed");return 0;
} catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
