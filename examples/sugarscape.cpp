#include "ankurafathom/abm/models/sugarscape.hpp"
#include <iostream>
using Model=ankurafathom::abm::models::Sugarscape;
int main() {
    std::vector<std::int64_t> capacity;
    std::vector<Model::State> agents;
    for(std::int64_t y=0;y<6;++y) for(std::int64_t x=0;x<6;++x) {
        capacity.push_back(1+std::max({std::int64_t{0},4-std::abs(x-1)-std::abs(y-1),4-std::abs(x-4)-std::abs(y-4)}));
        if((x+y)%2==0) agents.push_back({4+(x+3*y)%5,1+(x+y)%3,1+(x+2*y)%3,{x,y}});
    }
    Model model({6,6,true,1},capacity,capacity,agents,{15485863,31,0,1201,1202});
    std::cout<<"tick,active,reserve,land,regrown,consumed,deaths\n";
    for(unsigned tick=0;tick<=16;++tick) {
        if(tick) model.step();
        std::int64_t reserve=0,land=0;
        for(auto id:model.active_ids()) reserve+=model.state(id).reserve;
        for(auto amount:model.sugar()) land+=amount;
        const auto l=model.ledger();
        std::cout<<tick<<','<<model.store().active_count()<<','<<reserve<<','<<land<<','<<l.regrown<<','<<l.consumed<<','<<l.deaths<<'\n';
    }
}
