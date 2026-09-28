#include "ankurafathom/abm/models/bass.hpp"
#include "ankurafathom/sd/model.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Json=nlohmann::json;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream input(argv[1]);Json plan;input>>plan;
        std::ofstream out(argv[2]);out<<Json{{"plan",plan}}.dump()<<'\n';
        for(const auto& c:plan.at("cases")) {
            const double p=c.at("p"),q=c.at("q"),a0=c.at("initial_fraction");
            for(const auto& h:plan.at("sd_steps")) for(const auto method:{"euler","rk4"}) {
                const double dt=h;ankurafathom::sd::Model sd;
                const auto a=sd.add_stock("adoption",a0);
                sd.add_flow(ankurafathom::sd::Model::boundary,a,[=](const auto& x,double) { return (p+q*x[a])*(1-x[a]); });
                std::uint64_t tick=0;
                for(const auto& t:plan.at("times")) {
                    while(tick*dt<t.get<double>()) {
                        sd.step(tick*dt,dt,std::string(method)=="euler" ? ankurafathom::sd::Integrator::euler : ankurafathom::sd::Integrator::rk4);++tick;
                    }
                    if(tick*dt!=t.get<double>()) throw std::logic_error("unaligned Bass SD observation");
                    out<<Json{{"kind","sd"},{"case",c.at("id")},{"dt",dt},{"method",method},{"time",t},{"fraction",sd.state()[a]}}.dump()<<'\n';
                }
            }
            for(const auto& h:plan.at("ensemble_steps")) for(const auto& size:plan.at("populations")) {
                const double dt=h;const auto n=size.get<std::size_t>();
                std::vector<std::int64_t> initial(n,0);
                for(std::size_t i=0;i<static_cast<std::size_t>(a0*n);++i) initial[i]=1;
                for(std::uint32_t rep=0;rep<plan.at("replications").get<std::uint32_t>();++rep) {
                    ankurafathom::abm::models::Bass model({p,q,dt},initial,
                        {plan.at("seed"),plan.at("scenario"),rep,plan.at("stream")});
                    for(const auto& t:plan.at("times")) {
                        while(model.time()<t.get<double>()) model.step();
                        if(model.time()!=t.get<double>()) throw std::logic_error("unaligned Bass ABM observation");
                        out<<Json{{"kind","abm"},{"case",c.at("id")},{"dt",dt},{"population",n},{"replication",rep},{"time",t},
                                  {"adopted",model.adopted()},{"ticks",model.ticks()}}.dump()<<'\n';
                    }
                }
            }
        }
        if(!out) throw std::runtime_error("Bass output failed");
        std::cout<<"Bass native SD and individual ABM trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
