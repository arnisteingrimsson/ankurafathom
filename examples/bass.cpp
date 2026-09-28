#include "ankurafathom/abm/models/bass.hpp"
#include <iomanip>
#include <iostream>
int main() {
    try {
        constexpr std::size_t n=160;
        ankurafathom::abm::models::Bass model({.03,.7,.0625},std::vector<std::int64_t>(n,0),{32452843,53,0,1501});
        std::cout<<std::setprecision(17)<<"time,adopted,population,adopted_fraction\n";
        for(int t=0;t<=8;++t) {
            while(model.time()<t) model.step();
            std::cout<<model.time()<<','<<model.adopted()<<','<<n<<','<<static_cast<double>(model.adopted())/n<<'\n';
        }
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
