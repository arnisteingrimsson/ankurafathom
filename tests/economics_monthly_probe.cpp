#include "ankurafathom/economics.hpp"
#include <iomanip>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    try {
        std::cout<<"scenario,practice,month,revenue,cost,profit,actual_delivery,paid_hours,headcount,tm_backlog,fixed_backlog,tm_won,fixed_won,tm_delivered,fixed_delivered\n"<<std::setprecision(17);
        for(const auto& practice:ankurafathom::load_practices_csv(argv[1])) for(int flags=0;flags<16;++flags) {
            const auto run=ankurafathom::simulate(practice,static_cast<std::uint8_t>(flags));
            for(std::size_t i=0;i<run.monthly.size();++i) {
                const auto& r=run.monthly[i];
                std::cout<<flags<<','<<practice.name<<','<<i+1<<','<<r.revenue<<','<<r.cost<<','<<r.profit<<','
                    <<r.actual_delivery_hours<<','<<r.paid_hours<<','<<r.fte<<','<<r.tm_backlog_hours<<','<<r.fixed_backlog_hours<<','
                    <<r.tm_won_hours<<','<<r.fixed_won_hours<<','<<r.tm_delivered_hours<<','<<r.fixed_delivered_hours<<'\n';
            }
        }
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
