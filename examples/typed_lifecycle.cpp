#include "ankurafathom/des/reference_delay.hpp"
#include "ankurafathom/hybrid/event_to_lifecycle.hpp"
#include <iostream>

struct Person {};struct Onboarding {};
using Token=ankurafathom::des::EntityToken<Onboarding>;
using Message=std::variant<Token,ankurafathom::des::QueuePull,ankurafathom::abm::PopulationCommand<Person>,
    ankurafathom::abm::PopulationResult<Person>,ankurafathom::abm::PopulationTopicInput<Person>,
    ankurafathom::abm::PopulationLifecycleInput<Person>,ankurafathom::abm::PopulationNetworkInput<Person>>;
using Pop=ankurafathom::abm::PopulationAtomic<Person,Message>;
using Bridge=ankurafathom::hybrid::EventToLifecycle<Token,Person,Message>;
int main() {
    Pop::Store staff(8,{{"capacity",ankurafathom::des::FieldKind::real}});
    Pop::Sync runtime(staff);
    runtime.add_phase([](auto ref,const auto& s) { return Pop::Store::Record{std::get<double>(s.field(ref,0))+1}; });
    ankurafathom::devs::Simulator<Message> sim;
    auto delay=sim.add(std::make_unique<ankurafathom::des::ReferenceDelay<Onboarding,Message>>(7,.375));
    auto bridge=sim.add(std::make_unique<Bridge>(staff,[](const Token& t) { return t.entity.id; },[](const Token&,double) {
        return Bridge::Change{{},{{2.}}};
    }));
    auto population=sim.add(std::make_unique<Pop>(std::move(runtime),.5));
    sim.connect(delay,1,bridge,0);sim.connect(bridge,1,population,3);
    sim.inject(0,delay,0,Message{Token{{7,0}}});sim.inject(.125,delay,0,Message{Token{{7,1}}});
    std::cout<<"time,headcount,capacity\n";
    for(double t:{0.,.375,.5,1.}) {
        sim.run_until_transactional(t);
        const auto& store=dynamic_cast<const Pop&>(sim.model(population)).store();
        double capacity=0;
        for(auto id=store.first_id();id<store.next_id();++id) if(store.alive({8,id})) capacity+=std::get<double>(store.field({8,id},0));
        std::cout<<t<<','<<store.active_count()<<','<<capacity<<'\n';
    }
}
