#include "ankurafathom/abm/models/sir_async.hpp"
#include "ankurafathom/sd/model.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Json=nlohmann::json;
using ABM=ankurafathom::abm::models::AsyncSIR;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream in(argv[1]); Json p; in>>p;
        std::ofstream out(argv[2]); out<<Json{{"plan",p}}.dump()<<'\n';
        for(const auto& c:p.at("cases")) {
            const double beta=c.at("beta"),gamma=c.at("gamma");
            for(const auto& dt_json:p.at("sd_steps")) {
                const double dt=dt_json;
                ankurafathom::sd::Model sd;
                const double i0=1./p.at("initial_infected_denominator").get<double>();
                const auto s=sd.add_stock("susceptible",1-i0),i=sd.add_stock("infected",i0),r=sd.add_stock("recovered",0);
                sd.add_flow(s,i,[=](const auto& x,double) { return beta*x[s]*x[i]; });
                sd.add_flow(i,r,[=](const auto& x,double) { return gamma*x[i]; });
                std::uint64_t steps=0;
                for(const auto& t:p.at("times")) {
                    while(steps*dt<t.get<double>()) { sd.step(steps*dt,dt,ankurafathom::sd::Integrator::rk4); ++steps; }
                    if(steps*dt!=t.get<double>()) throw std::logic_error("SD observation not aligned");
                    out<<Json{{"kind","sd"},{"case",c.at("id")},{"dt",dt},{"time",t},{"fractions",sd.state()}}.dump()<<'\n';
                }
            }
            for(const auto& n_json:p.at("populations")) {
                const auto n=n_json.get<std::size_t>();
                std::vector<std::int64_t> initial(n,0);
                for(std::size_t i=0;i<n/p.at("initial_infected_denominator").get<std::size_t>();++i) initial[i]=1;
                for(std::uint32_t rep=0;rep<p.at("replications").get<std::uint32_t>();++rep) {
                    auto model=ABM::well_mixed({beta,gamma},initial,
                        {p.at("seed"),p.at("scenario"),rep,p.at("waiting_stream"),p.at("selection_stream")});
                    for(const auto& t:p.at("times")) {
                        model.run_until(t.get<double>());
                        const auto counts=model.counts();
                        out<<Json{{"kind","abm"},{"case",c.at("id")},{"population",n},{"replication",rep},
                                  {"time",t},{"counts",{counts.susceptible,counts.infected,counts.recovered}},
                                  {"events",model.events()}}.dump()<<'\n';
                    }
                }
            }
        }
        if(!out) throw std::runtime_error("failed to write SIR mean-field output");
        std::cout<<"SIR mean-field native trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
