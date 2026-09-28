#include "ankurafathom/abm/models/sir_async.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <numeric>
using Async=ankurafathom::abm::models::AsyncSIR;
using Sync=ankurafathom::abm::models::SIR;
using Network=ankurafathom::abm::CsrNetwork;
using Json=nlohmann::json;
Network network(const Json& c) {
    std::vector<std::uint64_t> ids(c.at("states").size()); std::iota(ids.begin(),ids.end(),0);
    return Network(ids,c.at("edges").get<std::vector<Network::Edge>>());
}
int main(int argc,char** argv) {
    try {
        if(argc!=4) throw std::invalid_argument("expected plan and two output paths");
        std::ifstream in(argv[1]); Json p; in>>p;
        std::ofstream out(argv[2]),conv(argv[3]);
        out<<Json{{"plan",p}}.dump()<<'\n'; conv<<Json{{"plan",p}}.dump()<<'\n';
        for(const auto& c:p.at("cases")) for(std::uint32_t r=0;r<p.at("replications").get<std::uint32_t>();++r) {
            Async m({c.at("infection_rate"),c.at("recovery_rate")},c.at("states"),network(c),
                    {p.at("seed"),p.at("scenario"),r,p.at("waiting_stream"),p.at("selection_stream")});
            auto events=Json::array();
            for(const auto& t:p.at("times")) {
                for(const auto& e:m.run_until(t.get<double>())) events.push_back({e.time,e.agent,e.before,e.after,e.generation});
                out<<Json{{"case",c.at("id")},{"replication",r},{"time",t},{"states",m.states()},{"events",events}}.dump()<<'\n';
            }
        }
        const auto cp=p.at("convergence");
        for(const auto& c:cp.at("cases")) for(std::uint32_t r=0;r<cp.at("replications").get<std::uint32_t>();++r) {
            Async a({c.at("infection_rate"),c.at("recovery_rate")},c.at("states"),network(c),
                    {cp.at("seed"),cp.at("scenario"),r,p.at("waiting_stream"),p.at("selection_stream")});
            for(const auto& t:cp.at("times")) {
                a.run_until(t.get<double>());
                conv<<Json{{"case",c.at("id")},{"replication",r},{"time",t},{"dt",0},{"states",a.states()}}.dump()<<'\n';
            }
            for(const auto& dt:cp.at("steps")) {
                Sync s({c.at("infection_rate"),c.at("recovery_rate"),dt},c.at("states"),network(c),
                       {cp.at("seed"),cp.at("scenario"),r,cp.at("infection_stream"),cp.at("recovery_stream")});
                for(const auto& t:cp.at("times")) {
                    while(s.time()<t.get<double>()) s.step();
                    if(s.time()!=t.get<double>()) throw std::logic_error("convergence observation not grid aligned");
                    conv<<Json{{"case",c.at("id")},{"replication",r},{"time",t},{"dt",dt},{"states",s.states()}}.dump()<<'\n';
                }
            }
        }
        if(!out || !conv) throw std::runtime_error("failed to write async SIR evidence");
        std::cout<<"Async SIR and convergence trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
