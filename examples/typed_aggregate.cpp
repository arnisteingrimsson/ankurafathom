#include "ankurafathom/hybrid/population_aggregate.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <iostream>
struct Consultant {};
int main() {
    using Store=ankurafathom::abm::PopulationStore<Consultant>;
    using Aggregate=ankurafathom::hybrid::PopulationAggregate<Consultant>;
    using Reduction=Aggregate::Reduction;
    using Message=ankurafathom::hybrid::AggregateMessage<Consultant>;
    using SD=ankurafathom::hybrid::SignalSD<Message>;
    Store people(1,{{"capacity",ankurafathom::des::FieldKind::real}});
    const auto first=people.spawn({2.});people.spawn({4.});
    ankurafathom::devs::Simulator<Message> sim;
    const auto aggregate=sim.add(std::make_unique<Aggregate>(people,std::vector<Reduction>{
        {"capacity",Reduction::Kind::sum,[](auto ref,const auto& store) { return std::get<double>(store.field(ref,0)); }}}));
    const auto sd=sim.add(std::make_unique<SD>(std::vector<SD::Stock>{{"capacity_time",0}},
        std::vector<SD::Flow>{{SD::boundary,0,[](const auto&,const auto& v,double) { return v.at(0); }}},
        std::vector<double>{0},.25));
    sim.connect(aggregate,1,sd,0);
    sim.inject(0,aggregate,0,Message{Aggregate::Snapshot{0,0,people}});
    people.retire(first);
    sim.inject(.375,aggregate,0,Message{Aggregate::Snapshot{.375,1,people}});
    std::cout<<"time,capacity,capacity_time\n";
    for(int tick=0;tick<=8;++tick) {
        (void)sim.run_until_transactional(tick*.25);
        const auto& model=dynamic_cast<const SD&>(sim.model(sd));
        std::cout<<model.time()<<','<<model.signals()[0]<<','<<model.state()[0]<<'\n';
    }
}
