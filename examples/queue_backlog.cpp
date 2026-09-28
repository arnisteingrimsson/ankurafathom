#include "ankurafathom/hybrid/models/queue_backlog.hpp"
#include <iostream>
int main() {
    try {
        ankurafathom::hybrid::models::QueueBacklog model({{0,0,1},{1,0,1},{2,.5,1},{3,2,1}});
        std::cout<<"time,arrivals,completed,waiting,busy,backlog_stock\n";
        for(int tick=0;tick<=8;++tick) {
            const auto x=model.observe(tick*.5);
            std::cout<<x.time<<','<<x.arrivals<<','<<x.completed<<','<<x.waiting<<','<<x.busy<<','<<x.stock<<'\n';
        }
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
