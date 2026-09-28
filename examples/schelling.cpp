#include "ankurafathom/abm/models/schelling.hpp"
#include <iostream>
int main() {
    using Model=ankurafathom::abm::models::Schelling;
    std::vector<Model::State> agents;
    for(std::int64_t y=0;y<6;++y) for(std::int64_t x=0;x<6;++x)
        if((x+2*y)%3) agents.push_back({(x*7+y*11+x*y)%5<2,{x,y}});
    Model model({6,6,true,true,1,2},agents,{873109,29,0,1101,1102});
    std::cout<<"sweep,agent,group,x,y,satisfied\n";
    for(unsigned tick=0;tick<=16;++tick) {
        if(tick) model.step();
        const auto state=model.states();
        for(std::size_t i=0;i<state.size();++i)
            std::cout<<tick<<','<<i<<','<<state[i].group<<','<<state[i].position.x<<','<<state[i].position.y<<','<<model.satisfied(i)<<'\n';
    }
}
