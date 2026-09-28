#include "ankurafathom/hybrid/event_to_lifecycle.hpp"
#include "ankurafathom/hybrid/population_result_publisher.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>

struct Person {};struct Ticket {};
using Token=ankurafathom::des::EntityToken<Ticket>;
using M=std::variant<Token,ankurafathom::des::QueuePull,ankurafathom::abm::PopulationCommand<Person>,
    ankurafathom::abm::PopulationResult<Person>,ankurafathom::abm::PopulationTopicInput<Person>,
    ankurafathom::abm::PopulationLifecycleInput<Person>,ankurafathom::abm::PopulationNetworkInput<Person>,
    ankurafathom::hybrid::PopulationSnapshot<Person>,ankurafathom::hybrid::ScalarPublication>;
using P=ankurafathom::abm::PopulationAtomic<Person,M>;
using B=ankurafathom::hybrid::EventToLifecycle<Token,Person,M>;
using D=ankurafathom::des::ReferenceDelay<Ticket,M>;
using A=ankurafathom::hybrid::PopulationResultPublisher<Person,M>;
using Store=P::Store;using json=nlohmann::json;
Store::Record record(const json& r) { return {r[0].get<double>(),r[1].get<double>(),r[2].get<std::int64_t>()}; }
Store initial(const json& c) {
    Store s(8,{{"work",ankurafathom::des::FieldKind::real},{"rate",ankurafathom::des::FieldKind::real},{"origin",ankurafathom::des::FieldKind::integer}});
    for(const auto& r:c.at("initial")) s.spawn(record(r));
    s.configure_network(ankurafathom::abm::CsrNetwork({0,1,2},{{0,1},{0,2},{1,2}},false));return s;
}
std::unique_ptr<P> population(const Store& s,const std::string& mode,double dt) {
    if(mode=="sync") {
        P::Sync runtime(s);runtime.add_phase([](auto ref,const Store& store) { auto r=store.record(ref);std::get<double>(r[0])+=std::get<double>(r[1]);return r; });
        return std::make_unique<P>(std::move(runtime),dt);
    }
    P::Async runtime(s,[](const auto& timer,const Store& store) {
        P::Effects e;auto r=store.record(timer.agent);std::get<double>(r[0])+=2*std::get<double>(r[1]);e.updates.push_back({timer.agent,r});return e;
    });
    runtime.schedule({.5,{8,0},"due",0});runtime.schedule({.875,{8,1},"due",0});runtime.schedule({.625,{8,2},"due",0});
    auto p=std::make_unique<P>(std::move(runtime));
    p->configure_births([](auto ref,const Store& store,double t) { return P::Async::Birth{store.record(ref),{{t+.25,"due",0},{t+1.75,"later",0}}}; });return p;
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan/output paths");
        std::ifstream in(argv[1]);const auto plan=json::parse(in);
        std::ofstream out(argv[2]);out.exceptions(std::ios::badbit|std::ios::failbit);std::size_t snapshots=0;
        for(const auto& c:plan.at("cases")) for(const std::string mode:plan.at("modes")) for(const auto& order:plan.at("orders")) {
            ankurafathom::devs::Simulator<M> sim;std::array<std::size_t,4> ids{};
            const auto s=initial(c);
            std::map<std::uint64_t,B::Change> effects;
            for(const auto& e:c.at("events")) {
                B::Change change;
                for(std::uint64_t id:e.at("retirements")) change.retirements.push_back({8,id});
                for(const auto& r:e.at("births")) change.births.push_back(record(r));
                effects.emplace(e.at("key").get<std::uint64_t>(),std::move(change));
            }
            for(int role:order) {
                if(role==0) ids[role]=sim.add(std::make_unique<D>(7,.125));
                if(role==1) ids[role]=sim.add(std::make_unique<B>(s,[](const Token& t) { return t.entity.id; },
                    [effects](const Token& t,double) { return effects.at(t.entity.id); }));
                if(role==2) ids[role]=sim.add(population(s,mode,c.at("dt")));
                if(role==3) ids[role]=sim.add(std::make_unique<A>(s));
            }
            sim.connect(ids[0],1,ids[1],0);sim.connect(ids[1],1,ids[2],3);sim.connect(ids[2],1,ids[3],0);
            for(const auto& e:c.at("events")) sim.inject(e.at("time").get<double>()-.125,ids[0],0,M{Token{{7,e.at("key")}}});
            for(double h:plan.at("horizons")) {
                sim.run_until_transactional(h);
                const auto& pop=dynamic_cast<const P&>(sim.model(ids[2]));
                const auto& snapshot=dynamic_cast<const A&>(sim.model(ids[3])).snapshot();
                if(pop.store().next_id()!=snapshot.population.next_id() || pop.store().active_count()!=snapshot.population.active_count())
                    throw std::logic_error("adapter does not match committed population");
                json records=json::array(),edges=json::array();
                const auto& store=snapshot.population;
                const auto& work=std::get<std::vector<double>>(store.column(0));
                const auto& rates=std::get<std::vector<double>>(store.column(1));
                const auto& origins=std::get<std::vector<std::int64_t>>(store.column(2));
                for(std::size_t i=0;i<store.size();++i) records.push_back(json::array({i,work[i],rates[i],origins[i],bool(store.live_rows()[i])}));
                for(const auto& [a,b]:store.network()->edges()) edges.push_back(json::array({a,b}));
                const auto completed=dynamic_cast<const D&>(sim.model(ids[0])).completed_count();
                if(completed!=dynamic_cast<const B&>(sim.model(ids[1])).event_count()) throw std::logic_error("DES/lifecycle event count mismatch");
                out<<json{{"case",c.at("id")},{"mode",mode},{"order",order},{"horizon",h},{"time",pop.now()},
                          {"revision",snapshot.revision},{"completed",completed},{"rows",records},{"edges",edges}}.dump()<<'\n';++snapshots;
            }
        }
        std::cout<<snapshots<<" DES/lifecycle snapshots generated\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
