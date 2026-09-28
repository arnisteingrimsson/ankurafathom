#include "ankurafathom/hybrid/agent_stocks_atomic.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>
struct Person {};
using Core=ankurafathom::hybrid::AgentStocks<Person>;
using E=Core::Endpoint;
using Message=ankurafathom::hybrid::AggregateMessage<Person>;
using Atomic=ankurafathom::hybrid::AgentStocksAtomic<Person>;
using Aggregate=ankurafathom::hybrid::PopulationAggregate<Person>;
using Reduction=Aggregate::Reduction;
using SD=ankurafathom::hybrid::SignalSD<Message>;
using json=nlohmann::json;
Core make_core(const json& c) {
    Core::Store store(3,{{"work",ankurafathom::des::FieldKind::real},{"rate",ankurafathom::des::FieldKind::real}});
    for(std::size_t i=0;i<c.at("initial").size();++i) store.spawn({c.at("initial")[i].get<double>(),c.at("rates")[i].get<double>()});
    Core core(std::move(store),{{0}},{{"reservoir",c.at("reservoir")},{"completed",0},{"exposure",0}});
    core.add_agent_flow(E::agent(0),E::global(1),[](auto ref,const auto& s,const auto&,double) {
        return std::get<double>(s.field(ref,0))*std::get<double>(s.field(ref,1));
    });
    core.add_agent_flow(E::global(0),E::agent(0),[a=c.at("supply_rate").get<double>(),n=c.at("initial").size()](auto,const auto&,const auto& g,double) {
        return a*g[0]/n;
    });
    core.add_global_flow(E::boundary(),E::global(2),[](const auto& s,const auto&,double) { return Core::sum(s,0); });
    return core;
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream input(argv[1]);const auto plan=json::parse(input);
        std::ofstream output(argv[2]);output.exceptions(std::ios::badbit|std::ios::failbit);
        std::size_t rows=0;
        for(const auto& c:plan.at("cases")) for(double dt:plan.at("agent_steps"))
        for(double ratio:plan.at("consumer_ratios")) for(const auto& order:plan.at("orders")) {
            auto core=make_core(c);
            std::array<std::unique_ptr<ankurafathom::devs::Atomic<Message>>,3> components;
            components[0]=std::make_unique<Atomic>(core,dt);
            components[1]=std::make_unique<Aggregate>(core.store(),std::vector<Reduction>{
                {"work",Reduction::Kind::sum,[](auto ref,const auto& s) { return std::get<double>(s.field(ref,0)); }}});
            components[2]=std::make_unique<SD>(std::vector<SD::Stock>{{"exposure",0}},
                std::vector<SD::Flow>{{SD::boundary,0,[](const auto&,const auto& v,double) { return v[0]; }}},std::vector<double>{0},dt*ratio);
            ankurafathom::devs::Simulator<Message> sim;
            std::array<std::size_t,3> ids;
            for(std::size_t index:order) ids[index]=sim.add(std::move(components[index]));
            sim.connect(ids[0],1,ids[1],0);sim.connect(ids[1],1,ids[2],0);
            for(double horizon:plan.at("horizons")) {
                (void)sim.run_until_transactional(horizon);
                const auto& source=dynamic_cast<const Atomic&>(sim.model(ids[0]));
                const auto& consumer=dynamic_cast<const SD&>(sim.model(ids[2]));
                std::vector<double> agents;
                const auto& store=source.core().store();
                for(auto id=store.first_id();id<store.next_id();++id) agents.push_back(std::get<double>(store.field({store.store_id(),id},0)));
                output<<json{{"case",c.at("id")},{"dt",dt},{"ratio",ratio},{"order",order},{"horizon",horizon},
                    {"source_time",source.core().time()},{"consumer_time",consumer.time()},{"revision",source.revision()},
                    {"agents",agents},{"globals",source.core().global_state()},{"signal",consumer.signals()[0]},
                    {"area",consumer.state()[0]}}.dump()<<'\n';
                ++rows;
            }
        }
        std::cout<<rows<<" coupled agent-stock observations generated\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
