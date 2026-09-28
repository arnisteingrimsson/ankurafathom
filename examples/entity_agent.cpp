#include "ankurafathom/hybrid/entity_agent.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <iostream>

struct Engagement {};
using Owner=ankurafathom::hybrid::EntityAgentAtomic<Engagement>;
using Message=ankurafathom::hybrid::EntityAgentMessage<Engagement>;
using Pop=Owner::Population;
int main() {
    Pop::Store records(7,{{"work",ankurafathom::des::FieldKind::real},{"duration",ankurafathom::des::FieldKind::real}});
    records.spawn({0.,.5});records.spawn({10.,1.});
    Pop::Async behavior(records,[](const auto& timer,const auto& store) {
        Pop::Effects e;auto record=store.record(timer.agent);std::get<double>(record[0])+=1;
        e.updates.push_back({timer.agent,record});return e;
    });
    behavior.schedule({.25,{7,0},"progress",0});behavior.schedule({.25,{7,1},"progress",0});
    Pop population(std::move(behavior),[](const Pop::Command& command,const auto& store) {
        if(command.kind!="complete") throw std::invalid_argument("unknown command");
        Pop::Effects e;auto record=store.record(command.agent);std::get<double>(record[0])+=100;
        e.updates.push_back({command.agent,record});return e;
    });
    auto owner=std::make_unique<Owner>(std::move(population),[](auto,const auto&,double) { return true; },"complete");
    ankurafathom::devs::Simulator<Message> sim;
    auto id=sim.add(std::move(owner));
    auto delay=sim.add(std::make_unique<ankurafathom::des::ReferenceDelay<Engagement,Message>>(7,
        [](const Owner::Token& token) { return std::get<double>(token.snapshot->record[1]); }));
    sim.connect(id,Owner::process_port,delay,0);sim.connect(delay,1,id,Owner::return_port);
    std::cout<<"time,in_flight,returned,agent_0_work,agent_1_work\n";
    for(double t:{0.,.25,.5,1.}) {
        sim.run_until_transactional(t);const auto& live=dynamic_cast<const Owner&>(sim.model(id));
        std::cout<<t<<','<<live.in_flight_count()<<','<<live.returned_count()<<','
                 <<std::get<double>(live.store().field({7,0},0))<<','<<std::get<double>(live.store().field({7,1},0))<<'\n';
    }
}
