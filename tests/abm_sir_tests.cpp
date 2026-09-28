#include "ankurafathom/abm/models/sir.hpp"
#include <iostream>
#include <limits>
using namespace ankurafathom;
using Model=abm::models::SIR;
using Network=abm::CsrNetwork;
void require(bool v,const char* msg) { if(!v) throw std::runtime_error(msg); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } require(rejected,"invalid SIR accepted"); }
void same(const Model& a,const Model& b) {
    require(a.states()==b.states() && a.ticks()==b.ticks() && a.time()==b.time() && a.counts()==b.counts()
            && a.store().network()->edges()==b.store().network()->edges(),"SIR state changed");
}
int main() {
    try {
        const Network line({0,1,2},{{0,1},{1,2}});
        // Saturated hazards give exact synchronous histories: no same-tick transmission cascade.
        Model sync({800,800,1},{1,0,0},line,{});
        sync.scripted_step({.5,.5,.5});
        require(sync.states()==std::vector<std::int64_t>{2,1,0},"new infection transmitted/recovered in same tick");
        sync.scripted_step({.5,.5,.5});
        require(sync.states()==std::vector<std::int64_t>{2,2,1},"recovering source failed to transmit from snapshot");
        sync.step(); require(sync.counts()==Model::Counts{0,0,3} && sync.time()==3,"SIR conservation/clock");
        sync.step(); require(sync.counts()==Model::Counts{0,0,3},"recovered agents changed");
        Model zero({0,0,1},{1,0,2},line,{});
        zero.scripted_step({0,0,0}); require(zero.states()==std::vector<std::int64_t>{1,0,2},"zero hazard transitioned at zero variate");
        Model isolated({800,0,1},{1,0,0},Network({0,1,2},{}),{});
        isolated.step(); require(isolated.counts()==Model::Counts{2,1,0},"infection crossed absent edge");
        // Additive per-edge hazard; no degree normalization. Strict threshold and tiny hazards.
        Model rates({.7,.4,.2},{1,0,1},line,{});
        const auto p=-std::expm1(-(.7*.2)*2);
        require(rates.infection_probability(1)==p && rates.recovery_probability()==-std::expm1(-.4*.2),"integrated hazards differ");
        auto boundary=rates;
        boundary.scripted_step({.99,p,.99}); require(boundary.states()[1]==0,"equality threshold infected");
        rates.scripted_step({.99,std::nextafter(p,0.),.99}); require(rates.states()[1]==1,"below threshold did not infect");
        Model tiny({1e-20,1e-20,1},{1,0},Network({0,1},{{0,1}}),{});
        require(tiny.infection_probability(1)>0 && tiny.recovery_probability()>0,"tiny hazard cancelled");
        rejects([&] { (void)tiny.infection_probability(9); });
        // A late invalid variate follows valid pending changes; the full tick must roll back.
        Model failed({800,800,1},{1,0,0},line,{77,3,4,11,12});
        const auto saved=failed;
        rejects([&] { failed.scripted_step({0,0,1}); }); same(failed,saved);
        rejects([&] { failed.scripted_step({0,0,std::numeric_limits<double>::quiet_NaN()}); }); same(failed,saved);
        rejects([&] { failed.scripted_step({0,-.1,0}); });
        rejects([&] { failed.scripted_step({0,0}); });
        auto replay=saved; failed.step(); replay.step(); same(failed,replay);
        require(saved.ticks()==0 && saved.states()==std::vector<std::int64_t>{1,0,0},"copy shares state");
        Model clock({0,0,std::numeric_limits<double>::max()},{},Network({},{}),{});
        clock.step(); const auto before=clock;
        rejects([&] { clock.step(); }); same(clock,before);
        Model exhausted({}, {},Network({},{}),{});
        for(unsigned i=0;i<65536;++i) exhausted.step();
        rejects([&] { exhausted.step(); }); require(exhausted.ticks()==65536,"exhausted tick advanced");
        for(auto p:std::vector<Model::Parameters>{{-1,0,1},{0,-1,1},{0,0,0},{0,0,-1},
                  {std::numeric_limits<double>::infinity(),0,1},{0,std::numeric_limits<double>::quiet_NaN(),1},
                  {0,0,std::numeric_limits<double>::infinity()},{1e308,0,2},{0,1e308,2}})
            rejects([&] { Model(p,{1,0},Network({0,1},{{0,1}}),{}); });
        rejects([] { Model({1e308,0,1},{1,0,1},Network({0,1,2},{{0,1},{1,2}}),{}); });
        rejects([] { Model({}, {-1},Network({0},{}),{}); });
        rejects([] { Model({}, {3},Network({0},{}),{}); });
        rejects([] { Model({}, {0,1},Network({0},{}),{}); });
        rejects([] { Model({}, {0,1},Network({0,1},{{0,1}},true),{}); });
        rejects([] { Model({}, {},Network({},{}),{0,0,0,1,1}); });
        rejects([] { Model({}, {},Network({},{}),{0,65536,0,1,2}); });
        rejects([] { Model({}, {},Network({},{}),{0,0,65536,1,2}); });
        rejects([] { Model({}, {},Network({},{}),{0,0,0,65536,2}); });
        rejects([] { Model({}, {},Network({},{}),{},std::uint64_t{1}<<48); });
        // Vertex/edge declaration order must not alter addressing or the trajectory.
        Model a({.7,.2,.25},{1,0,0},line,{7319,9,4,71,72});
        Model b({.7,.2,.25},{1,0,0},Network({2,0,1},{{2,1},{1,0}}),{7319,9,4,71,72});
        for(unsigned i=0;i<32;++i) { (void)a.counts(); (void)a.infection_probability(1); a.step(); b.step(); same(a,b); }
        // Independent Python recurrence exercises upper entity/key bits and maximum address fields.
        const std::uint64_t first=4294967298ULL;
        Model addressed({.9,.3,.5},{1,0,0,1},
                        Network({first,first+1,first+2,first+3},{{first,first+1},{first+1,first+2},{first+2,first+3}}),
                        {0xFEDCBA9876543210ULL,65535,65535,65535,65534},first);
        for(const auto& expected:std::vector<std::vector<std::int64_t>>{{1,0,1,2},{1,1,1,2},{1,1,2,2},{2,1,2,2}}) {
            addressed.step(); require(addressed.states()==expected,"high-address SIR recurrence differs");
        }
        std::cout<<"SIR synchronous hazards, transitions, conservation, graph, boundaries, replay and rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
