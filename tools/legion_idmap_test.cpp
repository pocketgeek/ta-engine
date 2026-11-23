// IdMap (sim/idmap.h; WE E4.4 D5): Legion's flat groups/members/anchors maps
// must visit exactly as std::map<int,T> does.
//
//   legion_idmap_test
//
// Random streams of inserts, erases, lookups and ordered walks run on an
// IdMap<int> and a std::map<int,int> side by side, and every walk mutates the
// maps mid-walk the ways std::map allows (insert any key, erase any key but
// the current one, erase the current one through erase(it)). Each walk's visit
// sequence, every lookup and every lower_bound must match, and a value's
// address must not move while its key stays in the map.
#include "sim/idmap.h"

#include <cstdio>
#include <map>
#include <random>
#include <vector>

using tak::sim::IdMap;

namespace {
int failures=0;
void check(bool ok,const char* what,unsigned seed,int step) {
    if(ok)return;
    if(++failures<=10)std::fprintf(stderr,"FAIL seed %u step %d: %s\n",seed,step,what);
}
}

int main() {
    for(unsigned seed=0;seed<200;++seed) {
        std::mt19937 rng(seed);
        // Key ranges from tiny (heavy churn on the same ids) to wide.
        const int range=seed%4==0?8:seed%4==1?64:seed%4==2?1000:20000;
        IdMap<int> a;std::map<int,int> b;
        std::map<int,const int*> where;   // key -> address of its value in `a`
        auto key=[&] {return int(rng()%unsigned(range));};
        auto insert=[&](int k,int v) {
            const auto ra=a.try_emplace(k,v);const auto rb=b.try_emplace(k,v);
            check(ra.second==rb.second,"try_emplace inserted flag",seed,0);
            check(ra.first->first==k&&ra.first->second==rb.first->second,"try_emplace value",seed,0);
            if(ra.second)where[k]=&ra.first->second;
        };
        auto erase=[&](int k) {
            check(a.erase(k)==b.erase(k),"erase count",seed,0);
            where.erase(k);
        };
        for(int step=0;step<4000;++step) {
            const unsigned op=rng()%100;
            if(op<35)insert(key(),int(rng()));
            else if(op<45) {const int k=key();a[k]+=1;b[k]+=1;if(!where.count(k))where[k]=a.get(k);}
            else if(op<70)erase(key());
            else if(op<80) {
                const int k=key();
                const auto fa=a.find(k);const auto fb=b.find(k);
                check((fa==a.end())==(fb==b.end()),"find presence",seed,step);
                if(fb!=b.end())check(fa->second==fb->second&&a.get(k)==&fa->second,"find value",seed,step);
                else check(a.get(k)==nullptr&&a.count(k)==0,"get of absent",seed,step);
                // A lookup's iterator increments to the next key, as std::map's does.
                if(fb!=b.end()) {
                    auto na=fa;++na;auto nb=fb;++nb;
                    check((na==a.end())==(nb==b.end())&&(nb==b.end()||na->first==nb->first),"++find",seed,step);
                }
            } else if(op<85) {
                const int k=key();
                const auto la=a.lower_bound(k);const auto lb=b.lower_bound(k);
                check((la==a.end())==(lb==b.end())&&(lb==b.end()||la->first==lb->first),"lower_bound",seed,step);
            } else {
                // A walk with mutation, as Legion's loops do (insert into the
                // map being walked, erase other entries, erase the current one).
                // a[] is std::map's operator[]: it must not move other values.
                std::vector<int> va,vb;
                auto ia=a.begin();auto ib=b.begin();
                while(ib!=b.end()) {
                    if(ia==a.end()) {check(false,"walk ended early",seed,step);break;}
                    check(ia->first==ib->first&&ia->second==ib->second,"walk entry",seed,step);
                    va.push_back(ia->first);vb.push_back(ib->first);
                    const int cur=ib->first;
                    const unsigned m=rng()%10;
                    if(m==0) {const int k=key();const int v=int(rng());a.try_emplace(k,v);b.try_emplace(k,v);if(!where.count(k))where[k]=a.get(k);}
                    else if(m==1) {const int k=key();if(k!=cur) {a.erase(k);b.erase(k);where.erase(k);}}
                    if(m==2) {ia=a.erase(ia);ib=b.erase(ib);where.erase(cur);}
                    else {++ia;++ib;}
                }
                check(ia==a.end(),"walk ended late",seed,step);
                check(va==vb,"walk order",seed,step);
            }
            if(step%97==0) {
                check(a.size()==b.size()&&a.empty()==b.empty(),"size",seed,step);
                std::vector<int> ka,kb;
                for(const auto& [k,v]:a)ka.push_back(k);
                for(const auto& [k,v]:b)kb.push_back(k);
                check(ka==kb,"full walk",seed,step);
                for(const auto& [k,p]:where)check(a.get(k)==p,"value moved",seed,step);
            }
        }
    }
    if(failures) {std::fprintf(stderr,"legion_idmap_test: %d failures\n",failures);return 1;}
    std::puts("legion_idmap_test: ok");
    return 0;
}
