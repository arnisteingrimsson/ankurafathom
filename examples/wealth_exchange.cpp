#include "ankurafathom/abm/models/wealth.hpp"
#include <iostream>

int main() {
    using Wealth=ankurafathom::abm::models::WealthExchange;
    Wealth model(std::vector<std::int64_t>(24,1),{}, {630127,23,0,1001,1002});
    std::cout<<"sweep,agent,wealth\n";
    for(unsigned sweep=0;sweep<=32;++sweep) {
        if(sweep) model.step();
        const auto wealth=model.wealth();
        for(std::size_t i=0;i<wealth.size();++i) std::cout<<sweep<<','<<i<<','<<wealth[i]<<'\n';
    }
}
