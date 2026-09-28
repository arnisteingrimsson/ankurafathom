#include "ankurafathom/hybrid/models/queue_backlog.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Json=nlohmann::json;
using Entity=ankurafathom::des::Entity;
std::vector<Entity> schedule(const Json& plan,const Json& c,std::size_t n,std::uint32_t rep) {
    std::vector<Entity> result;
    const auto initial=static_cast<std::size_t>(c.at("initial_backlog").get<double>()*n);
    const double lambda=n*c.at("arrival_rate").get<double>(),mu=n*c.at("service_rate").get<double>();
    const double horizon=plan.at("times").back();
    double time=0;
    for(std::uint64_t id=0;;++id) {
        if(id>=plan.at("max_entities").get<std::size_t>()) throw std::runtime_error("fluid arrival schedule guard exhausted");
        const auto draw=[&](std::uint32_t stream,double rate) {
            return ankurafathom::rng::exponential(rate,ankurafathom::rng::draw(plan.at("seed"),
                {plan.at("scenario"),rep,id,0,stream,0})[0]);
        };
        if(id>=initial) {
            const auto next=time+draw(plan.at("arrival_stream"),lambda);
            if(!std::isfinite(next) || next<=time) throw std::overflow_error("fluid arrival clock");
            time=next;if(time>horizon)break;
        }
        result.push_back({id,time,draw(plan.at("service_stream"),mu)});
    }
    return result;
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream input(argv[1]);Json plan;input>>plan;
        std::ofstream out(argv[2]);out<<Json{{"plan",plan}}.dump()<<'\n';
        for(const auto& c:plan.at("cases")) {
            const double lambda=c.at("arrival_rate"),mu=c.at("service_rate"),initial=c.at("initial_backlog");
            for(const auto& h:plan.at("sd_steps")) {
                const double dt=h;ankurafathom::sd::Model fluid;
                const auto b=fluid.add_stock("backlog",initial,true,true);
                fluid.add_flow(fluid.boundary,b,[=](const auto&,double) { return lambda; });
                const auto service=fluid.add_flow(b,fluid.boundary,[=](const auto&,double) { return mu; });
                fluid.set_outflow_order(b,{service});
                std::uint64_t tick=0;
                for(const auto& t:plan.at("times")) {
                    while(tick*dt<t.get<double>()) { fluid.step(tick*dt,dt);++tick; }
                    if(tick*dt!=t.get<double>())throw std::logic_error("unaligned fluid SD observation");
                    out<<Json{{"kind","sd"},{"case",c.at("id")},{"dt",dt},{"time",t},{"backlog",fluid.state()[b]}}.dump()<<'\n';
                }
            }
            for(const auto& scale:plan.at("scales")) for(std::uint32_t rep=0;rep<plan.at("replications").get<std::uint32_t>();++rep) {
                const auto n=scale.get<std::size_t>();
                ankurafathom::hybrid::models::QueueBacklog model(schedule(plan,c,n,rep));
                for(const auto& t:plan.at("times")) {
                    const auto x=model.observe(t.get<double>());
                    out<<Json{{"kind","des"},{"case",c.at("id")},{"scale",n},{"replication",rep},{"time",t},
                              {"arrivals",x.arrivals},{"completed",x.completed},{"waiting",x.waiting},{"busy",x.busy},{"stock",x.stock}}.dump()<<'\n';
                }
            }
        }
        if(!out)throw std::runtime_error("fluid output failed");
        std::cout<<"DES/pulse/SD and native reflected-fluid trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
