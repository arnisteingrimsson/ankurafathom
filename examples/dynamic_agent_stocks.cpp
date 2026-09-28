#include "ankurafathom/hybrid/dynamic_agent_stocks.hpp"
#include <iostream>

struct Consultant {};
namespace af=ankurafathom;
using Core=af::hybrid::AgentStocks<Consultant>;
using Owner=af::hybrid::DynamicAgentStocksAtomic<Consultant>;
using Message=af::hybrid::DynamicAgentStockMessage<Consultant>;
using Input=af::hybrid::AgentStockInput<Consultant>;
int main() {
    using K=af::des::FieldKind;using E=Core::Endpoint;
    Core::Store staff(1,{{"experience",K::real},{"learning_rate",K::real}});
    staff.spawn({2.,1.});
    Core core(staff,{{0,true}},{{"shared_knowledge",0,true}});
    core.add_agent_flow(E::boundary(),E::agent(0),[](auto ref,const auto& store,const auto&,double) { return std::get<double>(store.field(ref,1)); });
    af::devs::Simulator<Message> sim;const auto owner=sim.add(std::make_unique<Owner>(std::move(core),.5,std::vector<std::string>{"workforce","knowledge"}));
    Core::Change hiring;hiring.births={{1.,2.}};
    sim.inject(.25,owner,0,Message{Input{.25,"workforce",0,hiring}});
    Core::Change departure;departure.retirements={{1,0}};
    sim.inject(1,owner,0,Message{Input{1,"workforce",1,departure}});
    Core::Change knowledge;knowledge.pulses={{E::global(0),{},2},{E::agent(0),Core::Reference{1,1},1}};
    sim.inject(1.25,owner,0,Message{Input{1.25,"knowledge",0,knowledge}});
    std::cout<<"horizon,committed_time,headcount,experience,shared_knowledge\n";
    for(const double horizon:{0.,.25,.5,.75,1.,1.25,1.5,2.}) {
        for(const auto& step:sim.run_until_transactional(horizon)) for(const auto& event:step.emissions)
            if(event.source==owner && event.port==Owner::commit_port) {
                const auto& commit=std::get<af::hybrid::AgentStockCommit<Consultant>>(event.value);
                if(commit.discrete && !commit.discrete->retired.empty())
                    std::cerr<<"Retirement at "<<commit.time<<" exports "<<commit.discrete->retired_amounts[0]<<" experience units\n";
            }
        const auto& current=dynamic_cast<const Owner&>(sim.model(owner)).core();
        std::cout<<horizon<<','<<current.time()<<','<<current.store().active_count()<<','<<current.sum(0)<<','<<current.global_state()[0]<<'\n';
    }
}
