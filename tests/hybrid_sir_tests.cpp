#include "ankurafathom/abm/models/sir_async.hpp"
#include <iostream>
#include <numeric>
using Model=ankurafathom::abm::models::AsyncSIR;
using Network=ankurafathom::abm::CsrNetwork;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } require(caught,"invalid mixed SIR accepted"); }
int main() {
    try {
        // Exact complete-graph equivalence, including event choice, clock and pending race.
        for(std::size_t n:{1,4,8,16}) for(std::uint32_t r=0;r<24;++r) {
            const std::uint64_t first=4294967298ULL;
            std::vector<std::uint64_t> ids(n); std::iota(ids.begin(),ids.end(),first);
            std::vector<Network::Edge> edges;
            for(auto i:ids) for(auto j:ids) if(i<j) edges.push_back({i,j});
            std::vector<std::int64_t> states(n,0); states[0]=1;
            if(n>2) states[n-1]=2;
            const Model::Draws draws{0xFEDCBA9876543210ULL,65535,r,65535,65534};
            auto mixed=Model::well_mixed({.8,.3},states,draws,first);
            Model explicit_graph({.8/static_cast<double>(n),.3},states,Network(ids,edges),draws,first);
            require(mixed.is_well_mixed() && !mixed.store().network() && !explicit_graph.is_well_mixed(),"contact policy ambiguous");
            for(double t:{0.,.125,.5,1.,2.,4.,8.,20.}) {
                require(mixed.next_time()==explicit_graph.next_time(),"mixed race differs from complete graph");
                require(mixed.run_until(t)==explicit_graph.run_until(t),"mixed event history differs from complete graph");
                require(mixed.states()==explicit_graph.states() && mixed.counts()==explicit_graph.counts(),"mixed states differ from complete graph");
            }
        }
        // N includes susceptible, infected and recovered; no N-1 or active-only normalization.
        auto hand=Model::well_mixed({4,0},{0,1,2,2},{},0,Model::Script{{std::exp(-1.),.5}});
        require(std::abs(hand.next_time()-1)<1e-14,"normalization denominator is not total N");
        auto e=hand.step(); require(e && e->agent==0 && hand.counts().infected==2 && !hand.step(),"mixed infection-only history");
        auto empty=Model::well_mixed({}, {},{}); require(empty.run_until(2).empty(),"empty mixed population");
        auto susceptible=Model::well_mixed({1,1},{0,0,2},{}); require(susceptible.run_until(2).empty(),"spontaneous mixed infection");
        auto failed=Model::well_mixed({1,1},{1,0},{},0,Model::Script{{.5,.9}});
        auto saved=failed;
        rejects([&] { failed.run_until(100); });
        require(failed.states()==saved.states() && failed.events()==0 && failed.time()==0 && failed.next_time()==saved.next_time(),"mixed horizon rollback");
        rejects([] { Model::well_mixed({-1,1},{1},{}); });
        rejects([] { Model::well_mixed({}, {3},{}); });
        rejects([] { Model::well_mixed({}, {},{0,0,0,1,1}); });
        rejects([] { Model::well_mixed({}, {},{},std::uint64_t{1}<<48); });
        std::cout<<"Well-mixed SIR: 96 exact complete-graph comparisons, normalization, empty/absorbing and rollback contracts passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
