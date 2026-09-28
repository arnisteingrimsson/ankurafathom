#include "ankurafathom/hybrid/dynamic_agent_stocks.hpp"
#include "ankurafathom/hybrid/event_to_lifecycle.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>

namespace af=ankurafathom;
struct Person {};struct Event { std::uint64_t key; };
using Core=af::hybrid::AgentStocks<Person>;
using Input=af::hybrid::AgentStockInput<Person>;
using Commit=af::hybrid::AgentStockCommit<Person>;
using Life=af::abm::PopulationLifecycleInput<Person>;
using M=std::variant<Event,Input,Commit,Life,af::hybrid::PopulationSnapshot<Person>,af::hybrid::ScalarPublication>;
using Owner=af::hybrid::DynamicAgentStocksAtomic<Person,M>;
using Bridge=af::hybrid::EventToLifecycle<Event,Person,M>;
using Aggregate=af::hybrid::PopulationAggregate<Person,M>;
using SD=af::hybrid::SignalSD<M>;
using json=nlohmann::json;
Core::Store::Record record(const json& value) { return {value[0].get<double>(),value[1].get<double>(),value[2].get<double>(),value[3].get<bool>()}; }
Core initial(const json& c) {
    using K=af::des::FieldKind;using E=Core::Endpoint;
    Core::Store store(9,{{"work",K::real},{"done",K::real},{"rate",K::real},{"enabled",K::boolean}},c.at("first"));
    for(const auto& r:c.at("initial")) store.spawn(record(r));
    Core core(store,{{0,true},{1,true}},{{"global",0,true}});
    core.add_agent_flow(E::agent(0),E::agent(1),[](auto ref,const auto& s,const auto& globals,double) {
        return std::get<bool>(s.field(ref,3))?std::get<double>(s.field(ref,2))*(1+globals[0]/64):0.;
    });
    core.add_agent_flow(E::agent(0),E::global(0),[](auto ref,const auto& s,const auto&,double) { return std::get<bool>(s.field(ref,3))?.125:0.; });
    return core;
}
json receipt(const Commit& commit) {
    const auto& r=*commit.discrete;json born=json::array(),retired=json::array();
    for(const auto ref:r.born) born.push_back(ref.id);for(const auto ref:r.retired) retired.push_back(ref.id);
    return json::array({commit.time,commit.revision,born,retired,r.birth_amounts,r.retired_amounts,r.agent_pulses,r.global_pulses});
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan/output paths");
        std::ifstream in(argv[1]);const auto plan=json::parse(in);std::ofstream out(argv[2]);out.exceptions(std::ios::badbit|std::ios::failbit);
        std::size_t snapshots=0;
        for(const auto& c:plan.at("cases")) for(const auto& order:plan.at("orders")) for(const std::string direction:plan.at("bag_orders")) {
            const auto start=initial(c);const auto first=start.store().first_id();
            std::map<std::uint64_t,Bridge::Change> lifecycle;
            for(const auto& e:c.at("lifecycle")) {
                Bridge::Change change;for(const std::uint64_t offset:e.at("retire")) change.retirements.push_back({9,first+offset});
                for(const auto& r:e.at("births")) change.births.push_back(record(r));lifecycle.emplace(e.at("key"),std::move(change));
            }
            af::devs::Simulator<M> sim;std::array<std::size_t,4> ids{};
            for(const int role:order) {
                if(role==0) ids[role]=sim.add(std::make_unique<Owner>(start,c.at("agent_dt"),std::vector<std::string>{"a","z"}));
                if(role==1) {
                    using R=Aggregate::Reduction;
                    ids[role]=sim.add(std::make_unique<Aggregate>(start.store(),std::vector<R>{
                        {"work",R::Kind::sum,[](auto ref,const auto& s) { return std::get<double>(s.field(ref,0)); }},
                        {"done",R::Kind::sum,[](auto ref,const auto& s) { return std::get<double>(s.field(ref,1)); }},
                        {"headcount",R::Kind::count}}));
                }
                if(role==2) ids[role]=sim.add(std::make_unique<SD>(std::vector<SD::Stock>{{"work_hours",0},{"done_hours",0},{"agent_hours",0}},
                    std::vector<SD::Flow>{{SD::boundary,0,[](const auto&,const auto& v,double) { return v[0]; }},
                        {SD::boundary,1,[](const auto&,const auto& v,double) { return v[1]; }},
                        {SD::boundary,2,[](const auto&,const auto& v,double) { return v[2]; }}},std::vector<double>{0,0,0},c.at("sd_dt")));
                if(role==3) ids[role]=sim.add(std::make_unique<Bridge>(start.store(),[](const Event& e) { return e.key; },
                    [lifecycle](const Event& e,double) { return lifecycle.at(e.key); }));
            }
            sim.connect(ids[0],1,ids[1],0);sim.connect(ids[1],1,ids[2],0);sim.connect(ids[3],1,ids[0],3);
            struct Injection { double time;std::size_t target;M message; };
            std::vector<Injection> injections;std::uint64_t revision=0;
            for(const auto& e:c.at("direct")) {
                Core::Change change;
                if(e.contains("updates")) for(const auto& u:e.at("updates")) {
                    Core::Store::Value value=u[1]==3?Core::Store::Value{u[2].get<bool>()}:Core::Store::Value{u[2].get<double>()};
                    change.updates.push_back({{9,first+u[0].get<std::uint64_t>()},u[1],value});
                }
                if(e.contains("pulses")) for(const auto& p:e.at("pulses")) {
                    if(p[0]=="global") change.pulses.push_back({Core::Endpoint::global(p[2]),{},p[3]});
                    else change.pulses.push_back({Core::Endpoint::agent(p[2]),Core::Reference{9,first+p[1].get<std::uint64_t>()},p[3]});
                }
                if(e.contains("births")) for(const auto& b:e.at("births")) change.births.push_back(record(b));
                injections.push_back({e.at("time"),ids[0],M{Input{e.at("time"),e.at("channel"),revision++,change}}});
            }
            for(const auto& e:c.at("lifecycle")) injections.push_back({e.at("time"),ids[3],M{Event{e.at("key")}}});
            if(direction=="reverse") std::reverse(injections.begin(),injections.end());
            for(const auto& injection:injections) sim.inject(injection.time,injection.target,0,injection.message);
            json receipts=json::array();
            for(const double horizon:plan.at("horizons")) {
                for(const auto& step:sim.run_until_transactional(horizon)) for(const auto& emission:step.emissions)
                    if(emission.source==ids[0] && emission.port==Owner::commit_port) {
                        const auto& commit=std::get<Commit>(emission.value);if(commit.discrete) receipts.push_back(receipt(commit));
                    }
                const auto& owner=dynamic_cast<const Owner&>(sim.model(ids[0]));const auto& s=owner.core().store();
                const auto& sd=dynamic_cast<const SD&>(sim.model(ids[2]));
                const auto& work=std::get<std::vector<double>>(s.column(0));const auto& done=std::get<std::vector<double>>(s.column(1));
                const auto& rate=std::get<std::vector<double>>(s.column(2));const auto& enabled=std::get<std::vector<bool>>(s.column(3));
                json rows=json::array();for(std::size_t i=0;i<s.size();++i) rows.push_back(json::array({first+i,work[i],done[i],rate[i],bool(enabled[i]),bool(s.live_rows()[i])}));
                const json state{{"time",owner.core().time()},{"revision",owner.revision()},{"rows",rows},{"globals",owner.core().global_state()},
                    {"sd_time",sd.time()},{"sd",sd.state()},{"signals",sd.signals()},{"receipts",receipts},{"next_id",s.next_id()}};
                out<<json{{"case",c.at("id")},{"order",order},{"bag_order",direction},{"horizon",horizon},{"state",state}}.dump()<<'\n';++snapshots;
            }
        }
        std::cout<<snapshots<<" dynamic agent stock/lifecycle/aggregate/SD snapshots generated\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
