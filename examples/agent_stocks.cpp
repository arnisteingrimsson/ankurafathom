#include "ankurafathom/hybrid/agent_stocks.hpp"
#include <iostream>
struct Consultant {};
int main() {
    using Stocks=ankurafathom::hybrid::AgentStocks<Consultant>;
    using E=Stocks::Endpoint;
    Stocks::Store people(1,{{"remaining_work",ankurafathom::des::FieldKind::real}});
    people.spawn({2.});people.spawn({4.});
    Stocks model(std::move(people),{{0}},{{"completed_work",0},{"work_exposure",0}});
    model.add_agent_flow(E::agent(0),E::global(0),[](auto person,const auto& snapshot,const auto&,double) {
        return .5*std::get<double>(snapshot.field(person,0));
    });
    model.add_global_flow(E::boundary(),E::global(1),[](const auto& snapshot,const auto&,double) {
        return Stocks::sum(snapshot,0);
    });
    std::cout<<"time,remaining_work,completed_work,work_exposure\n";
    for(int tick=0;tick<=16;++tick) {
        std::cout<<model.time()<<','<<model.sum(0)<<','<<model.global_state()[0]<<','<<model.global_state()[1]<<'\n';
        if(tick<16) model.step(.25);
    }
}
