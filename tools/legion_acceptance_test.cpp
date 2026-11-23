// legion_acceptance_test -- the Legion navigation requirements as small, fast,
// mode-parameterised World checks.
//
//   legion_acceptance_test MODE CHECK [--report]
//
// Each check runs one crowdbench acceptance scenario (tools/crowdbench_matrix.h,
// metrics in tools/crowdbench_acceptance.h) so the test, the benchmark and the
// published comparison share one definition. With --report the result is
// printed and the exit status is 0 whatever it says: the other modes are
// measured for comparison, not held to Legion's bar.
//
// Checks (requirement -> assertion):
//   singleunit  lone units route around a concave cup (half start inside it):
//               every unit reaches its goal, path/shortest-legal-path <= 1.25 each
//   group       64 units around a large block: all 64 legally settled, none illegal
//   jagged      mixed 1..4 footprints through the sawtooth funnel, corridor and rock
//               field: all reach the goal, zero units ever terrain_stuck, no spinning
//   trapped     1 in 8 units in sealed or too-narrow pockets: zero spinning while
//               trapped, last motion within 300 ticks of becoming trapped, the
//               free units all reach the goal
//   crowdheld   crowd at two chokes, one gated shut then reopened: zero spinning
//               while crowd-held or trapped, zero terrain_stuck, all reach the goal
#include "crowdbench_matrix.h"

#include <cstdio>
#include <cstring>
#include <string>
#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace {
// Run one crowdbench case and capture its JSON line.
std::string capture(const crowdbench_matrix::Options& o) {
    std::fflush(stdout);
    FILE* temp=std::tmpfile();
    if(!temp)throw std::runtime_error("tmpfile failed");
    const int saved=dup(fileno(stdout));
    dup2(fileno(temp),fileno(stdout));
    const int status=crowdbench_matrix::run(o);
    std::fflush(stdout);dup2(saved,fileno(stdout));
    std::rewind(temp);std::string text;char buffer[4096];size_t n;
    while((n=std::fread(buffer,1,sizeof buffer,temp))>0)text.append(buffer,n);
    std::fclose(temp);
    if(status)throw std::runtime_error("crowdbench run failed");
    return text;
}
double field(const std::string& json,const char* key) {
    const std::string needle=std::string("\"")+key+"\":";
    const auto at=json.find(needle);
    if(at==std::string::npos)throw std::runtime_error(std::string("missing field ")+key);
    return std::strtod(json.c_str()+at+needle.size(),nullptr);
}
}

int main(int argc,char** argv) {
    if(argc<3) {std::fprintf(stderr,"usage: legion_acceptance_test MODE CHECK [--report]\n");return 2;}
    const std::string mode=argv[1],check=argv[2];
    const bool report=argc>3&&std::strcmp(argv[3],"--report")==0;
    crowdbench_matrix::Options o;o.mode=mode;o.players=1;o.movingPercent=100;o.seed=7;
    struct Rule {const char* name;bool ok;double value;};
    std::vector<Rule> rules;
    try {
        if(check=="singleunit") {o.scenario="singleunit";o.units=4;o.ticks=1800;}
        else if(check=="group") {o.scenario="groupdetour";o.units=64;o.ticks=6000;}
        else if(check=="jagged") {o.scenario="jagged";o.units=16;o.ticks=6000;}
        else if(check=="trapped") {o.scenario="trapped";o.units=32;o.ticks=4500;}
        else if(check=="crowdheld") {o.scenario="crowdtrap";o.units=64;o.ticks=7200;}
        else {std::fprintf(stderr,"unknown check %s\n",check.c_str());return 2;}
        const std::string j=capture(o);
        const double moving=field(j,"moving_units");
        auto rule=[&](const char* name,bool ok,double value) {rules.push_back({name,ok,value});};
        if(check=="singleunit") {
            rule("physical_in_goal == moving",field(j,"physical_in_goal")==moving,field(j,"physical_in_goal"));
            rule("path_optimality_units == moving",field(j,"path_optimality_units")==moving,field(j,"path_optimality_units"));
            const double ratio=field(j,"path_optimality_ratio_max");
            rule("path_optimality_ratio_max <= 1.25",ratio>0&&ratio<=1.25,ratio);
            rule("spin_unit_ticks == 0",field(j,"spin_unit_ticks")==0,field(j,"spin_unit_ticks"));
        } else if(check=="group") {
            rule("arrived_settled == 64",field(j,"arrived_settled")==moving,field(j,"arrived_settled"));
            rule("illegal_final_movers == 0",field(j,"illegal_final_movers")==0,field(j,"illegal_final_movers"));
            rule("units_ever_terrain_stuck == 0",field(j,"units_ever_terrain_stuck")==0,field(j,"units_ever_terrain_stuck"));
        } else if(check=="jagged") {
            rule("physical_in_goal == moving",field(j,"physical_in_goal")==moving,field(j,"physical_in_goal"));
            rule("units_ever_terrain_stuck == 0",field(j,"units_ever_terrain_stuck")==0,field(j,"units_ever_terrain_stuck"));
            rule("spin_unit_ticks == 0",field(j,"spin_unit_ticks")==0,field(j,"spin_unit_ticks"));
        } else if(check=="trapped") {
            const double trapped=field(j,"units_ever_trapped");
            rule("units_ever_trapped == moving/8",trapped==moving/8,trapped);
            rule("spin_while_trapped_unit_ticks == 0",field(j,"spin_while_trapped_unit_ticks")==0,field(j,"spin_while_trapped_unit_ticks"));
            const double settle=field(j,"trapped_settle_ticks_max");
            rule("trapped_settle_ticks_max <= 300",settle<=300,settle);
            rule("physical_in_goal == moving - trapped",field(j,"physical_in_goal")==moving-trapped,field(j,"physical_in_goal"));
        } else {
            rule("spin_while_crowd_held_unit_ticks == 0",field(j,"spin_while_crowd_held_unit_ticks")==0,field(j,"spin_while_crowd_held_unit_ticks"));
            rule("spin_while_trapped_unit_ticks == 0",field(j,"spin_while_trapped_unit_ticks")==0,field(j,"spin_while_trapped_unit_ticks"));
            rule("units_ever_terrain_stuck == 0",field(j,"units_ever_terrain_stuck")==0,field(j,"units_ever_terrain_stuck"));
            rule("physical_in_goal == moving",field(j,"physical_in_goal")==moving,field(j,"physical_in_goal"));
        }
    } catch(const std::exception& e) {
        std::fprintf(stderr,"legion_acceptance %s %s: %s\n",mode.c_str(),check.c_str(),e.what());return 2;
    }
    bool pass=true;
    for(const auto& r:rules) {
        std::printf("%s %s %s: %s (value %g)\n",mode.c_str(),check.c_str(),r.ok?"PASS":"FAIL",r.name,r.value);
        pass&=r.ok;
    }
    std::printf("%s %s %s\n",mode.c_str(),check.c_str(),pass?"PASSED":report?"FAILED (reported, not asserted)":"FAILED");
    return pass||report?0:1;
}
