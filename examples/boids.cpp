#include "ankurafathom/abm/models/boids.hpp"
#include <iostream>
#include <iomanip>
int main() {
    using Model=ankurafathom::abm::models::Boids;
    auto model=Model::seeded({},20,{15485863,31,0,1201,1202});
    std::cout<<std::setprecision(17)<<"tick,agent,x,y,vx,vy\n";
    for(unsigned tick=0;tick<=24;++tick) {
        if(tick) model.step();
        const auto states=model.states();
        for(std::size_t i=0;i<states.size();++i) std::cout<<tick<<','<<i<<','<<states[i][0]<<','<<states[i][1]<<','<<states[i][2]<<','<<states[i][3]<<'\n';
    }
}
