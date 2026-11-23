#include "tdf/tdf.h"
#include <cstdio>
#include <stdexcept>
using namespace tak::tdf;
static void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F fn) {bool rejected=false;try{fn();}catch(const ParseLimitError&){rejected=true;}check(rejected,"limit not enforced");}
int main() try {
    auto root=parseText("[MiXeD]{\n Value=first;\n}\n[other]{}\n[mixed]{\nVALUE=last;\n}");
    check(root.child("MIXED")->valueOr("value","")=="last","case/last definition");
    auto ordered=root.orderedChildren();
    check(ordered.size()==3 && ordered[0].second->valueOr("VALUE","")=="first" && ordered[2].first=="mixed","duplicate order/history");
    check(root.childOrder.size()==2,"unique first-seen order");
    std::string adversarial;
    for(int pass=0;pass<2;++pass)for(int i=0;i<16000;++i)adversarial+="[section"+std::to_string(i)+"]{}\n";
    auto many=parseText(adversarial);
    check(many.children.size()==16000 && many.orderedChildren().size()==32000,"wide duplicate sections");
    rejects([]{parseText("[a]{}[b]{}","sections",{1,20,100000});});
    rejects([]{parseText("[a]{\nx=1;\ny=2;\n}","nodes",{10,2,100000});});
    rejects([]{parseText("[a]{}","memory",{10,20,10});});
    ParseUsage usage;
    parseText("[a]{}","shared",{1,20,100000,{},&usage});
    rejects([&]{parseText("[b]{}","shared",{1,20,100000,{},&usage});});
    tak::StopSource stop;stop.request_stop();
    rejects([&]{parseText("[a]{}","cancel",{10,20,100000,stop.get_token()});});
    auto strings=parseText("// comment\n[TEXT]{\na=French; sentence;\nb=};\n}");
    check(strings.child("text")->valueOr("a","")=="French; sentence" && strings.child("text")->valueOr("b","")=="}","retail string syntax");
    std::puts("TDF compatibility and resource limits passed");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
