#include "ankurafathom/hybrid/agent_stocks_atomic.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <iostream>
struct Consultant {};
int main() {
    using Core=ankurafathom::hybrid::AgentStocks<Consultant>;
    using E=Core::Endpoint;
    using Atomic=ankurafathom::hybrid::AgentStocksAtomic<Consultant>;
    using Aggregate=ankurafathom::hybrid::PopulationAggregate<Consultant>;
    using Reduction=Aggregate::Reduction;
    using Message=ankurafathom::hybrid::AggregateMessage<Consultant>;
    using SD=ankurafathom::hybrid::SignalSD<Message>;
    Core::Store people(1,{{"work",ankurafathom::des::FieldKind::real}});
    people.spawn({2.});people.spawn({4.});
    Core core(std::move(people),{{0}},{{"done",0}});
    core.add_agent_flow(E::agent(0),E::global(0),[](auto ref,const auto& s,const auto&,double) {
        return .5*std::get<double>(s.field(ref,0));
    });
    ankurafathom::devs::Simulator<Message> sim;
    const auto aggregate=sim.add(std::make_unique<Aggregate>(core.store(),std::vector<Reduction>{
        {"remaining_work",Reduction::Kind::sum,[](auto ref,const auto& s) { return std::get<double>(s.field(ref,0)); }}}));
    const auto source=sim.add(std::make_unique<Atomic>(std::move(core),.25));
    const auto consumer=sim.add(std::make_unique<SD>(std::vector<SD::Stock>{{"work_exposure",0}},
        std::vector<SD::Flow>{{SD::boundary,0,[](const auto&,const auto& v,double) { return v[0]; }}},std::vector<double>{0},.5));
    sim.connect(source,1,aggregate,0);sim.connect(aggregate,1,consumer,0);
    std::cout<<"time,remaining_work,completed_work,work_exposure\n";
    for(int tick=0;tick<=16;++tick) {
        (void)sim.run_until_transactional(tick*.25);
        const auto& agents=dynamic_cast<const Atomic&>(sim.model(source));
        const auto& sd=dynamic_cast<const SD&>(sim.model(consumer));
        std::cout<<agents.core().time()<<','<<agents.core().sum(0)<<','<<agents.core().global_state()[0]<<','<<sd.state()[0]<<'\n';
    }
}
