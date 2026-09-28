#include "ankurafathom/abm/models/sir.hpp"
#include <iostream>
#include <numeric>
using Model=ankurafathom::abm::models::SIR;
using Network=ankurafathom::abm::CsrNetwork;
int main() {
    std::vector<std::uint64_t> ids(24); std::iota(ids.begin(),ids.end(),0);
    std::set<Network::Edge> edges;
    std::vector<std::int64_t> states;
    for(auto i:ids) {
        states.push_back(i==0 || i==6 ? 1 : i==23 ? 2 : 0);
        for(std::uint64_t d:{1,2}) { auto j=(i+d)%ids.size(); edges.insert({std::min(i,j),std::max(i,j)}); }
    }
    Model model({.5,.3,.25},states,Network(ids,{edges.begin(),edges.end()}),{67867967,37,0,1301,1302});
    std::cout<<"tick,time,susceptible,infected,recovered\n";
    for(unsigned tick=0;tick<=32;++tick) {
        if(tick) model.step();
        const auto c=model.counts();
        std::cout<<tick<<','<<model.time()<<','<<c.susceptible<<','<<c.infected<<','<<c.recovered<<'\n';
    }
}
