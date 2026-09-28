#include "ankurafathom/abm/models/sir_async.hpp"
#include <iostream>
using namespace ankurafathom;
using Model=abm::models::AsyncSIR;
using Network=abm::CsrNetwork;
void require(bool v,const char* msg) { if(!v) throw std::runtime_error(msg); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } require(rejected,"invalid async SIR accepted"); }
void same(const Model& a,const Model& b) {
    require(a.states()==b.states() && a.time()==b.time() && a.next_time()==b.next_time() && a.events()==b.events()
            && a.pending_count()==b.pending_count() && a.store().network()->edges()==b.store().network()->edges(),"async SIR state changed");
}
int main() {
    try {
        const Network pair({0,1},{{0,1}});
        const Model::Script script{{std::exp(-2.),.75},{std::exp(-2.),.25},{std::exp(-1.),.5}};
        Model hand({1,1},{1,0},pair,{},0,script);
        require(std::abs(hand.next_time()-1)<1e-14 && hand.pending_count()==1,"initial race deadline");
        const auto first=hand.step();
        require(first && first->agent==1 && first->before==0 && first->after==1 && first->generation==0,"infection race selection");
        require(hand.states()==std::vector<std::int64_t>{1,1} && std::abs(hand.next_time()-2)<1e-14,"post-infection hazards");
        const auto second=hand.step(); require(second->agent==0 && second->after==2,"recovery race selection");
        const auto third=hand.step(); require(third->agent==1 && std::abs(third->time-3)<1e-14,"last recovery deadline");
        require(hand.counts()==abm::models::SIR::Counts{0,0,2} && hand.pending_count()==0 && !hand.step(),"absorbing process");
        require(hand.run_until(5).empty() && hand.time()==5 && hand.events()==3,"absorbed projection");
        // Exact selection boundary skips the interval ending at the target.
        Model boundary({1,1},{1,0},pair,{},0,Model::Script{{.5,.5},{.5,.25},{.5,.5}});
        require(boundary.step()->agent==1,"weighted equality boundary");
        Model extinct({1,1},{1,0},pair,{},0,Model::Script{{.5,.25}});
        require(extinct.step()->agent==0 && !std::isfinite(extinct.next_time()) && extinct.states()[1]==0,"recovery failed to cancel infection hazard");
        Model infection_only({1,0},{1,0},pair,{},0,Model::Script{{.5,.25}});
        require(infection_only.step()->agent==1 && infection_only.counts().infected==2 && !infection_only.step(),"zero recovery process");
        Model zero({0,0},{1,0},pair,{});
        require(!zero.step() && zero.run_until(100).empty() && zero.events()==0,"zero rates scheduled event");
        Model empty({}, {},Network({},{}),{});
        require(empty.run_until(1).empty() && empty.counts()==abm::models::SIR::Counts{},"empty process");
        // Projection at an event boundary includes it; observation density preserves pending draws.
        Model dense({.8,.4},{1,0,0},Network({0,1,2},{{0,1},{1,2}}),{19,3,4,41,42});
        auto sparse=dense;
        const auto next=dense.next_time();
        require(dense.run_until(std::nextafter(next,0.)).empty(),"early event");
        require(dense.run_until(next).size()==1,"event excluded at horizon");
        dense.run_until(100); sparse.run_until(100); same(dense,sparse);
        auto copy=sparse; copy.run_until(101); require(sparse.time()==100,"copy shares clock");
        // A failed successor schedule rolls back the current event and its calendar.
        Model failed({1,1},{1,0},pair,{},0,Model::Script{{.5,.75},{0,.5}});
        const auto saved=failed;
        rejects([&] { failed.step(); }); same(failed,saved);
        rejects([&] { failed.step(); }); same(failed,saved);
        Model late({1,1},{1,0},pair,{},0,Model::Script{{.5,.75},{.5,.25}});
        const auto original=late;
        rejects([&] { late.run_until(100); }); same(late,original);
        require(late.step().has_value() && late.events()==1,"valid partial retry failed");
        const auto partial=late; rejects([&] { late.step(); }); same(late,partial);
        for(double horizon:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
            rejects([&] { sparse.run_until(horizon); });
        rejects([&] { sparse.run_until(99); });
        for(auto p:std::vector<Model::Parameters>{{-1,0},{0,-1},{std::numeric_limits<double>::infinity(),0},{0,std::numeric_limits<double>::quiet_NaN()}})
            rejects([&] { Model(p,{1,0},pair,{}); });
        rejects([&] { Model({1e308,1e308},{1,0},pair,{}); });
        rejects([&] { Model({0,std::numeric_limits<double>::denorm_min()},{1,0},pair,{},0,Model::Script{{.5,.5}}); });
        for(double u:{0.,1.,-.1,std::numeric_limits<double>::quiet_NaN()})
            rejects([&] { Model({1,1},{1,0},pair,{},0,Model::Script{{u,.5}}); });
        rejects([&] { Model({1,1},{1,0},pair,{},0,Model::Script{}); });
        rejects([] { Model({}, {3},Network({0},{}),{}); });
        rejects([] { Model({}, {0,1},Network({0},{}),{}); });
        rejects([] { Model({}, {0,1},Network({0,1},{{0,1}},true),{}); });
        rejects([] { Model({}, {},Network({},{}),{0,0,0,1,1}); });
        rejects([] { Model({}, {},Network({},{}),{0,65536,0,1,2}); });
        rejects([] { Model({}, {},Network({},{}),{0,0,65536,1,2}); });
        rejects([] { Model({}, {},Network({},{}),{0,0,0,65536,2}); });
        rejects([] { Model({}, {},Network({},{}),{},std::uint64_t{1}<<48); });
        Model permuted({.8,.4},{1,0,0},Network({2,0,1},{{2,1},{1,0}}),{19,3,4,41,42});
        permuted.run_until(100); same(permuted,sparse);
        const std::uint64_t first_id=4294967298ULL;
        Model high({.9,.3},{1,0,0,1},Network({first_id,first_id+1,first_id+2,first_id+3},
                   {{first_id,first_id+1},{first_id+1,first_id+2},{first_id+2,first_id+3}}),
                   {0xFEDCBA9876543210ULL,65535,65535,65535,65534},first_id);
        const auto history=high.run_until(4);
        const std::vector<Model::Event> golden{{.09550122048992324,first_id+2,0,1,0},
            {1.9722741320186599,first_id+2,1,2,1},{2.8790171918208705,first_id+1,0,1,2}};
        require(history.size()==golden.size() && high.states()==std::vector<std::int64_t>{1,1,2,1},"high-address async state differs");
        for(std::size_t i=0;i<golden.size();++i)
            require(std::abs(history[i].time-golden[i].time)<2e-11 && history[i].agent==golden[i].agent
                    && history[i].before==golden[i].before && history[i].after==golden[i].after
                    && history[i].generation==golden[i].generation,"high-address async event differs");
        std::cout<<"Async SIR races, hazard changes, boundaries, horizon/step rollback, replay and invariants passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
