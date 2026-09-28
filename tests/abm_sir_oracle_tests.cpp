#include "ankurafathom/abm/models/sir.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <numeric>
using Model=ankurafathom::abm::models::SIR;
using Network=ankurafathom::abm::CsrNetwork;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream in(argv[1]); nlohmann::json plan; in>>plan;
        std::ofstream out(argv[2]); out<<nlohmann::json{{"plan",plan}}.dump()<<'\n';
        for(const auto& c:plan.at("cases")) {
            std::vector<std::uint64_t> ids(c.at("states").size()); std::iota(ids.begin(),ids.end(),0);
            const Network network(ids,c.at("edges").get<std::vector<Network::Edge>>());
            const Model::Parameters parameters{c.at("infection_rate"),c.at("recovery_rate"),c.at("dt")};
            for(std::uint32_t r=0;r<plan.at("replications").get<std::uint32_t>();++r) {
                Model model(parameters,c.at("states"),network,
                            {plan.at("seed"),plan.at("scenario"),r,plan.at("infection_stream"),plan.at("recovery_stream")});
                for(const auto& tick:plan.at("ticks")) {
                    while(model.ticks()<tick.get<std::uint32_t>()) model.step();
                    out<<nlohmann::json{{"case",c.at("id")},{"replication",r},{"tick",tick},
                                       {"time",model.time()},{"states",model.states()}}.dump()<<'\n';
                }
            }
        }
        if(!out) throw std::runtime_error("failed to write SIR observations");
        std::cout<<"SIR trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
